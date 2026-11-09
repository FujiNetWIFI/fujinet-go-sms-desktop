/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running SMS
 * session (the CONFIG client, no BIOS, FujiNet off) whose console RAM holds
 * a hand-assembled loop written through the debugger: the tabs,
 * attach/detach with show and hide, F12, the stepping keys and buttons, the
 * console (transcript, Tab completion, history), the disassembly (the PC's
 * accent bar, click / double-click / F9 for a breakpoint, selection and Run
 * to Cursor, the wheel, the jump box), the register and memory entries, the
 * Breakpoints tab (every type), Watches, search, Trace and History, Symbols,
 * Break at, pop-out and dock, the Video tab, and F12 and the View menu from
 * the main window.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1: in the
 * light scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QOpenGLContext>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QMenu>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "smsdebug.h"
#include "smsmemtools.h"
#include "smssession.h"
#include "DisplayWidget.h"
#include "MainWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

/* The test subject, in console RAM at $C000 (interrupts off):
 *   C000  LD SP,$DF00
 *   C003  LD HL,$C100
 *   C006  XOR A
 *   C007  LD B,3
 *   C009  CALL $C050     ; INC C / RET
 *   C00C  DJNZ $C009
 *   C00E  INC A
 *   C00F  LD (HL),A      ; write $C100
 *   C010  OUT ($7F),A    ; the PSG
 *   C012  IN A,($DC)     ; the pads
 *   C014  LD A,(HL)      ; read $C100
 *   C015  JP $C006 */
const uint8_t kProg[] = {
    0x31, 0x00, 0xDF, 0x21, 0x00, 0xC1, 0xAF, 0x06, 0x03,
    0xCD, 0x50, 0xC0, 0x10, 0xFB, 0x3C, 0x77, 0xD3, 0x7F,
    0xDB, 0xDC, 0x7E, 0xC3, 0x06, 0xC0,
};
const uint8_t kSub[] = { 0x0C, 0xC9 };

/* The accent: the SMS icon's red. */
const QColor kAccent(0xe4, 0x00, 0x2b);

/* With the machine stopped: the program in place, PC at its LD B,3. */
void loadProgram(smsdebug *d)
{
    for (size_t i = 0; i < sizeof kProg; i++) smsdebug_write(d, (uint16_t)(0xC000 + i), kProg[i]);
    for (size_t i = 0; i < sizeof kSub; i++) smsdebug_write(d, (uint16_t)(0xC050 + i), kSub[i]);
    smsdebug_cpu_set(d, SMS_REG_IFF1, 0);
    smsdebug_cpu_set(d, SMS_REG_IFF2, 0);
    smsdebug_cpu_set(d, SMS_REG_SP, 0xDF00);
    smsdebug_cpu_set(d, SMS_REG_HL, 0xC100);
    smsdebug_cpu_set(d, SMS_REG_PC, 0xC007);
    smsdebug_cpu_set(d, SMS_REG_B, 3);
}

int imOf(smsdebug *d)
{
    smsdebug_cpu c;
    smsdebug_cpu_get(d, &c);
    return c.im;
}

