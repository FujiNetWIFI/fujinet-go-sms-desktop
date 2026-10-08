/*
 * DebuggerWindow -- see DebuggerWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cstring>

#include "../FujiNetWindows.h"

#define DISASM_WINDOW 48

namespace {

QPlainTextEdit *monoView(bool editable)
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(!editable);
    QFont f = v->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    v->setFont(f);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

QString stripControl(const char *s)
{
    QString out;
    for (; *s; ++s)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') out += QChar(*s);
    return out;
}

/* $C000, 0xC000, C000h, #49152; a bare number is hex, as everywhere in a
 * Z80 debugger. */
bool parseNum(const QString &t, long *out)
{
    QString s = t.trimmed();
    bool ok = false;
    long v = 0;
    if (s.startsWith('$')) v = s.mid(1).toLong(&ok, 16);
    else if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) v = s.mid(2).toLong(&ok, 16);
    else if (s.startsWith('#')) v = s.mid(1).toLong(&ok, 10);
    else if (s.endsWith('h', Qt::CaseInsensitive)) v = s.chopped(1).toLong(&ok, 16);
    else v = s.toLong(&ok, 16);
    if (ok) *out = v;
    return ok;
}

QString hx(unsigned v, int digits)
{
    return QStringLiteral("%1").arg(v, digits, 16, QLatin1Char('0')).toUpper();
}

QString yesNo(int v) { return v ? QStringLiteral("yes") : QStringLiteral("no"); }

/* The order of the tabs, and their names for SMS_DEBUGGER_TAB. */
const char *const kTabNames[] = { "prompt", "cpu", "disassembly", "vdp", "sound", "breakpoints", "cart" };

QPointer<DebuggerWindow> s_win;

} // namespace

void DebuggerWindow::showFor(QWidget *parent, smssession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    /* Opening the debugger stops the machine, as on every sibling. */
    smsdebug_attach(s_win->m_dbg);
    s_win->refreshAll();
}

void DebuggerWindow::toggleFor(QWidget *parent, smssession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else showFor(parent, session);
}

DebuggerWindow::DebuggerWindow(smssession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(smssession_debugger(session))
{
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1180, 820);
    m_vdpPx.resize(SMSDEBUG_VIEW_MAX_PIXELS);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    auto *tabs = new QTabWidget;
    tabs->addTab(buildPrompt(), QStringLiteral("Prompt"));
    tabs->addTab(buildCpu(), QStringLiteral("CPU && RAM"));
    tabs->addTab(buildDisasm(), QStringLiteral("Disassembly"));
    tabs->addTab(buildVdp(), QStringLiteral("VDP"));
    tabs->addTab(buildSound(), QStringLiteral("Sound && I/O"));
    tabs->addTab(buildBreaks(), QStringLiteral("Breakpoints"));
    tabs->addTab(buildCart(), QStringLiteral("Cart"));
    /* SMS_DEBUGGER_TAB: an index, or a tab's name (or the start of one) */
    if (qEnvironmentVariableIsSet("SMS_DEBUGGER_TAB")) {
        const QString want = qEnvironmentVariable("SMS_DEBUGGER_TAB").trimmed().toLower();
        bool ok = false;
        int n = want.toInt(&ok);
        if (!ok) {
            n = -1;
            for (int i = 0; i < (int)(sizeof kTabNames / sizeof kTabNames[0]) && !want.isEmpty(); ++i)
                if (QString::fromUtf8(kTabNames[i]).startsWith(want)) { n = i; break; }
        }
        if (n >= 0 && n < tabs->count()) tabs->setCurrentIndex(n);
    }
    root->addWidget(tabs, 1);
    setCentralWidget(central);

    connect(&m_timer, &QTimer::timeout, this, &DebuggerWindow::tick);
    m_timer.start(100);
}

/* ---- building ------------------------------------------------------------- */

void DebuggerWindow::stepAnd(void (*fn)(smsdebug *))
{
    fn(m_dbg);
    refreshAll();
}

void DebuggerWindow::toggleRun()
{
    if (smsdebug_is_stopped(m_dbg)) smsdebug_resume(m_dbg); else smsdebug_stop(m_dbg);
    refreshAll();
}

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, auto fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), [this] { stepAnd(smsdebug_step); });
    add(QStringLiteral("Over (F8)"), [this] { stepAnd(smsdebug_step_over); });
    add(QStringLiteral("Out (Shift+F8)"), [this] { stepAnd(smsdebug_step_out); });
    add(QStringLiteral("Scanline+1"), [this] { smsdebug_scanline(m_dbg, 1); refreshAll(); });
    add(QStringLiteral("Frame+1"), [this] { smsdebug_frame(m_dbg, 1); refreshAll(); });

    auto *sym = new QPushButton(QStringLiteral("Load Symbols..."));
    sym->setFocusPolicy(Qt::NoFocus);
    auto *symMenu = new QMenu(sym);
    symMenu->addAction(QStringLiteral("Symbol File..."), this, [this] { loadSymbols(true); });
    symMenu->addAction(QStringLiteral("Next to the Cartridge"), this, [this] { loadSymbols(false); });
    sym->setMenu(symMenu);
    h->addWidget(sym);

    auto *save = new QPushButton(QStringLiteral("Save..."));
    save->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(save);
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($0000-$BFFF as mapped)", "disassembly.asm" },
        { "ram", "Console RAM ($C000-$DFFF)", "ram.bin" },
        { "vram", "VRAM", "vram.bin" },
        { "cram", "CRAM", "cram.bin" },
        { "sram", "Cartridge SRAM", "sram.bin" },
        { "arena", "Mailbox Arena", "arena.bin" },
        { "regs", "CPU and VDP Registers", "registers.txt" } };
    for (const auto &s : saves) {
        const char *kind = s.kind;
        const char *title = s.title;
        const char *file = s.file;
        menu->addAction(QString::fromUtf8(title), this, [this, kind, title, file] {
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(QString::fromUtf8(title)),
                                                              QString::fromUtf8(file));
            if (path.isEmpty()) return;
            char msg[512];
            smsdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
            const QString text = stripControl(msg);
            appendPrompt(text + QLatin1Char('\n'));
            statusBar()->showMessage(text, 6000);
        });
    }
    save->setMenu(menu);
    h->addWidget(save);

    m_status = new QLabel;
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    h->addWidget(m_status, 1);
    return bar;
}

/* A symbol file the user picks, or (fromFile false) the <cart>.sym/.map/.noi
 * beside the opened cartridge. */
void DebuggerWindow::loadSymbols(bool fromFile)
{
    QString path;
    if (fromFile) {
        const QString cart = QString::fromUtf8(smssession_cart_path(m_session));
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Symbols"), cart.isEmpty() ? QString() : QFileInfo(cart).absolutePath(),
            QStringLiteral("Symbol files (*.sym *.map *.noi *.txt);;All files (*)"));
        if (path.isEmpty()) return;
    }
    char msg[512];
    const QByteArray p = path.toLocal8Bit();
    smsdebug_load_symbols(m_dbg, fromFile ? p.constData() : nullptr, msg, sizeof msg);
    const QString text = stripControl(msg);
    appendPrompt(text + QLatin1Char('\n'));
    statusBar()->showMessage(text, 6000);
    refreshAll();
}

QWidget *DebuggerWindow::buildPrompt()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_promptOut = monoView(false);
    m_promptOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_promptOut->setPlainText(QStringLiteral("Debugger prompt. Type 'help' for every command.\n"));
    m_promptIn = new QLineEdit;
    m_promptIn->setPlaceholderText(QStringLiteral("command (help, step, break, bpw, print, mem, poke, runto, disasm, vdp, cart ...)"
                                                  " — Tab completes, Up/Down recall"));
    m_promptIn->installEventFilter(this);
    connect(m_promptIn, &QLineEdit::returnPressed, this, &DebuggerWindow::runPrompt);
    v->addWidget(m_promptOut, 1);
    v->addWidget(m_promptIn);
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    /* the main set on the first row, the shadow set and the interrupt state
     * on the second; edits go in with Enter */
    static const char *const names[17] = { "PC", "SP", "AF", "BC", "DE", "HL", "IX", "IY",
                                           "AF'", "BC'", "DE'", "HL'", "I", "R", "IM", "IFF1", "IFF2" };
    static_assert(SMS_REG_IFF2 - SMS_REG_PC == 16, "the register table follows smsdebug_reg");
    auto *regs = new QGridLayout;
    for (int i = 0; i < 17; ++i) {
        const int row = i < 8 ? 0 : 1, col = 2 * (i < 8 ? i : i - 8);
        regs->addWidget(new QLabel(QString::fromUtf8(names[i])), row, col, Qt::AlignRight);
        m_reg[i] = new QLineEdit;
        m_reg[i]->setMaxLength(6);
        m_reg[i]->setMaximumWidth(64);
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] {
            long val;
            if (parseNum(m_reg[i]->text(), &val)) smsdebug_cpu_set(m_dbg, SMS_REG_PC + i, (int)val);
            m_reg[i]->clearFocus();
            refreshAll();
        });
        regs->addWidget(m_reg[i], row, col + 1);
    }
    regs->setColumnStretch(18, 1);
    v->addLayout(regs);
    auto *flags = new QHBoxLayout;
    static const char *const fnames[6] = { "S", "Z", "H", "P/V", "N", "C" };
    static const int flagIds[6] = { SMS_FLAG_S, SMS_FLAG_Z, SMS_FLAG_H, SMS_FLAG_PV, SMS_FLAG_N, SMS_FLAG_C };
    for (int i = 0; i < 6; ++i) {
        m_flag[i] = new QCheckBox(QString::fromUtf8(fnames[i]));
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) {
            if (smsdebug_is_stopped(m_dbg)) smsdebug_cpu_set(m_dbg, flagIds[i], on);
            refreshAll();
        });
        flags->addWidget(m_flag[i]);
    }
    flags->addStretch();
    v->addLayout(flags);
    m_beam = new QLabel;
    m_beam->setStyleSheet(QStringLiteral("color: gray;"));
    m_beam->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_beam);

    auto *head = new QHBoxLayout;
    head->addWidget(new QLabel(QStringLiteral("Console RAM ($C000-$DFFF)")));
    head->addStretch();
    head->addWidget(new QLabel(QStringLiteral("Go to")));
    m_ramGoto = new QLineEdit;
    m_ramGoto->setPlaceholderText(QStringLiteral("$C000 or label"));
    m_ramGoto->setMaximumWidth(140);
    connect(m_ramGoto, &QLineEdit::returnPressed, this, [this] {
        long a;
        if (!addressOf(m_ramGoto->text(), &a) || a < 0xC000 || a > 0xFFFF) {
            statusBar()->showMessage(QStringLiteral("Not a RAM address ($C000-$DFFF, or its mirror)"), 4000);
            return;
        }
        /* line 0 is the column header */
        m_ram->verticalScrollBar()->setValue(1 + (int)((a & 0x1FFF) / 16));
    });
    head->addWidget(m_ramGoto);
    v->addLayout(head);
    m_ram = monoView(false);
    v->addWidget(m_ram, 1);
    auto *edit = new QHBoxLayout;
    m_ramAddr = new QLineEdit; m_ramAddr->setPlaceholderText(QStringLiteral("$C000")); m_ramAddr->setMaximumWidth(140);
    m_ramVal = new QLineEdit; m_ramVal->setPlaceholderText(QStringLiteral("$00")); m_ramVal->setMaximumWidth(60);
    auto write = [this] {
        long a, val;
        if (!addressOf(m_ramAddr->text(), &a) || !parseNum(m_ramVal->text(), &val)) {
            statusBar()->showMessage(QStringLiteral("Bad address or value"), 4000);
            return;
        }
        smsdebug_write(m_dbg, (uint16_t)a, (uint8_t)val);
        refreshAll();
    };
    connect(m_ramVal, &QLineEdit::returnPressed, this, write);
    connect(m_ramAddr, &QLineEdit::returnPressed, this, write);
    edit->addWidget(new QLabel(QStringLiteral("Poke address (RAM, or below $C000 the cartridge's memory)")));
    edit->addWidget(m_ramAddr);
    edit->addWidget(new QLabel(QStringLiteral("value")));
    edit->addWidget(m_ramVal);
    edit->addStretch();
    v->addLayout(edit);
    return w;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow PC"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::toggled, this, [this](bool on) { if (on) refreshDisasm(); });
    m_jump = new QLineEdit;
    m_jump->setPlaceholderText(QStringLiteral("$address or label"));
    m_jump->setMaximumWidth(160);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(m_jump->text()); });
    row->addWidget(m_followPc);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addWidget(new QLabel(QStringLiteral("Click a line to toggle its breakpoint; scroll to browse")));
    row->addStretch();
    v->addLayout(row);
    m_disasm = monoView(false);
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildVdp()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    auto *left = new QVBoxLayout;
    m_vdpText = monoView(false);
    left->addWidget(m_vdpText, 3);
    left->addWidget(new QLabel(QStringLiteral("Sprites")));
    m_sprites = monoView(false);
    left->addWidget(m_sprites, 2);
    h->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    auto *sel = new QHBoxLayout;
    m_vdpView = new QComboBox;
    m_vdpView->addItems({ QStringLiteral("Name table"), QStringLiteral("Tiles"),
                          QStringLiteral("Sprites"), QStringLiteral("Palette") });
    m_vdpPalette = new QComboBox;
    m_vdpPalette->addItems({ QStringLiteral("Background palette"), QStringLiteral("Sprite palette") });
    m_vdpPalette->setToolTip(QStringLiteral("The palette the Tiles view draws with (Mode 4)"));
    connect(m_vdpView, &QComboBox::currentIndexChanged, this, [this](int) { refreshVdp(); });
    connect(m_vdpPalette, &QComboBox::currentIndexChanged, this, [this](int) { refreshVdp(); });
    sel->addWidget(m_vdpView);
    sel->addWidget(m_vdpPalette);
    sel->addStretch();
    right->addLayout(sel);
    m_vdpPic = new QLabel;
    m_vdpPic->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    auto *scroll = new QScrollArea;
    scroll->setWidget(m_vdpPic);
    scroll->setWidgetResizable(true);
    scroll->setMinimumSize(530, 400);
    right->addWidget(scroll, 1);

    /* CRAM: the background palette over the sprite palette, each entry its
     * own colour; a value typed in goes in with Enter */
    right->addWidget(new QLabel(QStringLiteral("CRAM (background, then sprites; Enter writes a value)")));
    auto *cram = new QGridLayout;
    cram->setSpacing(2);
    for (int i = 0; i < 32; ++i) {
        auto *e = new QLineEdit;
        e->setFixedWidth(30);
        e->setMaxLength(3);
        e->setStyleSheet(QStringLiteral("padding: 0px;"));
        e->setAlignment(Qt::AlignCenter);
        e->setToolTip(QStringLiteral("CRAM $%1").arg(hx(i, 2)));
        connect(e, &QLineEdit::returnPressed, this, [this, i] {
            long val;
            if (parseNum(m_cram[i]->text(), &val)) smsdebug_cram_write(m_dbg, i, (uint8_t)val);
            m_cram[i]->clearFocus();
            refreshAll();
        });
        m_cram[i] = e;
        cram->addWidget(e, i / 16, i % 16);
    }
    cram->setColumnStretch(16, 1);
    right->addLayout(cram);
    h->addLayout(right);
    return w;
}