int pcOf(smsdebug *d)
{
    smsdebug_cpu c;
    smsdebug_cpu_get(d, &c);
    return c.pc;
}

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    smssession *m_s = nullptr;
    smsdebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    smssession_start_opts m_opts = {};

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *b : m_win->findChildren<QPushButton *>())
            if (b->text().startsWith(textStart)) return b;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText().startsWith(placeholderStart)) return e;
        return nullptr;
    }
    QLineEdit *entryIs(const QString &placeholder)
    {
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->placeholderText() == placeholder) return e;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return v;
        return nullptr;
    }
    QPlainTextEdit *consoleView()
    {
        for (QPlainTextEdit *v : m_win->findChildren<QPlainTextEdit *>())
            if (v->lineWrapMode() == QPlainTextEdit::WidgetWidth) return v;
        return nullptr;
    }
    QLabel *statusLabel()
    {
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped")) ||
                l->text() == QLatin1String("Running"))
                return l;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *c : m_win->findChildren<QCheckBox *>())
            if (c->text() == text) return c;
        return nullptr;
    }
    QTableWidget *bpTable() { return m_tabs->widget(3)->findChild<QTableWidget *>(); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d), 3000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() &&
                                     statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an address, or -1. */
    int lineOf(QPlainTextEdit *v, int addr)
    {
        const QString want = QStringLiteral("%1").arg(addr, 4, 16, QLatin1Char('0')).toUpper();
        int n = 0;
        for (QTextBlock b = v->document()->begin(); b.isValid(); b = b.next(), ++n)
            if (b.text().mid(2, 4) == want) return n;
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *v, int line, int column)
    {
        QTextCursor c(v->document()->findBlockByNumber(line));
        c.setPosition(c.block().position() + column);
        const QRect r = v->cursorRect(c);
        return QPoint(r.left() + 1, r.center().y());
    }
    void clickLine(int addr, int column, bool dbl = false)
    {
        QPlainTextEdit *v = disasmView();
        const int line = lineOf(v, addr);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(addr, 0, 16)));
        const QPoint p = lineAt(v, line, column);
        if (dbl) QTest::mouseDClick(v->viewport(), Qt::LeftButton, {}, p);
        else QTest::mouseClick(v->viewport(), Qt::LeftButton, {}, p);
    }
    void typeInto(QLineEdit *e, const QString &text)
    {
        e->clear();
        QTest::keyClicks(e, text);
        QTest::keyClick(e, Qt::Key_Return);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
        smssession_paths paths = {};
        const QByteArray dir = m_dir.path().toUtf8();
        paths.config_dir = dir.constData();
        paths.data_dir = dir.constData();
        paths.fujinet_lib = "";
        m_s = smssession_new(&paths);
        QVERIFY(m_s);
        m_d = smssession_debugger(m_s);
        QVERIFY(m_d);

        smssession_default_opts(m_s, &m_opts);
        m_opts.cart_path = nullptr;
        m_opts.bios = nullptr;
        m_opts.enable_fujinet = 0;
        m_opts.enable_audio = 0;
        m_opts.enable_gamepad = 0;
        QCOMPARE(smssession_start(m_s, &m_opts), 0);

        /* the window opens on the tab SMS_DEBUGGER_TAB names, and SMS_WATCH,
         * SMS_WATCH_BREAK and SMS_MEM_GOTO are read at creation */
        qputenv("SMS_DEBUGGER_TAB", "i/o");
        qputenv("SMS_WATCH", "$C110, $C120-$C12F");
        qputenv("SMS_WATCH_BREAK", "$C130");
        qputenv("SMS_MEM_GOTO", "$C140-$C14F");
        DebuggerWindow::showFor(nullptr, m_s);
        qunsetenv("SMS_DEBUGGER_TAB");
        qunsetenv("SMS_WATCH");
        qunsetenv("SMS_WATCH_BREAK");
        qunsetenv("SMS_MEM_GOTO");
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->objectName() == QLatin1String("debugger")) m_win = qobject_cast<QMainWindow *>(w);
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), 5);
        QCOMPARE(smswatch_count(), 3);                      /* two watches and the break-on-change one */
        smsdebug_breakpoint list[4];
        QCOMPARE(smsdebug_breakpoint_list(m_d, list, 4), 1);
        QCOMPARE((int)list[0].start, 0xC130);
        QVERIFY(QString::fromUtf8(list[0].condition).contains(QStringLiteral("old")));
        smswatch_clear();
        m_tabs->setCurrentIndex(0);
        waitStopped();
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        if (m_s) {
            smssession_stop(m_s);
            smssession_free(m_s);
        }
    }

    void init()
    {
        if (!m_win->isVisible()) DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_attached(m_d), 2000);
        if (!smsdebug_is_stopped(m_d)) smsdebug_stop(m_d);
        waitStopped();
        smsdebug_breakpoint_clear(m_d);
        loadProgram(m_d);
    }

    void tabsAndAttach()
    {
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names, (QStringList{ QStringLiteral("Console"), QStringLiteral("Disassembly"),
                                      QStringLiteral("Memory"), QStringLiteral("Breakpoints"),
                                      QStringLiteral("Watches"), QStringLiteral("Cart && I/O"),
                                      QStringLiteral("Trace"), QStringLiteral("History"),
                                      QStringLiteral("Video"), QStringLiteral("Sound"),
                                      QStringLiteral("Symbols"), QStringLiteral("FujiNet") }));
        QVERIFY(smsdebug_is_attached(m_d));
        QVERIFY(smsdebug_is_stopped(m_d));
        /* Run is the accent while stopped, whichever text it wears, in one size */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->isChecked());
        QVERIFY(run->styleSheet().contains(kAccent.name()));
        const QSize size = run->size();
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!smsdebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(!run->isChecked());
        QCOMPARE(run->size(), size);
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->isChecked());
        QCOMPARE(run->size(), size);
        /* the toolbar has the family's buttons */
        for (const char *label : { "Step (F7)", "Step Over", "Step Out", "Scanline+1", "Frame+1", "Clear Breakpoints",
                                   "Load Symbols", "Clear Symbols", "Save", "Pop Out" })
            QVERIFY2(button(QString::fromUtf8(label)), label);
    }

    void stepKeys()
    {
        QCOMPARE(pcOf(m_d), 0xC007);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC009, 2000);
        QCOMPARE(statusLabel()->text().left(13), QStringLiteral("Stopped: step"));
        /* F8 steps over the CALL */
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC00C, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step over")));
        /* the buttons do the same */
        smsdebug_write(m_d, 0xDEFE, 0x0C);          /* a return address, as a CALL left it */
        smsdebug_write(m_d, 0xDEFF, 0xC0);
        smsdebug_cpu_set(m_d, SMS_REG_SP, 0xDEFE);
        smsdebug_cpu_set(m_d, SMS_REG_PC, 0xC050);
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC00C, 2000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("step out")));
        /* Frame: Shift+F7 lands on a frame boundary */
        smsdebug_cpu c, c2;
        smsdebug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F7, Qt::ShiftModifier);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d) &&
                                     (smsdebug_cpu_get(m_d, &c2), c2.frame > c.frame), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("frame")), 2000);
        /* Scanline+1 */
        smsdebug_cpu_get(m_d, &c);
        QTest::mouseClick(button(QStringLiteral("Scanline+1")), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d) &&
                                     (smsdebug_cpu_get(m_d, &c2), c2.vpos != c.vpos || c2.frame != c.frame), 3000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!smsdebug_is_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    /* The stop reason is the status line, and it is never cut: the label has
     * the width its whole text needs at the default window width. */
    void stopReasonNotElided()
    {
        smsdebug_breakpoint_add(m_d, SMSDEBUG_BP_WRITE, 0xC100, 0xC100, "value != old");
        smsdebug_write(m_d, 0xC100, 0x77);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("wrote $C100")), 2000);
        QLabel *status = statusLabel();
        QVERIFY(status->text().contains(QLatin1String("(was $77)")));
        QVERIFY2(status->width() >= status->fontMetrics().horizontalAdvance(status->text()),
                 qPrintable(QStringLiteral("status %1 wide for %2").arg(status->width())
                                .arg(status->fontMetrics().horizontalAdvance(status->text()))));
        QVERIFY(!status->text().contains(QChar(0x2026)));
        shot(QStringLiteral("stopreason"));
    }

    void consoleCommands()
    {
        m_tabs->setCurrentIndex(0);
        QLineEdit *in = entry(QStringLiteral("command"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(in && out);
        QCOMPARE(m_win->focusWidget(), in);         /* typing goes straight in */
        typeInto(in, QStringLiteral("regs"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("> regs")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("flags")));
        QVERIFY(in->text().isEmpty());
        /* a command that moves the machine moves the window */
        typeInto(in, QStringLiteral("set pc $C003"));
        QCOMPARE(pcOf(m_d), 0xC003);
        typeInto(in, QStringLiteral("break 49161"));    /* decimal, the engine's rule: $C009 */
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC009));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint: exec $C009")), 3000);
        waitStopped();
        QCOMPARE(pcOf(m_d), 0xC009);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint: exec $C009")));
        /* Tab completes a command, lists several, and Up recalls */
        in->clear();
        QTest::keyClicks(in, QStringLiteral("reg"));
        QTest::keyClick(in, Qt::Key_Tab);
        QCOMPARE(in->text(), QStringLiteral("regs "));
        in->clear();
        QTest::keyClicks(in, QStringLiteral("b"));
        QTest::keyClick(in, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("break")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(in->text().startsWith(QLatin1Char('b')));
        in->clear();
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("run"));
        QTest::keyClick(in, Qt::Key_Up);
        QCOMPARE(in->text(), QStringLiteral("break 49161"));
        QTest::keyClick(in, Qt::Key_Down);
        QCOMPARE(in->text(), QStringLiteral("run"));
        in->clear();
        shot(QStringLiteral("console"));
    }

    /* Load Symbols > Next to the Cartridge (no cartridge file: said, not
     * silent), a .sym file loaded through the engine shows in the views, and
     * Break at takes a symbol or a hex address. */
    void symbolsAndBreakAt()
    {
        m_tabs->setCurrentIndex(0);
        QAction *next = nullptr;
        for (QAction *a : m_win->findChildren<QAction *>())
            if (a->text() == QLatin1String("Next to the Cartridge")) next = a;
        QVERIFY(next);
        const int before = consoleView()->document()->blockCount();
        next->trigger();
        QVERIFY(consoleView()->document()->blockCount() > before);
        QCOMPARE(smsdebug_label_address(m_d, "afterwards"), -1);
        QFile sym(m_dir.path() + QStringLiteral("/loop.sym"));
        QVERIFY(sym.open(QIODevice::WriteOnly));
        sym.write("C00C afterwards\nC050 helper\n");
        sym.close();
        QLineEdit *in = entry(QStringLiteral("command"));
        typeInto(in, QStringLiteral("labels ") + sym.fileName());
        QCOMPARE(smsdebug_label_address(m_d, "afterwards"), 0xC00C);

        /* Break at: a symbol, then a hex address, then a repeat */
        QLineEdit *breakAt = entry(QStringLiteral("symbol or"));
        QVERIFY(breakAt);
        typeInto(breakAt, QStringLiteral("afterwards"));
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC00C));
        QVERIFY(breakAt->text().isEmpty());
        typeInto(breakAt, QStringLiteral("C00E"));
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC00E));
        typeInto(breakAt, QStringLiteral("C00E"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("already")));
        smsdebug_breakpoint list[4];
        QCOMPARE(smsdebug_breakpoint_list(m_d, list, 4), 2);
        typeInto(breakAt, QStringLiteral("nosuchsymbol"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Not a number")));
        smsdebug_breakpoint_clear(m_d);

        /* the stop reason names where it is */
        typeInto(in, QStringLiteral("break afterwards"));
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC00C));
        typeInto(in, QStringLiteral("run"));
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d) && pcOf(m_d) == 0xC00C, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("(afterwards)")), 2000);

        /* the Symbols tab lists them, filtered; activating a row opens the Disassembly there */
        m_tabs->setCurrentIndex(10);
        QTableWidget *table = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *filter = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(table && filter);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 2, 2000);
        shot(QStringLiteral("symbols"));
        const int all = table->rowCount();
        filter->setText(QStringLiteral("AFTERW"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("afterwards"));
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("$C00C"));
        table->setCurrentCell(0, 0);
        QTest::keyClick(table, Qt::Key_Return);             /* activating a row */
        QCOMPARE(m_tabs->currentIndex(), 1);
        QVERIFY(!check(QStringLiteral("Follow PC"))->isChecked());
        QVERIFY(lineOf(disasmView(), 0xC00C) >= 0);
        m_tabs->setCurrentIndex(10);
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), all, 2000);

        /* Clear Symbols drops the loaded ones */
        QTest::mouseClick(button(QStringLiteral("Clear Symbols")), Qt::LeftButton);
        QCOMPARE(smsdebug_label_address(m_d, "afterwards"), -1);
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() < all, 2000);
        m_tabs->setCurrentIndex(0);
    }

    void disassemblyHighlights()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QVERIFY(v);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC007) >= 0, 2000);
        const int pcLine = lineOf(v, 0xC007);
        QCOMPARE(v->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow-PC keeps it about a third of the way down */
        const int rows = v->document()->blockCount();
        QVERIFY(pcLine >= rows / 3 - 2 && pcLine <= rows / 3 + 2);
        /* the PC's line: the accent as the block's own background, so it lies under the gutter */
        const QTextBlock pcBlock = v->document()->findBlockByNumber(pcLine);
        QCOMPARE(pcBlock.blockFormat().background().color(), kAccent);
        for (const auto &s : v->extraSelections())
            QVERIFY(!s.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(v->document()->findBlockByNumber(pcLine + 1).blockFormat().background().style(), Qt::NoBrush);

        /* a breakpoint's dot: filled and red, hollow and grey when disabled */
        smsdebug_breakpoint_toggle(m_d, 0xC009);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0xC009)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor c;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == line && s.cursor.hasSelection() &&
                    s.cursor.selectionStart() == s.cursor.block().position() &&
                    s.cursor.selectionEnd() == s.cursor.block().position() + 1)
                    c = s.format.foreground().color();
            return c;
        };
        QCOMPARE(dotColor(lineOf(v, 0xC009)), QColor(0xe0, 0x1b, 0x24));
        smsdebug_breakpoint b;
        QCOMPARE(smsdebug_breakpoint_list(m_d, &b, 1), 1);
        smsdebug_breakpoint_enable(m_d, b.id, 0);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0xC009)).text().startsWith(
                                     QStringLiteral("○")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0xC009)), QColor(0x9a, 0x99, 0x96));
        /* a breakpoint on the PC's own line keeps its dot, in white on the bar */
        smsdebug_breakpoint_toggle(m_d, 0xC007);
        QTRY_VERIFY_WITH_TIMEOUT(v->document()->findBlockByNumber(lineOf(v, 0xC007)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        QCOMPARE(dotColor(lineOf(v, 0xC007)), QColor(Qt::white));
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC00C) >= 0, 2000);
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(0xC00C, 30);
        QVERIFY(!smsdebug_breakpoint_check(m_d, 0xC00C));
        {
            bool underlined = false;
            for (const auto &s : v->extraSelections())
                if (s.cursor.blockNumber() == lineOf(v, 0xC00C) && s.format.fontUnderline() &&
                    s.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        /* moving to another line moves the selection; no caret-toggled breakpoints */
        clickLine(0xC00E, 30);
        QVERIFY(!smsdebug_breakpoint_check(m_d, 0xC00C));
        QVERIFY(!smsdebug_breakpoint_check(m_d, 0xC00E));
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(0xC00F, 0);
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC00F));
        clickLine(0xC00F, 0);
        QVERIFY(!smsdebug_breakpoint_check(m_d, 0xC00F));
        clickLine(0xC010, 30, true);
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC010));
        smsdebug_breakpoint_toggle(m_d, 0xC010);
        clickLine(0xC012, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC012));
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(!smsdebug_breakpoint_check(m_d, 0xC012));
        smsdebug_breakpoint list[4];
        QCOMPARE(smsdebug_breakpoint_list(m_d, list, 4), 0);
    }

    void runToCursor()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC00F) >= 0, 2000);
        clickLine(0xC00F, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC00F, 3000);
        QVERIFY(statusLabel()->text().contains(QLatin1String("run to")));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC00F) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        m_tabs->setCurrentIndex(1);
        QPlainTextEdit *v = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC007) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        QVERIFY(lineOf(v, 0xC007) < 0 || lineOf(v, 0xC007) < v->document()->blockCount() / 3 - 1);
        /* the jump box: bare digits are hex here, a label wins first, then expressions */
        QLineEdit *jump = entry(QStringLiteral("$address"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("C050"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC050) >= 0, 2000);
        smsdebug_set_label(m_d, 0xC00C, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC00C) >= 0, 2000);
        QVERIFY(v->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("$C050+1"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC051) >= 0, 2000);
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow PC again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC007) >= 0, 2000);
        smsdebug_set_label(m_d, 0xC00C, nullptr);
    }

    void registersFlagsMemory()
    {
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QList<QLineEdit *> regs;
        for (QLineEdit *e : m_win->findChildren<QLineEdit *>())
            if (e->toolTip().startsWith(QLatin1String("Enter sets it"))) regs << e;
        QCOMPARE(regs.size(), 15);                  /* PC..IY, AF'..HL', I, R, IM; IFF1/2 are the boxes */
        QTRY_COMPARE_WITH_TIMEOUT(regs[0]->text(), QStringLiteral("C007"), 2000);   /* PC */
        QCOMPARE(regs[14]->text(), QString::number(imOf(m_d)));
        typeInto(regs[0], QStringLiteral("C00C"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(m_d), 0xC00C, 2000);
        typeInto(regs[8], QStringLiteral("1234"));  /* AF' */
        smsdebug_cpu c;
        smsdebug_cpu_get(m_d, &c);
        QCOMPARE(c.af2, 0x1234);
        typeInto(regs[6], QStringLiteral("$C050+2"));   /* IX, an expression */
        smsdebug_cpu_get(m_d, &c);
        QCOMPARE(c.ix, 0xC052);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("C"));
        QVERIFY(carry);
        smsdebug_cpu_set(m_d, SMS_FLAG_C, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        smsdebug_cpu_get(m_d, &c);
        QCOMPARE(c.cf, 1);
        QCheckBox *iff1 = check(QStringLiteral("IFF1"));
        QTest::mouseClick(iff1, Qt::LeftButton, {}, QPoint(6, iff1->height() / 2));
        smsdebug_cpu_get(m_d, &c);
        QCOMPARE(c.iff1, 1);
        smsdebug_cpu_set(m_d, SMS_REG_IFF1, 0);
        smsdebug_cpu_set(m_d, SMS_FLAG_C, 0);
        /* the beam / cycles line */
        bool beam = false;
        for (QLabel *l : m_win->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("vpos"))) beam = l->text().contains(QLatin1String("T-states"));
        QVERIFY(beam);
        /* memory: poke through the map, go to a label or an address */
        QLineEdit *addr = entryIs(QStringLiteral("$C000"));
        QLineEdit *val = entryIs(QStringLiteral("$00"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("C110"));       /* hex */
        typeInto(val, QStringLiteral("5a"));
        uint8_t b = 0;
        smsdebug_read(m_d, 0xC110, &b, 1);
        QCOMPARE((int)b, 0x5A);
        addr->setText(QStringLiteral("$C111"));
        typeInto(val, QStringLiteral("$A5"));
        smsdebug_read(m_d, 0xC111, &b, 1);
        QCOMPARE((int)b, 0xA5);
        smsdebug_write(m_d, 0xC112, 0x11);
        addr->setText(QStringLiteral("C112"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte */
        smsdebug_read(m_d, 0xC112, &b, 1);
        QCOMPARE((int)b, 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        QPlainTextEdit *mem = memView();
        QLineEdit *go = entryIs(QStringLiteral("$C000, label or lo-hi"));
        QVERIFY(go && mem);
        typeInto(go, QStringLiteral("C000"));
        /* all 64K are there: the first row and the last, each named by its region */
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$0000: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$FFF0: ")));
        QVERIFY(mem->toPlainText().contains(QStringLiteral("$C000: 31 00 DF")));
        QCOMPARE(mem->document()->blockCount(), 4097);
        const QString first = mem->document()->findBlockByNumber(1).text();
        const QString ram = mem->document()->findBlockByNumber(1 + 0xC000 / 16).text();
        QVERIFY2(!first.mid(72).trimmed().isEmpty(), qPrintable(first));
        QVERIFY2(ram.contains(QLatin1String("RAM")), qPrintable(ram));
        /* Go to scrolled the row into view and marked it in the accent, white ink */
        QVERIFY(mem->verticalScrollBar()->value() >= 0xC000 / 16 - 40);
        bool marked = false;
        for (const auto &s : mem->extraSelections())
            marked |= s.format.background().color() == kAccent && s.format.foreground().color() == QColor(Qt::white);
        QVERIFY(marked);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!smsdebug_is_stopped(m_d), 2000);
        typeInto(regs[0], QStringLiteral("C000"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the machine")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("memory"));
    }

    void breakpointsTab()
    {
        m_tabs->setCurrentIndex(3);
        QTableWidget *t = bpTable();
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(t->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("address, range"));
        QLineEdit *end = entry(QStringLiteral("end"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QPushButton *add = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Add")) add = b;
        QPushButton *clear = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Breakpoints")) clear = b;
        QLabel *msg = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("e01b24"))) msg = l;
        QVERIFY(add && clear && msg);

        /* exec at a hex address */
        typeInto(start, QStringLiteral("C009"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(t->item(0, 1)->text(), QStringLiteral("Exec  $C009"));
        QCOMPARE(t->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a write watchpoint over a range, with a condition */
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("C100"));
        end->setText(QStringLiteral("$C1FF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        QCOMPARE(t->item(1, 1)->text(), QStringLiteral("Write  $C100-$C1FF  if value > 0"));
        /* the start may itself be a range */
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Access"));
        typeInto(start, QStringLiteral("C200-C20F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 3, 2000);
        QVERIFY2(t->item(2, 1)->text().startsWith(QLatin1String("Access  $C200-$C20F")), qPrintable(t->item(2, 1)->text()));
        /* the port types, VRAM and CRAM writes */
        type->setCurrentText(QStringLiteral("Port OUT"));
        typeInto(start, QStringLiteral("7F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 4, 2000);
        QCOMPARE(t->item(3, 1)->text(), QStringLiteral("Out  $7F"));
        type->setCurrentText(QStringLiteral("Port IN"));
        typeInto(start, QStringLiteral("DC"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 5, 2000);
        type->setCurrentText(QStringLiteral("VRAM write"));
        start->setText(QStringLiteral("1800"));
        typeInto(end, QStringLiteral("1AFF"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 6, 2000);
        QCOMPARE(t->item(5, 1)->text(), QStringLiteral("VRAM write  $1800-$1AFF"));
        type->setCurrentText(QStringLiteral("CRAM write"));
        end->clear();
        typeInto(start, QStringLiteral("5"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 7, 2000);
        QCOMPARE(t->item(6, 1)->text(), QStringLiteral("CRAM write  5"));
        /* the ranges are checked: a port is a byte, VRAM is 16K, CRAM has 32 entries */
        const int rowsBefore = t->rowCount();
        type->setCurrentText(QStringLiteral("CRAM write"));
        typeInto(start, QStringLiteral("32"));
        QVERIFY2(msg->text().contains(QLatin1String("32 entries")), qPrintable(msg->text()));
        type->setCurrentText(QStringLiteral("VRAM write"));
        typeInto(start, QStringLiteral("4000"));
        QVERIFY(msg->text().contains(QLatin1String("$0000-$3FFF")));
        type->setCurrentText(QStringLiteral("Port IN"));
        typeInto(start, QStringLiteral("100"));
        QVERIFY(msg->text().contains(QLatin1String("$00-$FF")));
        QCOMPARE(t->rowCount(), rowsBefore);
        /* errors are shown, not swallowed */
        type->setCurrentText(QStringLiteral("Execute"));
        cond->setText(QStringLiteral("a == (1 +"));
        start->setText(QStringLiteral("7000"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(t->rowCount(), rowsBefore);
        cond->clear();
        type->setCurrentText(QStringLiteral("Port IN"));
        start->setText(QStringLiteral("FF"));
        end->setText(QStringLiteral("FC"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(msg->text().contains(QLatin1String("before")));
        start->clear();
        end->clear();
        msg->clear();
        type->setCurrentText(QStringLiteral("Execute"));

        QStringList types;
        for (int i = 0; i < type->count(); ++i) types << type->itemText(i);
        QCOMPARE(types, (QStringList{ QStringLiteral("Execute"), QStringLiteral("Read"), QStringLiteral("Write"),
                                      QStringLiteral("Access"), QStringLiteral("Port IN"), QStringLiteral("Port OUT"),
                                      QStringLiteral("VRAM write"), QStringLiteral("CRAM write") }));
        shot(QStringLiteral("breakpoints-types"));

        /* back to two: the exec one and the conditional write */
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        typeInto(start, QStringLiteral("C009"));
        type->setCurrentText(QStringLiteral("Write"));
        start->setText(QStringLiteral("C100"));
        end->setText(QStringLiteral("$C1FF"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        cond->clear();
        end->clear();
        type->setCurrentText(QStringLiteral("Execute"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);

        /* the exec breakpoint stops the run, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d) && pcOf(m_d) == 0xC009, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("exec $C009")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 2) && t->item(0, 2)->text().toInt() >= 1, 2000);
        /* unticking it lets the run pass; the write watch then stops it instead */
        t->item(0, 0)->setCheckState(Qt::Unchecked);
        smsdebug_breakpoint list[2];
        QCOMPARE(smsdebug_breakpoint_list(m_d, list, 2), 2);
        QCOMPARE(list[0].enabled, 0);
        QCOMPARE(list[1].enabled, 1);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d) && statusLabel()->text().contains(QLatin1String("wrote $C100")),
                                 4000);
        QVERIFY(pcOf(m_d) != 0xC009);
        /* Clear Hits zeroes the counts */
        QPushButton *clearHits = nullptr;
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear Hits")) clearHits = b;
        QVERIFY(clearHits);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(1, 2) && t->item(1, 2)->text().toInt() >= 1, 2000);
        QTest::mouseClick(clearHits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->item(1, 2)->text(), QStringLiteral("0"), 2000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *rm = qobject_cast<QToolButton *>(t->cellWidget(1, 3));
        QVERIFY(rm);
        QTest::mouseClick(rm, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(smsdebug_breakpoint_list(m_d, list, 2), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        QCOMPARE(smsdebug_breakpoint_list(m_d, list, 2), 0);
    }

    /* The VRAM and CRAM write breakpoints stop at the VDP's data port. */
    void vdpWriteBreakpoints()
    {
        /* LD A,$11 / OUT ($BF),A / LD A,$C0 / OUT ($BF),A  -- the CRAM address 17 -- then OUT ($BE) a colour */
        const uint8_t code[] = { 0x3E, 0x11, 0xD3, 0xBF, 0x3E, 0xC0, 0xD3, 0xBF, 0x3E, 0x2A, 0xD3, 0xBE, 0x00, 0x18, 0xFD };
        for (size_t i = 0; i < sizeof code; i++) smsdebug_write(m_d, (uint16_t)(0xC200 + i), code[i]);
        smsdebug_cpu_set(m_d, SMS_REG_PC, 0xC200);
        QVERIFY(smsdebug_breakpoint_add(m_d, SMSDEBUG_BP_CRAM, 17, 17, "") >= 0);
        m_tabs->setCurrentIndex(3);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("cram write 17 = $2A")), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(bpTable()->item(0, 1)->text(), QStringLiteral("CRAM write  17"), 2000);
        smsdebug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(0);
    }

    QPlainTextEdit *memView()
    {
        return m_tabs->widget(2)->findChild<QPlainTextEdit *>();
    }

    /* Watches: add, live value, Memory, Break on change, Remove. */
    void watchesTab()
    {
        smswatch_clear();
        smsdebug_write(m_d, 0xC110, 0x42);
        m_tabs->setCurrentIndex(4);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QTableWidget *t = m_tabs->currentWidget()->findChild<QTableWidget *>();
        QLineEdit *e = m_tabs->currentWidget()->findChild<QLineEdit *>();
        QVERIFY(t && e);
        QCOMPARE(t->rowCount(), 0);
        typeInto(e, QStringLiteral("$C110"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$C110")));
        QVERIFY(t->item(0, 0)->text().contains(QStringLiteral("$42")));
        smsdebug_write(m_d, 0xC110, 0x43);
        QTest::keyClick(m_win, Qt::Key_F7);             /* a stop refreshes the values */
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        typeInto(e, QStringLiteral("$C120-$C12F"));
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 2, 2000);
        /* while the machine runs the value is a dash */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!smsdebug_is_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(t->item(0, 0)->text().contains(QLatin1String("-")) &&
                                     !t->item(0, 0)->text().contains(QStringLiteral("$43")), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("watches"));

        /* Break on change adds a write breakpoint with the condition */
        smsdebug_breakpoint_clear(m_d);
        auto buttonOf = [&](int row, const QString &text) -> QPushButton * {
            for (QPushButton *b : t->cellWidget(row, 1)->findChildren<QPushButton *>())
                if (b->text() == text) return b;
            return nullptr;
        };
        QPushButton *brk = buttonOf(0, QStringLiteral("Break on change"));
        QVERIFY(brk);
        QTest::mouseClick(brk, Qt::LeftButton);
        smsdebug_breakpoint bp[2];
        QCOMPARE(smsdebug_breakpoint_list(m_d, bp, 2), 1);
        QVERIFY(bp[0].type & SMSDEBUG_BP_WRITE);
        QCOMPARE((int)bp[0].start, 0xC110);
        QVERIFY(QString::fromUtf8(bp[0].condition).contains(QStringLiteral("old")));
        smsdebug_breakpoint_clear(m_d);

        /* Memory switches to the Memory tab with the range marked */
        QPushButton *mem = buttonOf(1, QStringLiteral("Memory"));
        QVERIFY(mem);
        QTest::mouseClick(mem, Qt::LeftButton);
        QCOMPARE(m_tabs->currentIndex(), 2);
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$C120: ")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* and every watch is marked in the dump, in its own colour */
        bool watched = false;
        for (const auto &s : memView()->extraSelections())
            watched |= s.format.background().color() == QColor(0x1c, 0x71, 0xd8);
        QVERIFY(watched);

        m_tabs->setCurrentIndex(4);
        QTest::mouseClick(buttonOf(1, QStringLiteral("Remove")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 1, 2000);
        QCOMPARE(smswatch_count(), 1);
        smswatch_clear();
        QTRY_COMPARE_WITH_TIMEOUT(t->rowCount(), 0, 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Go to marks a range; Find Next / Previous step through hits. */
    void memorySearch()
    {
        const uint8_t pat[4] = { 0xDE, 0xEE, 0xDE, 0xEE };
        for (int i = 0; i < 4; ++i) smsdebug_write(m_d, (uint16_t)(0xC200 + i), pat[i]);
        for (int i = 0; i < 4; ++i) smsdebug_write(m_d, (uint16_t)(0xC300 + i), pat[i]);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *go = entryIs(QStringLiteral("$C000, label or lo-hi"));
        typeInto(go, QStringLiteral("C200-C203"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$C200: DE EE DE EE")));
        QVERIFY(!memView()->extraSelections().isEmpty());
        /* a label also works */
        smsdebug_set_label(m_d, 0xC300, "found");
        typeInto(go, QStringLiteral("found"));
        QVERIFY(memView()->toPlainText().contains(QStringLiteral("$C300: DE EE")));
        smsdebug_set_label(m_d, 0xC300, nullptr);

        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->styleSheet().contains(QLatin1String("gray")) && l->text().isEmpty()) note = l;
        QVERIFY(note);
        typeInto(go, QStringLiteral("C200-C203"));
        find->setText(QStringLiteral("DE EE DE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $C300"));          /* after the marked one */
        QTest::mouseClick(button(QStringLiteral("Find Previous")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $C200"));
        find->setText(QStringLiteral("DE EE DE EE FF 77 66"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Not found"));
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Not found"));
        /* text, case-insensitive */
        for (int i = 0; i < 5; ++i) smsdebug_write(m_d, (uint16_t)(0xC400 + i), (uint8_t)"HeLLo"[i]);
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Text"));
        find->setText(QStringLiteral("hello"));
        typeInto(go, QStringLiteral("C000"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(note->text(), QStringLiteral("Found at $C400"));
        shot(QStringLiteral("search"));
        m_tabs->currentWidget()->findChild<QComboBox *>()->setCurrentText(QStringLiteral("Hex"));
        /* it needs a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!smsdebug_is_stopped(m_d), 2000);
        find->setText(QStringLiteral("DE EE"));
        QTest::mouseClick(button(QStringLiteral("Find Next")), Qt::LeftButton);
        QCOMPARE(m_win->statusBar()->currentMessage(), QStringLiteral("Stop the machine first"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        m_tabs->setCurrentIndex(0);
    }

    /* Trace and History share the ring and the recorder. */
    void traceAndHistory()
    {
        smsdebug_trace_enable(m_d, 0);
        smsdebug_trace_clear(m_d);
        m_tabs->setCurrentIndex(6);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QPushButton *rec = m_tabs->currentWidget()->findChild<QPushButton *>();
        QVERIFY(rec && rec->text() == QLatin1String("Record"));
        QPushButton *rec2 = m_tabs->widget(7)->findChild<QPushButton *>();
        QVERIFY(rec2 && rec2 != rec);
        QVERIFY(!rec->isChecked());
        QTest::mouseClick(rec, Qt::LeftButton);
        QVERIFY(smsdebug_trace_enabled(m_d));
        QVERIFY(rec->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(rec2->isChecked(), 2000);                   /* in sync */
        QTest::keyClick(m_win, Qt::Key_F5);                                  /* runs */
        QTRY_VERIFY_WITH_TIMEOUT(!smsdebug_is_stopped(m_d), 2000);
        auto *tr = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(tr->toPlainText().contains(QStringLiteral("INC A")), 3000);   /* live */
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_trace_count(m_d) > 20, 2000);
        /* History shows the same ring, oldest first, at the stop */
        m_tabs->setCurrentIndex(7);
        auto *hist = m_tabs->currentWidget()->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        QVERIFY(hist->toPlainText().count(QLatin1Char('\n')) < 2000);
        /* newest at the bottom: the view is scrolled to its end */
        QVERIFY(hist->verticalScrollBar()->value() >= hist->verticalScrollBar()->maximum() - 1);
        QLabel *note = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().contains(QLatin1String("instructions"))) note = l;
        QVERIFY(note && note->text().startsWith(QLatin1String("recording")));
        shot(QStringLiteral("history"));
        /* a branches-only filter leaves jumps, calls and returns */
        auto *filter = m_tabs->currentWidget()->findChild<QComboBox *>();
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        QVERIFY(hist->toPlainText().contains(QStringLiteral("JP ")));
        /* a range filter */
        filter->setCurrentIndex(2);
        auto *range = m_tabs->currentWidget()->findChild<QLineEdit *>();
        typeInto(range, QStringLiteral("C00F-C00F"));
        QTRY_VERIFY_WITH_TIMEOUT(hist->toPlainText().contains(QStringLiteral("LD (HL),A")) &&
                                     !hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        filter->setCurrentIndex(0);
        /* Clear */
        for (QPushButton *b : m_tabs->currentWidget()->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Clear")) QTest::mouseClick(b, Qt::LeftButton);
        QCOMPARE(smsdebug_trace_count(m_d), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!hist->toPlainText().contains(QStringLiteral("INC A")), 2000);
        /* and off, from either tab */
        QTest::mouseClick(m_tabs->widget(7)->findChild<QPushButton *>(), Qt::LeftButton);
        QVERIFY(!smsdebug_trace_enabled(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(6)->findChild<QPushButton *>()->isChecked(), 2000);
        m_tabs->setCurrentIndex(0);
    }

    /* Any tab can leave for a window of its own and come back to its place. */
    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(3);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QWidget *win = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("SMS Debugger - Breakpoints")) win = w;
        QVERIFY(win);
        QTRY_VERIFY_WITH_TIMEOUT(win->isVisible(), 1000);
        QVERIFY(win->findChild<QTableWidget *>());
        QCOMPARE(win->property("smsHelpContext").toString(), QStringLiteral("debugger/breakpoints"));
        /* the popped tab keeps refreshing: a breakpoint added shows in its window */
        smsdebug_breakpoint_toggle(m_d, 0xC009);
        QTRY_COMPARE_WITH_TIMEOUT(win->findChild<QTableWidget *>()->rowCount(), 1, 2000);
        smsdebug_breakpoint_clear(m_d);
        shot(QStringLiteral("popout"));
        QPushButton *dockBtn = nullptr;
        for (QPushButton *b : win->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Dock")) dockBtn = b;
        QVERIFY(dockBtn);
        QTest::mouseClick(dockBtn, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(3), QStringLiteral("Breakpoints"));       /* back in its place */
        /* closing the window docks it too; several can be out, but one stays */
        m_tabs->setCurrentIndex(5);
        QTest::mouseClick(pop, Qt::LeftButton);
        QCOMPARE(m_tabs->count(), 11);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Trace"));
        QWidget *cart = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle() == QLatin1String("SMS Debugger - Cart & I/O")) cart = w;
        QVERIFY(cart);
        cart->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QCOMPARE(m_tabs->tabText(5), QStringLiteral("Cart && I/O"));
        for (int i = 0; i < 11; ++i) {
            m_tabs->setCurrentIndex(0);
            QTest::mouseClick(pop, Qt::LeftButton);
        }
        QCOMPARE(m_tabs->count(), 1);
        QTest::mouseClick(pop, Qt::LeftButton);                          /* the last stays */
        QCOMPARE(m_tabs->count(), 1);
        for (QWidget *w : QApplication::topLevelWidgets())
            if (w->windowTitle().startsWith(QLatin1String("SMS Debugger - "))) w->close();
        QTRY_COMPARE_WITH_TIMEOUT(m_tabs->count(), 12, 1000);
        QStringList names;
        for (int i = 0; i < m_tabs->count(); ++i) names << m_tabs->tabText(i);
        QCOMPARE(names.first(), QStringLiteral("Console"));
        QCOMPARE(names.last(), QStringLiteral("FujiNet"));
        QCOMPARE(names.at(5), QStringLiteral("Cart && I/O"));
        QCOMPARE(names.at(10), QStringLiteral("Symbols"));
    }

    /* A new stop pulls every view back to the spot from wherever it was
     * scrolled: the disassembly to the PC, Memory to the PC, the Trace and
     * History to their newest line; and the Breakpoints tab says why. */
    void stopRecentresAndNames()
    {
        m_tabs->setCurrentIndex(1);
        QCheckBox *follow = check(QStringLiteral("Follow PC"));
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, 0xC007) >= 0, 2000);
        const QPoint p = v->viewport()->rect().center();
        QWheelEvent wheel(p, v->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -600), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(v->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* Memory scrolled to the top, History away from its end */
        m_tabs->setCurrentIndex(2);
        memView()->verticalScrollBar()->setValue(0);
        m_tabs->setCurrentIndex(1);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_VERIFY_WITH_TIMEOUT(follow->isChecked(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(v, pcOf(m_d)) >= 0, 2000);
        m_tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(memView()->verticalScrollBar()->value() >= pcOf(m_d) / 16 - 80, 2000);
        m_tabs->setCurrentIndex(3);
        QLabel *stop = nullptr;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (l->text().startsWith(QLatin1String("Stopped"))) stop = l;
        QVERIFY(stop);
        QVERIFY(stop->text().startsWith(QLatin1String("Stopped")));
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(stop->text() == QLatin1String("Running."), 2000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QVERIFY(button(QStringLiteral("Clear Hits")));
        m_tabs->setCurrentIndex(0);
    }

    void otherTabs()
    {
        /* Cart & I/O: the cartridge, the memory map once, the ports */
        m_tabs->setCurrentIndex(5);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *io = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(io);
        QTRY_VERIFY_WITH_TIMEOUT(!io->toPlainText().isEmpty(), 2000);
        const QString text = io->toPlainText();
        QCOMPARE(text.count(QStringLiteral("Memory map")), 1);
        QVERIFY(text.contains(QStringLiteral("Memory control $3E")));
        QVERIFY(text.contains(QStringLiteral("Mapper")));
        QVERIFY(text.contains(QStringLiteral("Player 1")));
        QVERIFY(text.contains(QStringLiteral("$C000-$DFFF")));
        shot(QStringLiteral("cart"));
        /* Sound: the PSG, the FM chip */
        m_tabs->setCurrentIndex(9);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        auto *sound = qobject_cast<QPlainTextEdit *>(m_tabs->currentWidget());
        QVERIFY(sound);
        QTRY_VERIFY_WITH_TIMEOUT(sound->toPlainText().contains(QStringLiteral("PSG (SN76489)")), 2000);
        QVERIFY(sound->toPlainText().contains(QStringLiteral("YM2413")));
        QVERIFY(!sound->toPlainText().contains(QStringLiteral("Memory control")));      /* moved to Cart & I/O */
        shot(QStringLiteral("sound"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(11);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *l : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= l->text().contains(QLatin1String("FujiNet"));
        QVERIFY(note);
        QVERIFY(m_tabs->currentWidget()->findChild<QPushButton *>()->text() == QLatin1String("Clear"));
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(0);
    }

    /* The Video tab: views, the Decoded source, zoom, overlays, hover and
     * pin, the tables, Break on writes and Break on CRAM write. */
    void videoTab()
    {
        smsdebug_breakpoint_clear(m_d);
        m_tabs->setCurrentIndex(8);
        QWidget *page = m_tabs->currentWidget();
        QTRY_VERIFY_WITH_TIMEOUT(page->isVisible(), 1000);
        auto combos = page->findChildren<QComboBox *>();
        QVERIFY(combos.size() >= 3);
        QComboBox *view = combos[0], *source = combos[1], *zoom = combos[2];
        QStringList views;
        for (int i = 0; i < view->count(); ++i) views << view->itemText(i);
        QCOMPARE(views, (QStringList{ QStringLiteral("Display"), QStringLiteral("Decoded"), QStringLiteral("Beam"),
                                      QStringLiteral("Values") }));
        QCOMPARE(source->itemText(0), QStringLiteral("Screen"));
        QVERIFY(source->count() >= 6);
        QVERIFY(!source->isEnabled());                      /* only Decoded has a source */
        QCOMPARE(zoom->itemText(0), QStringLiteral("Fit"));
        QWidget *pic = page->findChild<QWidget *>(QStringLiteral("videoPicture"));
        QVERIFY(pic);
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100 && pic->height() >= 100, 2000);
        shot(QStringLiteral("video-display"));

        /* a change of view redraws */
        view->setCurrentIndex(1);
        QVERIFY(source->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);
        shot(QStringLiteral("video-decoded"));
        for (int i = 1; i < source->count(); ++i) {
            source->setCurrentIndex(i);
            QTest::qWait(30);
            QTRY_VERIFY_WITH_TIMEOUT(pic->width() >= 100, 1000);
            shot(QStringLiteral("video-decoded-%1").arg(i));
        }
        source->setCurrentIndex(0);
        view->setCurrentIndex(2);
        QTest::qWait(30);
        shot(QStringLiteral("video-beam"));
        view->setCurrentIndex(3);
        QTest::qWait(30);
        shot(QStringLiteral("video-values"));
        view->setCurrentIndex(0);

        /* zoom resizes the picture (Decoded: the Display base can be too big to double) */
        view->setCurrentIndex(1);
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("1×")));
        QTest::qWait(50);
        const int w1 = pic->width();
        zoom->setCurrentIndex(zoom->findText(QStringLiteral("2×")));
        QTRY_VERIFY2_WITH_TIMEOUT(pic->width() == w1 * 2, qPrintable(QStringLiteral("1x %1 2x %2").arg(w1).arg(pic->width())), 1000);
        view->setCurrentIndex(0);

        /* an overlay checkbox */
        QCheckBox *grid = nullptr;
        for (QCheckBox *box : page->findChildren<QCheckBox *>())
            if (box->text().contains(QLatin1String("rid"))) grid = box;
        QVERIFY(grid);
        grid->setChecked(true);
        QTest::qWait(50);
        shot(QStringLiteral("video-grid"));
        grid->setChecked(false);

        /* hover shows, a click pins, the same spot unpins */
        auto *tabs = page->findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Pixel"));
        tabs->setCurrentIndex(0);
        QLabel *pixel = nullptr;
        for (QLabel *l : tabs->widget(0)->findChildren<QLabel *>()) pixel = l;
        QVERIFY(pixel);
        QVERIFY(pixel->text().contains(QLatin1String("Move over")));
        const QPoint at(pic->width() / 2, pic->height() / 2);
        QTest::mouseMove(pic, at);
        QTRY_VERIFY_WITH_TIMEOUT(!pixel->text().contains(QLatin1String("Move over")), 1000);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, at + QPoint(30, 30));
        QVERIFY(!pixel->text().contains(QLatin1String("Move over")));   /* the pin stays */
        shot(QStringLiteral("video-pixel"));
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QTest::mouseMove(pic, QPoint(-5, -5));

        /* the tables */
        QTableWidget *regs = nullptr, *lines = nullptr, *colours = nullptr;
        for (QTableWidget *t : page->findChildren<QTableWidget *>()) {
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Name")) regs = t;
            if (t->horizontalHeaderItem(0) && t->horizontalHeaderItem(0)->text() == QLatin1String("Row")) lines = t;
            if (t->horizontalHeaderItem(1) && t->horizontalHeaderItem(1)->text() == QLatin1String("Entry")) colours = t;
        }
        QVERIFY(regs && lines && colours);
        tabs->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(regs->rowCount() > 0, 1000);
        shot(QStringLiteral("video-registers"));
        tabs->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(colours->rowCount() >= 16, 1000);
        shot(QStringLiteral("video-colours"));
        tabs->setCurrentIndex(3);
        QTRY_VERIFY_WITH_TIMEOUT(lines->rowCount() > 0, 1000);
        shot(QStringLiteral("video-lines"));
        tabs->setCurrentIndex(4);
        QTest::qWait(50);
        shot(QStringLiteral("video-timing"));

        /* Break on CRAM write, per swatch */
        tabs->setCurrentIndex(2);
        auto *swatchButton = qobject_cast<QPushButton *>(colours->cellWidget(3, 5));
        QVERIFY(swatchButton);
        smsdebug_breakpoint bps[8];
        QCOMPARE(smsdebug_breakpoint_list(m_d, bps, 8), 0);
        QTest::mouseClick(swatchButton, Qt::LeftButton);
        QCOMPARE(smsdebug_breakpoint_list(m_d, bps, 8), 1);
        QVERIFY(bps[0].type & SMSDEBUG_BP_CRAM);
        QCOMPARE((int)bps[0].start, 3);
        smsdebug_breakpoint_clear(m_d);
        tabs->setCurrentIndex(0);

        /* Break on writes: VRAM write breakpoints on the inspected pixel (the area outside the active
         * display, which CONFIG may not use, is skipped: the click only has to land on the picture) */
        view->setCurrentIndex(1);
        QTest::qWait(50);
        QTest::mouseMove(pic, at);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        QPushButton *brk = nullptr;
        for (QPushButton *b : page->findChildren<QPushButton *>())
            if (b->text() == QLatin1String("Break on writes")) brk = b;
        QVERIFY(brk);
        /* with the display off there is no tile behind the pixel: said, not silent */
        QTest::mouseClick(brk, Qt::LeftButton);
        QCOMPARE(smsdebug_breakpoint_list(m_d, bps, 8), 0);
        bool said = false;
        for (QLabel *l : page->findChildren<QLabel *>()) said |= l->text().contains(QLatin1String("Nothing to break on"));
        QVERIFY(said);
        /* a program that turns the display on (R1 = $E0), run for a few frames */
        const uint8_t displayOn[] = { 0x3E, 0xE0, 0xD3, 0xBF, 0x3E, 0x81, 0xD3, 0xBF, 0x18, 0xFE };
        for (size_t i = 0; i < sizeof displayOn; i++) smsdebug_write(m_d, (uint16_t)(0xC300 + i), displayOn[i]);
        smsdebug_cpu_set(m_d, SMS_REG_PC, 0xC300);
        smsdebug_cpu c, c2;
        smsdebug_cpu_get(m_d, &c);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT((smsdebug_cpu_get(m_d, &c2), c2.frame >= c.frame + 3), 3000);
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        QTest::qWait(50);
        QTest::mouseMove(pic, at);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);
        shot(QStringLiteral("video-pixel-on"));
        QTest::mouseClick(brk, Qt::LeftButton);
        const int made = smsdebug_breakpoint_list(m_d, bps, 8);
        QVERIFY2(made >= 1, "Break on writes made no VRAM breakpoint");
        for (int i = 0; i < made; ++i) QVERIFY(bps[i].type & SMSDEBUG_BP_VRAM);
        smsdebug_breakpoint_clear(m_d);
        QTest::mouseClick(pic, Qt::LeftButton, {}, at);     /* unpin */
        zoom->setCurrentIndex(0);
        view->setCurrentIndex(0);
        m_tabs->setCurrentIndex(0);
    }

    /* The main window's F12 (display focused: its ShortcutOverride lets the
     * chord through to the View menu's action) and the View menu entry. */
    void f12FromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display, so ask for whichever
         * is there */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        QWidget *display = main.centralWidget();
        QVERIFY(display);
        display->setFocus();
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::focusWidget() == display, 2000);
        QCOMPARE(QApplication::focusWidget(), display);
        QVERIFY(m_win->isVisible());
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!smsdebug_is_attached(m_d));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(display, Qt::Key_F12);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_attached(m_d), 2000);
        waitStopped();
        /* the menu entry does the same */
        QAction *entry = nullptr;
        for (QAction *a : main.findChildren<QAction *>())
            if (a->text() == QLatin1String("&Debugger")) entry = a;
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), QKeySequence(Qt::Key_F12));
        entry->trigger();
        QVERIFY(!m_win->isVisible());
        entry->trigger();
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_attached(m_d), 2000);
        waitStopped();
    }

    /* A power cycle or media change keeps the engine attached: the new
     * machine stops at its first instruction and the window follows. */
    void sessionRestart()
    {
        m_tabs->setCurrentIndex(1);
        smsdebug_breakpoint_toggle(m_d, 0xC009);
        smssession_stop(m_s);
        QCOMPARE(smssession_start(m_s, &m_opts), 0);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d), 3000);
        QCOMPARE(pcOf(m_d), 0x0000);
        QVERIFY(smsdebug_breakpoint_check(m_d, 0xC009));       /* kept */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped")), 2000);
        QPlainTextEdit *v = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(v->toPlainText().contains(QStringLiteral(">0000")), 2000);
        /* and, given the program again, it runs from there to the breakpoint */
        loadProgram(m_d);
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_stopped(m_d) && pcOf(m_d) == 0xC009, 5000);
    }

    void f12HidesAndDetaches()
    {
        QVERIFY(m_win->isVisible());
        QTest::keyClick(m_win, Qt::Key_F12);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!smsdebug_is_attached(m_d));
        smsdebug_cpu c, c2;
        smsdebug_cpu_get(m_d, &c);
        QTRY_VERIFY_WITH_TIMEOUT((smsdebug_cpu_get(m_d, &c2), c2.frame > c.frame + 3), 3000);
        /* the main window's F12 is toggleFor: it shows the window and attaches (stopping) */
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_attached(m_d), 2000);
        waitStopped();
        DebuggerWindow::toggleFor(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!smsdebug_is_attached(m_d));
        DebuggerWindow::toggleFor(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(smsdebug_is_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