QWidget *DebuggerWindow::buildSound()
{
    m_sound = monoView(false);
    return m_sound;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Exec"), QStringLiteral("Read"), QStringLiteral("Write"),
                         QStringLiteral("In"), QStringLiteral("Out") });
    m_bpType->setToolTip(QStringLiteral("In and Out take a port ($00-$FF)"));
    m_bpStart = new QLineEdit;
    m_bpStart->setPlaceholderText(QStringLiteral("start ($C000 or label)"));
    m_bpStart->setMaximumWidth(200);
    m_bpEnd = new QLineEdit;
    m_bpEnd->setPlaceholderText(QStringLiteral("end (optional)"));
    m_bpEnd->setMaximumWidth(140);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional, e.g. a == $FF or value & $80)"));
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto doAdd = [this] {
        static const int types[5] = { SMSDEBUG_BP_EXEC, SMSDEBUG_BP_READ, SMSDEBUG_BP_WRITE,
                                      SMSDEBUG_BP_IN, SMSDEBUG_BP_OUT };
        if (m_bpStart->text().trimmed().isEmpty()) return;
        long a, b;
        if (!addressOf(m_bpStart->text(), &a)) { statusBar()->showMessage(QStringLiteral("Bad start address"), 4000); return; }
        b = a;
        if (!m_bpEnd->text().trimmed().isEmpty() && !addressOf(m_bpEnd->text(), &b)) {
            statusBar()->showMessage(QStringLiteral("Bad end address"), 4000);
            return;
        }
        const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
        if (smsdebug_breakpoint_add(m_dbg, types[m_bpType->currentIndex()], (uint16_t)a, (uint16_t)b,
                                    cond.constData()) < 0) {
            statusBar()->showMessage(QStringLiteral("The condition does not parse"), 6000);
            return;
        }
        m_bpStart->clear();
        m_bpEnd->clear();
        m_bpCond->clear();
        refreshAll();
    };
    connect(add, &QPushButton::clicked, this, doAdd);
    connect(m_bpStart, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpEnd, &QLineEdit::returnPressed, this, doAdd);
    connect(m_bpCond, &QLineEdit::returnPressed, this, doAdd);
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    v->addLayout(row);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("ID"), QStringLiteral("Type"),
                                           QStringLiteral("Range"), QStringLiteral("Hits"),
                                           QStringLiteral("Condition") });
    m_bpTable->horizontalHeader()->setStretchLastSection(true);
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        smsdebug_breakpoint_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
    });
    v->addWidget(m_bpTable, 1);

    auto *btns = new QHBoxLayout;
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_bpTable->currentRow();
        if (r < 0) return;
        smsdebug_breakpoint_remove(m_dbg, m_bpTable->item(r, 0)->data(Qt::UserRole).toInt());
        refreshAll();
    });
    auto *clear = new QPushButton(QStringLiteral("Clear all"));
    connect(clear, &QPushButton::clicked, this, [this] {
        smsdebug_breakpoint_clear(m_dbg);
        refreshAll();
    });
    btns->addWidget(remove);
    btns->addWidget(clear);
    btns->addStretch();
    v->addLayout(btns);
    return w;
}

QWidget *DebuggerWindow::buildCart()
{
    m_cart = monoView(false);
    return m_cart;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    int addr;
    const bool stopped = smsdebug_is_stopped(m_dbg) != 0;
    smsdebug_stop_reason(m_dbg, reason, sizeof reason, &addr);
    m_status->setText(stopped ? QStringLiteral("Stopped%1%2").arg(reason[0] ? ": " : "", QString::fromUtf8(reason))
                              : QStringLiteral("Running"));
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: white;").arg(smsAccentColor().name()) : QString());
}

void DebuggerWindow::refreshCpu()
{
    smsdebug_cpu c;
    smsdebug_cpu_get(m_dbg, &c);
    const int vals[17] = { c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy,
                           c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im, c.iff1, c.iff2 };
    for (int i = 0; i < 17; ++i) {
        if (m_reg[i]->hasFocus()) continue;
        /* the pairs as four digits, I and R as two, IM and the IFFs bare */
        m_reg[i]->setText(i < 12 ? hx((unsigned)vals[i] & 0xFFFF, 4)
                          : i < 14 ? hx((unsigned)vals[i] & 0xFF, 2)
                                   : QString::number(vals[i]));
    }
    const int flags[6] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
    for (int i = 0; i < 6; ++i) m_flag[i]->setChecked(flags[i] != 0);
    m_beam->setText(QStringLiteral("vpos %1   hpos %2   V counter $%3   H counter $%4   frame %5   cycles %6%7%8%9")
                        .arg(c.vpos).arg(c.hpos).arg(hx((unsigned)c.vcount & 0xFF, 2), hx((unsigned)c.hcount & 0xFF, 2))
                        .arg(c.frame).arg((qulonglong)c.cycles)
                        .arg(c.int_line ? QStringLiteral("   /INT") : QString(),
                             c.nmi_line ? QStringLiteral("   /NMI") : QString(),
                             c.halted ? QStringLiteral("   HALT") : QString()));
}

void DebuggerWindow::refreshRam()
{
    static uint8_t ram[8192];
    static char buf[64 + 512 * 64];
    smsdebug_ram_get(m_dbg, ram);
    int n = snprintf(buf, sizeof buf, "        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (int row = 0; row < 512; ++row) {
        n += snprintf(buf + n, sizeof buf - n, "$%04X: ", 0xC000 + row * 16);
        for (int col = 0; col < 16; ++col)
            n += snprintf(buf + n, sizeof buf - n, "%02X ", ram[row * 16 + col]);
        buf[n++] = '\n';
    }
    buf[n] = '\0';
    const int scroll = m_ram->verticalScrollBar()->value();
    m_ram->setPlainText(QString::fromLatin1(buf, n));
    m_ram->verticalScrollBar()->setValue(scroll);
}

void DebuggerWindow::refreshDisasm()
{
    static smsdebug_line lines[DISASM_WINDOW];
    if (m_followPc->isChecked()) {
        smsdebug_cpu c;
        smsdebug_cpu_get(m_dbg, &c);
        /* the PC a third of the way down */
        m_disasmTop = (uint16_t)smsdebug_row_address(m_dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
    }
    int pcLine = -1;
    const int n = smsdebug_disassemble(m_dbg, m_disasmTop, lines, DISASM_WINDOW, &pcLine);
    /* the label column is as wide as the longest label shown, within reason */
    int labelWidth = 0;
    for (int i = 0; i < n; ++i) labelWidth = qMax(labelWidth, (int)strlen(lines[i].label));
    labelWidth = qMin(labelWidth, 24);
    m_lineAddr.clear();
    QString text;
    char buf[256];
    for (int i = 0; i < n; ++i) {
        m_lineAddr.push_back(lines[i].address);
        char label[48];
        snprintf(label, sizeof label, "%.*s", labelWidth, lines[i].label);
        /* the gutter: a breakpoint dot, then the PC's arrow */
        snprintf(buf, sizeof buf, " %c %04X  %-11s %-*s %-22s", lines[i].is_pc ? '>' : ' ', lines[i].address,
                 lines[i].bytes, labelWidth, label, lines[i].disasm);
        text += lines[i].has_breakpoint ? QStringLiteral("●") : QStringLiteral(" ");
        text += QString::fromUtf8(buf);
        if (lines[i].comment[0]) text += QStringLiteral("; ") + QString::fromUtf8(lines[i].comment);
        text += QLatin1Char('\n');
    }
    if (n == 0) text = QStringLiteral("(no disassembly: the debugger is not attached)\n");
    m_disasm->setPlainText(text);
    for (int i = 0; i < n; ++i) {
        if (i == pcLine) {
            QTextCursor cur(m_disasm->document()->findBlockByNumber(i));
            cur.select(QTextCursor::LineUnderCursor);
            QTextCharFormat fmt;
            fmt.setBackground(smsAccentColor());
            fmt.setForeground(Qt::white);
            cur.setCharFormat(fmt);
        } else if (lines[i].has_breakpoint) {
            QTextCursor cur(m_disasm->document()->findBlockByNumber(i));
            cur.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            QTextCharFormat fmt;
            fmt.setForeground(smsAccentColor());
            cur.setCharFormat(fmt);
        }
    }
}

void DebuggerWindow::refreshVdp()
{
    smsdebug_vdp p;
    smsdebug_vdp_get(m_dbg, &p);
    QString s;
    /* each register's line names it and its value, then the fields */
    char desc[160];
    for (int r = 0; r < 11; ++r) {
        smsdebug_vdp_describe_register(m_dbg, r, desc, sizeof desc);
        s += QString::fromUtf8(desc) + QLatin1Char('\n');
    }
    s += QStringLiteral("\nVDP          %1 (%2)   %3\n")
        .arg(p.kind_5246 ? QStringLiteral("315-5246") : QStringLiteral("315-5124"),
             p.kind_5246 ? QStringLiteral("Master System II") : QStringLiteral("Master System / Mark III"),
             p.is_pal ? QStringLiteral("PAL") : QStringLiteral("NTSC"));
    s += QStringLiteral("Mode         %1 (%2), %3 lines\n")
        .arg(QString::fromUtf8(p.mode_name ? p.mode_name : "?")).arg(p.mode).arg(p.y_pixels);
    s += QStringLiteral("Status       $%1   address $%2   code %3   read buffer $%4%5\n")
        .arg(hx(p.status, 2), hx(p.addr, 4)).arg(p.code).arg(hx(p.buffer, 2))
        .arg(p.second_byte ? QStringLiteral("   (control write half done)") : QString());
    s += QStringLiteral("Line counter $%1   H counter $%2\n").arg(hx(p.line_counter, 2), hx(p.hcounter, 2));
    s += QStringLiteral("Display      %1   VINT %2   HINT %3   sprites %4%5\n")
        .arg(p.display_on ? "on" : "off", p.vint_on ? "on" : "off", p.hint_on ? "on" : "off",
             p.sprites_16 ? "8x16" : "8x8", p.sprites_zoom ? " zoomed" : "");
    QStringList m4;
    if (p.left_column_blank) m4 << QStringLiteral("left column blank");
    if (p.hscroll_lock_top) m4 << QStringLiteral("H-scroll lock (top)");
    if (p.vscroll_lock_right) m4 << QStringLiteral("V-scroll lock (right)");
    if (p.sprite_shift) m4 << QStringLiteral("sprites shifted left");
    if (!m4.isEmpty()) s += QStringLiteral("             %1\n").arg(m4.join(QStringLiteral("   ")));
    s += QStringLiteral("Tables       name $%1   SAT $%2   sprite patterns $%3\n")
        .arg(hx(p.name_base, 4), hx(p.sat_base, 4), hx(p.sprite_pattern_base, 4));
    if (p.mode != 4)
        s += QStringLiteral("             colour $%1   pattern $%2\n").arg(hx(p.color_base, 4), hx(p.pattern_base, 4));
    s += QStringLiteral("Scroll       X $%1   Y $%2   backdrop %3\n")
        .arg(hx(p.scroll_x, 2), hx(p.scroll_y, 2)).arg(p.backdrop);
    s += QStringLiteral("Pending      VINT %1   HINT %2   /INT %3   /NMI %4   Pause held %5\n")
        .arg(yesNo(p.vint_pending), yesNo(p.hint_pending), yesNo(p.int_line), yesNo(p.nmi_line), yesNo(p.pause_held));
    s += QStringLiteral("Beam         vpos %1   hpos %2   V counter $%3   frame %4\n")
        .arg(p.vpos).arg(p.hpos).arg(hx((unsigned)p.vcount & 0xFF, 2)).arg(p.frame);
    const int vscroll = m_vdpText->verticalScrollBar()->value();
    m_vdpText->setPlainText(s);
    m_vdpText->verticalScrollBar()->setValue(vscroll);

    /* the sprite list: colour and the early clock only mean something in
     * the TMS9918 modes */
    smsdebug_sprite spr[64];
    const int ns = smsdebug_sprites_get(m_dbg, spr);
    const bool tms = p.mode != 4;
    QString o = tms ? QStringLiteral(" #    Y    X  tile  colour  EC  visible\n")
                    : QStringLiteral(" #    Y    X  tile  visible\n");
    for (int i = 0; i < ns; ++i) {
        o += QStringLiteral("%1  %2  %3  $%4").arg(i, 2).arg(spr[i].y, 3).arg(spr[i].x, 3).arg(hx((unsigned)spr[i].tile, 3));
        if (tms) o += QStringLiteral("   %1      %2").arg(spr[i].color, 2).arg(spr[i].early_clock ? "y" : "n");
        o += QStringLiteral("   %1\n").arg(spr[i].visible ? "yes" : "no");
    }
    const int sscroll = m_sprites->verticalScrollBar()->value();
    m_sprites->setPlainText(o);
    m_sprites->verticalScrollBar()->setValue(sscroll);

    const int view = m_vdpView->currentIndex();
    m_vdpPalette->setEnabled(view == SMSDEBUG_VIEW_TILES && !tms);
    int w = 0, h = 0;
    if (smsdebug_vdp_view(m_dbg, view, m_vdpPalette->currentIndex(), m_vdpPx.data(), &w, &h) && w > 0 && h > 0) {
        QImage img(reinterpret_cast<const uchar *>(m_vdpPx.data()), w, h, w * 4, QImage::Format_RGB32);
        /* 2x nearest */
        m_vdpPic->setPixmap(QPixmap::fromImage(img.scaled(w * 2, h * 2, Qt::IgnoreAspectRatio,
                                                          Qt::FastTransformation)));
    }

    for (int i = 0; i < 32; ++i) {
        QLineEdit *e = m_cram[i];
        const uint32_t rgb = p.cram_rgb[i] & 0xFFFFFF;
        if (e->property("rgb").toUInt() != rgb + 1) {
            /* readable text on any swatch */
            const int luma = (int)(((rgb >> 16) & 0xFF) * 299 + ((rgb >> 8) & 0xFF) * 587 + (rgb & 0xFF) * 114) / 1000;
            e->setStyleSheet(QStringLiteral("padding: 0px; border: 1px solid gray; background: #%1; color: %2;")
                                 .arg(rgb, 6, 16, QLatin1Char('0'))
                                 .arg(luma > 128 ? QStringLiteral("black") : QStringLiteral("white")));
            e->setProperty("rgb", rgb + 1);
        }
        if (!e->hasFocus()) e->setText(hx(p.cram[i], 2));
    }
}

void DebuggerWindow::refreshSound()
{
    smsdebug_io a;
    smsdebug_io_get(m_dbg, &a);
    QString s;
    s += QStringLiteral("PSG (SN76489)          %1\n").arg(a.psg_audible ? "audible" : "muted");
    for (int i = 0; i < 3; ++i)
        s += QStringLiteral("  Tone %1   period $%2 (%3)   volume %4%5\n")
            .arg(i).arg(hx((unsigned)a.tone_period[i] & 0x3FF, 3)).arg(a.tone_period[i], 4).arg(a.tone_volume[i], 2)
            .arg(a.tone_volume[i] == 15 ? QStringLiteral(" (off)") : QString());
    static const char *const rates[4] = { "N/512", "N/1024", "N/2048", "tone 2" };
    s += QStringLiteral("  Noise    %1, rate %2   volume %3%4   LFSR $%5\n")
        .arg(a.noise_mode ? "white" : "periodic", rates[a.noise_rate & 3]).arg(a.noise_volume, 2)
        .arg(a.noise_volume == 15 ? QStringLiteral(" (off)") : QString(), hx(a.lfsr, 4));
    s += QStringLiteral("  (volume 0 is the loudest, 15 off)\n\n");

    if (a.fm_present) {
        s += QStringLiteral("YM2413 (FM)            %1\n").arg(a.fm_audible ? "audible" : "muted");
        s += QStringLiteral("        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
        for (int row = 0; row < 4; ++row) {
            s += QStringLiteral("  $%1: ").arg(hx(row * 16, 2));
            for (int col = 0; col < 16; ++col) s += hx(a.fm_regs[row * 16 + col], 2) + QLatin1Char(' ');
            s += QLatin1Char('\n');
        }
    } else {
        s += QStringLiteral("YM2413 (FM)            not present\n");
    }

    s += QStringLiteral("\nMemory control $3E     $%1   cartridge %2   BIOS %3   work RAM %4   I/O chip %5\n")
        .arg(hx(a.mem_ctrl, 2), a.cart_enabled ? "on" : "off", a.bios_enabled ? "on" : "off",
             a.ram_enabled ? "on" : "off", a.io_enabled ? "on" : "off");
    s += QStringLiteral("I/O control $3F        $%1\n").arg(hx(a.io_ctrl, 2));
    s += QStringLiteral("BIOS                   %1").arg(a.bios_present ? "present" : "none");
    if (a.bios_present)
        s += QStringLiteral("   pages $%1 $%2 $%3").arg(hx(a.bios_page[0], 2), hx(a.bios_page[1], 2), hx(a.bios_page[2], 2));
    s += QLatin1Char('\n');
    s += QStringLiteral("Mapper ($FFFC-$FFFF)   $%1 $%2 $%3 $%4\n")
        .arg(hx(a.mapper[0], 2), hx(a.mapper[1], 2), hx(a.mapper[2], 2), hx(a.mapper[3], 2));
    s += QStringLiteral("Ports                  $DC = $%1   $DD = $%2\n").arg(hx(a.port_dc, 2), hx(a.port_dd, 2));
    static const char *const names[6] = { "Up", "Down", "Left", "Right", "1", "2" };
    for (int port = 0; port < 2; ++port) {
        s += QStringLiteral("Player %1               $%2 ").arg(port + 1).arg(hx(a.pad[port], 2));
        for (int b = 0; b < 6; ++b)
            if (a.pad[port] & (1u << b)) s += QLatin1Char(' ') + QString::fromUtf8(names[b]);
        s += QLatin1Char('\n');
    }
    s += QStringLiteral("Pause held             %1   Reset held %2\n").arg(yesNo(a.pause_held), yesNo(a.reset_held));
    s += QStringLiteral("Nationality            %1\n").arg(a.japanese ? "Japanese" : "export");
    s += QStringLiteral("Console                %1\n").arg(QString::fromUtf8(a.console_name ? a.console_name : "?"));
    const int scroll = m_sound->verticalScrollBar()->value();
    m_sound->setPlainText(s);
    m_sound->verticalScrollBar()->setValue(scroll);
}

void DebuggerWindow::refreshBps()
{
    smsdebug_breakpoint bps[256];
    const int n = smsdebug_breakpoint_list(m_dbg, bps, 256);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        m_bpTable->setItem(i, 0, on);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(QString::number(bps[i].id)));
        QStringList type;
        if (bps[i].type & SMSDEBUG_BP_EXEC) type << QStringLiteral("Exec");
        if (bps[i].type & SMSDEBUG_BP_READ) type << QStringLiteral("Read");
        if (bps[i].type & SMSDEBUG_BP_WRITE) type << QStringLiteral("Write");
        if (bps[i].type & SMSDEBUG_BP_IN) type << QStringLiteral("In");
        if (bps[i].type & SMSDEBUG_BP_OUT) type << QStringLiteral("Out");
        m_bpTable->setItem(i, 2, new QTableWidgetItem(type.join(QLatin1Char('/'))));
        /* a port range for In/Out is two digits */
        const int digits = (bps[i].type & (SMSDEBUG_BP_IN | SMSDEBUG_BP_OUT)) ? 2 : 4;
        QString range = QStringLiteral("$") + hx(bps[i].start, digits);
        if (bps[i].end != bps[i].start) range += QStringLiteral("-$") + hx(bps[i].end, digits);
        m_bpTable->setItem(i, 3, new QTableWidgetItem(range));
        m_bpTable->setItem(i, 4, new QTableWidgetItem(QString::number(bps[i].hits)));
        m_bpTable->setItem(i, 5, new QTableWidgetItem(QString::fromUtf8(bps[i].condition)));
    }
    if (n > 0) m_bpTable->resizeColumnsToContents();
    m_bpFilling = false;
}

void DebuggerWindow::refreshCart()
{
    smsdebug_cart c;
    char info[256];
    smsdebug_cart_get(m_dbg, &c);
    smsdebug_cart_info(m_dbg, info, sizeof info);
    QString s = QString::fromUtf8(info) + QStringLiteral("\n\n");
    if (!c.present) {
        const int scroll = m_cart->verticalScrollBar()->value();
        m_cart->setPlainText(s);
        m_cart->verticalScrollBar()->setValue(scroll);
        return;
    }
    s += QStringLiteral("Mode          %1 (%2)%3\n").arg(QString::fromUtf8(c.mode_name ? c.mode_name : "?")).arg(c.mode)
        .arg(c.direct ? QStringLiteral(", an opened cartridge file") : QString());
    s += QStringLiteral("Booted game   %1\n").arg(yesNo(c.booted_game));
    if (c.mapper >= 0) {
        s += QStringLiteral("Mapper        %1 (%2)   banks").arg(QString::fromUtf8(c.mapper_name)).arg(c.mapper);
        for (int i = 0; i < 6; ++i) s += QStringLiteral(" $%1").arg(hx(c.bank[i], 2));
        s += QLatin1Char('\n');
    } else {
        s += QStringLiteral("Mapper        none (CONFIG)\n");
    }
    s += QStringLiteral("RAM           %1%2   %3 bytes\n").arg(c.ram_enabled ? "enabled" : "off",
                                                          c.ram_writable ? ", writable" : "").arg(c.ram_size);
    s += QStringLiteral("Image         %1 bytes   CRC-32 %2   claim %3\n")
        .arg(c.image_size).arg(hx(c.image_crc, 8)).arg(c.claim);
    s += QStringLiteral("Link          %1%2\n\n").arg(c.link_up ? "up" : "down",
                                                     c.busy ? QStringLiteral(" (transaction in flight)") : QString());
    s += QStringLiteral("Mailbox       ACKSEQ $%1   STATUS $%2   last error %3   reply cmd $%4   RXLEN %5\n")
        .arg(hx(c.ackseq, 2), hx(c.status, 2)).arg(c.last_error).arg(hx(c.reply_cmd, 2)).arg(c.rxlen);
    s += QStringLiteral("Boot          state %1   %2%   error %3   %4 of %5 bytes\n")
        .arg(c.boot_state).arg(c.boot_pct).arg(c.boot_err).arg(c.boot_got).arg(c.boot_total);
    s += QStringLiteral("Load          state %1   window %2 of %3   %4%\n")
        .arg(c.load_state).arg(c.load_win).arg(c.load_nwin).arg(c.load_pct);
    s += QStringLiteral("BIOS snoop    phase %1   $C000 = $%2   $3E = $%3   $3F = $%4\n")
        .arg(c.bios_phase).arg(hx(c.snoop_c000, 2), hx(c.snoop_3e, 2), hx(c.snoop_3f, 2));
    s += QStringLiteral("              VDP");
    for (int i = 0; i < 11; ++i) s += QStringLiteral(" %1").arg(hx(c.snoop_vdp[i], 2));
    s += QStringLiteral("\nQueue depth   %1\n\n").arg(c.queue_depth);
    /* which 8K SRAM bank backs each 1K page of $0000-$BFFF; -- is the
     * cartridge's own memory */
    s += QStringLiteral("Page -> SRAM bank\n");
    for (int row = 0; row < 6; ++row) {
        s += QStringLiteral("  $%1:").arg(hx(row * 0x2000, 4));
        for (int col = 0; col < 8; ++col) {
            const int b = c.page_bank[row * 8 + col];
            s += b < 0 ? QStringLiteral(" --") : QStringLiteral(" %1").arg(hx((unsigned)b, 2));
        }
        s += QLatin1Char('\n');
    }
    const int scroll = m_cart->verticalScrollBar()->value();
    m_cart->setPlainText(s);
    m_cart->verticalScrollBar()->setValue(scroll);
}

void DebuggerWindow::refreshAll()
{
    refreshStatus(); refreshCpu(); refreshRam(); refreshDisasm(); refreshVdp(); refreshSound();
    refreshBps(); refreshCart();
}

/* Every smsdebug call takes the host's run lock, so the live tabs are safe
 * to refresh while the machine runs: twice a second. */
void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    const unsigned gen = smsdebug_generation(m_dbg);
    const bool stopped = smsdebug_is_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        m_seenGen = gen; m_wasStopped = stopped;
        refreshAll();
    } else if (!stopped && ++m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(); refreshCpu(); refreshRam(); refreshVdp(); refreshSound(); refreshCart();
    }
}

/* ---- the prompt and the rest ----------------------------------------------- */

void DebuggerWindow::appendPrompt(const QString &text)
{
    m_promptOut->moveCursor(QTextCursor::End);
    m_promptOut->insertPlainText(text);
    m_promptOut->verticalScrollBar()->setValue(m_promptOut->verticalScrollBar()->maximum());
}

void DebuggerWindow::runPrompt()
{
    static char out[65536];
    const QString cmd = m_promptIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = m_history.size();
    appendPrompt(QStringLiteral("> %1\n").arg(cmd));
    smsdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    appendPrompt(stripControl(out) + QLatin1Char('\n'));
    m_promptIn->clear();
    refreshAll();
}

/* A label, or a number as parseNum reads it. */
bool DebuggerWindow::addressOf(const QString &text, long *out)
{
    const int lbl = smsdebug_label_address(m_dbg, text.trimmed().toUtf8().constData());
    if (lbl >= 0) { *out = lbl; return true; }
    return parseNum(text, out);
}

void DebuggerWindow::jumpTo(const QString &text)
{
    long a;
    if (!addressOf(text, &a)) { statusBar()->showMessage(QStringLiteral("No such address or label"), 4000); return; }
    m_followPc->setChecked(false);
    m_disasmTop = (uint16_t)smsdebug_row_address(m_dbg, (uint16_t)(a & 0xFFFF), -(DISASM_WINDOW / 3));
    refreshDisasm();
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_promptIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Tab) {
            const QString text = m_promptIn->text();
            const int sp = text.lastIndexOf(QLatin1Char(' '));
            const QString word = sp >= 0 ? text.mid(sp + 1) : text;
            char comps[4096];
            const int n = smsdebug_completions(m_dbg, word.toUtf8().constData(), comps, sizeof comps);
            if (n == 1) {
                QString c = QString::fromUtf8(comps).section(QLatin1Char('\n'), 0, 0);
                m_promptIn->setText(text.left(sp + 1) + c + QLatin1Char(' '));
            } else if (n > 1) {
                appendPrompt(QString::fromUtf8(comps));
            }
            return true;
        }
        /* the history: Up for older, Down back towards an empty line */
        if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
            if (ke->key() == Qt::Key_Up && m_historyPos > 0) --m_historyPos;
            else if (ke->key() == Qt::Key_Down && m_historyPos < m_history.size()) ++m_historyPos;
            m_promptIn->setText(m_historyPos < m_history.size() ? m_history.at(m_historyPos) : QString());
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(e);
            const int line = m_disasm->cursorForPosition(me->pos()).blockNumber();
            if (line >= 0 && line < (int)m_lineAddr.size()) {
                smsdebug_breakpoint_toggle(m_dbg, m_lineAddr[line]);
                refreshDisasm();
                refreshBps();
            }
            return true;
        }
        if (e->type() == QEvent::Wheel) {
            auto *we = static_cast<QWheelEvent *>(e);
            const int rows = -we->angleDelta().y() / 40;
            if (rows != 0) {
                m_followPc->setChecked(false);
                m_disasmTop = (uint16_t)smsdebug_row_address(m_dbg, m_disasmTop, rows);
                refreshDisasm();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); return;
    case Qt::Key_F7: stepAnd(smsdebug_step); return;
    case Qt::Key_F8:
        stepAnd((e->modifiers() & Qt::ShiftModifier) ? smsdebug_step_out : smsdebug_step_over);
        return;
    case Qt::Key_F12: hide(); return;
    default: QMainWindow::keyPressEvent(e);
    }
}

/* Hidden by F12, the close button or the main window: the engine goes
 * away and the machine runs on. The close button is a plain close, which
 * only hides the window (it is not deleted, so it comes back as it was);
 * ignoring the close instead would make QApplication's quit, which closes
 * every window first, give up whenever it reached this one. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    smsdebug_detach(m_dbg);
    QMainWindow::hideEvent(e);
}
