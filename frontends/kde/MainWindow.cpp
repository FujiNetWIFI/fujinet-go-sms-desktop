/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QUrl>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "debugger/DebuggerWindow.h"
#include "controllers/ControllersWindow.h"

static const char *const kCartFilter =
    "Master System / SG-1000 cartridges (*.sms *.sg *.bin *.rom *.SMS *.SG *.BIN *.ROM);;"
    "All files (*)";

/* How long a menu click holds a console button: the console samples its
 * buttons once a frame, so a press and release in the same frame would
 * never be seen. A few frames, as a finger would. */
static constexpr int kTapMs = 100;

MainWindow::MainWindow(smssession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go SMS"));
    /* 306x224 (268x224 at 8:7 pixels) at 3x. */
    resize(918, 672 + 60);
    setAcceptDrops(true);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);

    m_dot = new QLabel(QStringLiteral("●"));
    m_status = new QLabel(QStringLiteral("Starting..."));
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status);

    buildMenus();
    applyPicture();

    connect(&m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatus);
    m_statusTimer.start(1000);
    updateStatus();
    /* The gamepad thread cannot call into Qt; it posts system actions and
     * this timer takes them. */
    connect(&m_sysactTimer, &QTimer::timeout, this, &MainWindow::drainSysactions);
    m_sysactTimer.start(100);
    /* Gamepads come and go on their own thread; say so when they do. */
    m_padGen = smssession_gamepad_generation(session);
    connect(&m_padTimer, &QTimer::timeout, this, &MainWindow::pollGamepads);
    m_padTimer.start(250);

    if (qEnvironmentVariableIsSet("SMS_OPEN_CONTROLLERS"))
        toggleControllers();
    /* once the event loop runs, i.e. after main() has started the session:
     * attaching before the machine exists would not stop it */
    if (qEnvironmentVariableIsSet("SMS_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { DebuggerWindow::showFor(this, m_session); });
    if (qEnvironmentVariableIsSet("SMS_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Open Cartridge..."), QKeySequence(Qt::CTRL | Qt::Key_O), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Cartridge"), QString(), QString::fromUtf8(kCartFilter));
        if (!f.isEmpty()) openCart(f);
    });
    machine->addAction(QStringLiteral("&Eject Cartridge"), this, [this] {
        smssession_eject(m_session);
        statusBar()->showMessage(QStringLiteral("Cartridge ejected — back to CONFIG"), 4000);
    });
    machine->addAction(QStringLiteral("&Import Cartridge to SD..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import Cartridge to SD"), QString(), QString::fromUtf8(kCartFilter));
        if (f.isEmpty()) return;
        char dest[1024];
        if (smssession_import_cart_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) != 0)
            QMessageBox::warning(this, QStringLiteral("Import failed"),
                                 QString::fromUtf8(smssession_last_error(m_session)));
        else
            statusBar()->showMessage(QStringLiteral("%1 is on the SD host — boot it from the CONFIG client")
                                     .arg(QFileInfo(QString::fromUtf8(dest)).fileName()), 6000);
    });
    machine->addAction(QStringLiteral("Import &BIOS..."), this,
                       [this] { SettingsDialog::importBios(this, m_session); });
    machine->addSeparator();
    /* The console's buttons and the session's resets act through their key
     * bindings (Return, Backspace, F3 and Escape by default, remappable);
     * the menu shows the bound key, it does not claim it as a shortcut, so
     * the binding stays the one that acts. */
    m_pauseAction = machine->addAction(QStringLiteral("&Pause"), this, [this] { tapSwitch(SMS_SW_PAUSE); });
    m_resetButtonAction = machine->addAction(QStringLiteral("&Reset Button"), this,
                                             [this] { tapSwitch(SMS_SW_RESET); });
    m_softResetAction = machine->addAction(QStringLiteral("&Soft Reset"), this,
                                           [this] { runSysaction(SMS_SYSACT_SOFT_RESET); });
    m_resetConfigAction = machine->addAction(QStringLiteral("Reset to &CONFIG"), this,
                                             [this] { runSysaction(SMS_SYSACT_RESET_CONFIG); });
    connect(machine, &QMenu::aboutToShow, this, &MainWindow::syncMachineMenu);
    syncMachineMenu();
    machine->addSeparator();
    machine->addAction(QStringLiteral("P&references..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                       &MainWindow::showSettings);
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, [this] { close(); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Controllers"), QKeySequence(Qt::Key_F9), this,
                    &MainWindow::toggleControllers);
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12), this,
                    [this] { DebuggerWindow::toggleFor(this, m_session); });
    view->addSeparator();
    m_tvAction = view->addAction(QStringLiteral("&TV Aspect"));
    m_tvAction->setCheckable(true);
    m_tvAction->setToolTip(QStringLiteral("Show the picture at the television's pixel aspect "
                                          "(8:7 on NTSC, about 1.39 on PAL); off shows square pixels"));
    connect(m_tvAction, &QAction::triggered, this, [this](bool on) {
        /* the family's "aspect": 0 the TV's pixel aspect, 1 square pixels */
        smssession_set_int(m_session, "aspect", on ? 0 : 1);
        applyPicture();
    });
    m_smoothAction = view->addAction(QStringLiteral("&Smooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) {
        smssession_set_int(m_session, "smooth", on ? 1 : 0);
        applyPicture();
    });
    view->addAction(QStringLiteral("&Fullscreen"), QKeySequence(Qt::Key_F11), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Web UI"), this, [this] {
        if (!smssession_fujinet_running(m_session)) {
            statusBar()->showMessage(QStringLiteral("FujiNet is not running"), 4000);
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] { fujinet_log_show(this, m_session); });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About FujiNet Go SMS"), this, &MainWindow::showAbout);
}

/* The bound key of each console control as the menu's shortcut text (it
 * follows a rebinding), and Reset Button only where the console has one:
 * the Master System II and the Mark III have none. */
void MainWindow::syncMachineMenu()
{
    auto label = [this](QAction *a, const QString &text, int target) {
        const sms_binding b = smssession_binding_get(m_session, target);
        char key[32] = "";
        if (b.keysym) smssession_keysym_name(b.keysym, key, sizeof key);
        a->setText(key[0] ? text + QLatin1Char('\t') + QString::fromUtf8(key) : text);
    };
    label(m_pauseAction, QStringLiteral("&Pause"), SMS_TARGET_SWITCH(SMS_SW_PAUSE));
    label(m_resetButtonAction, QStringLiteral("&Reset Button"), SMS_TARGET_SWITCH(SMS_SW_RESET));
    label(m_softResetAction, QStringLiteral("&Soft Reset"), SMS_TARGET_SYSACT(SMS_SYSACT_SOFT_RESET));
    label(m_resetConfigAction, QStringLiteral("Reset to &CONFIG"), SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG));
    /* the console running now; the setting runs ahead of it while
     * Settings has a change pending */
    const int console = smssession_console(m_session);
    m_resetButtonAction->setEnabled(sms_console_has_reset_button(console) != 0);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go SMS"),
        QStringLiteral("<b>FujiNet Go SMS</b> %1<br><br>"
                       "A Sega Master System with a built-in FujiNet.<br>"
                       "The machine is transposed to C from MAME's Master System drivers "
                       "(BSD-3-Clause), with floooh's z80.h (zlib) for the Z80 and ymfm "
                       "(BSD-3-Clause) for the YM2413, and the FujiNet SMS cartridge running "
                       "the FujiNet firmware's own sources (GPL-3.0-or-later).<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a>")
            .arg(QStringLiteral(SMS_VERSION_STRING)));
}

void MainWindow::showSettings()
{
    const bool restart = SettingsDialog::run(this, m_session);
    applyPicture();
    syncMachineMenu();
    if (restart) restartSession();
}

void MainWindow::applyPicture()
{
    const bool tv = smssession_get_int(m_session, "aspect", 0) == 0;
    const bool smooth = smssession_get_int(m_session, "smooth", 0) != 0;
    m_display->setTvAspect(tv);
    m_display->setSmooth(smooth);
    if (m_tvAction) m_tvAction->setChecked(tv);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

void MainWindow::pollGamepads()
{
    const unsigned gen = smssession_gamepad_generation(m_session);
    if (gen == m_padGen) return;
    m_padGen = gen;
    char msg[160];
    if (smssession_gamepad_last_event(m_session, msg, sizeof msg) > 0)
        statusBar()->showMessage(QString::fromUtf8(msg), 4000);
}

/* A machine option changed: a power cycle with the new settings (a full
 * restart when FujiNet, audio or the gamepads were switched). */
void MainWindow::restartSession()
{
    if (smssession_restart(m_session) != 0) {
        QMessageBox::warning(this, QStringLiteral("Restart failed"),
                             QString::fromUtf8(smssession_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Machine options applied (power cycled)"), 5000);
}

void MainWindow::toggleControllers()
{
    if (!m_controllers) m_controllers = new ControllersWindow(m_session, this);
    if (m_controllers->isVisible()) m_controllers->hide();
    else m_controllers->show();
}

/* A console button from the menu: pressed, then released a few frames
 * later. */
void MainWindow::tapSwitch(int sw)
{
    const int target = SMS_TARGET_SWITCH(sw);
    smssession_press(m_session, target, 1);
    QTimer::singleShot(kTapMs, this, [this, target] { smssession_press(m_session, target, 0); });
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case SMS_SYSACT_RESET_CONFIG:
        smssession_sysaction(m_session, sa);
        statusBar()->showMessage(QStringLiteral("Back to the FujiNet CONFIG client"), 4000);
        break;
    case SMS_SYSACT_SOFT_RESET:
        smssession_sysaction(m_session, sa);
        statusBar()->showMessage(QStringLiteral("Soft reset"), 2000);
        break;
    case SMS_SYSACT_DEBUG_STOP:
        DebuggerWindow::showFor(this, m_session);
        break;
    default: break;
    }
}

void MainWindow::drainSysactions()
{
    int sa;
    while (smssession_sysaction_take(m_session, &sa)) runSysaction(sa);
}

void MainWindow::updateStatus()
{
    QString text;
    bool on = false;
    const QString cart = QString::fromUtf8(smssession_cart_path(m_session));
    if (!smssession_is_running(m_session)) {
        text = QStringLiteral("Stopped");
    } else {
        char st[160];
        smssession_cart_status(m_session, st, sizeof st);
        on = smssession_cart_link_up(m_session) == 1;
        text = QStringLiteral("FujiNet: %1").arg(QString::fromUtf8(st));
        if (!cart.isEmpty())
            text = QStringLiteral("%1 — %2").arg(QFileInfo(cart).fileName(), text);
        else if (!smssession_cart_booted_game(m_session))
            text = QStringLiteral("CONFIG — %1").arg(text);
    }
    m_status->setText(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(smsAccentColor().name())
                            : QStringLiteral("color: gray;"));
}

void MainWindow::openCart(const QString &path)
{
    if (smssession_load_cart(m_session, path.toLocal8Bit().constData()) != 0) {
        QMessageBox::warning(this, QStringLiteral("Could not open"),
                             QString::fromUtf8(smssession_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Running %1").arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::loadMedia(const QString &path)
{
    const QByteArray p = path.toLocal8Bit();
    /* a BIOS first: one may well be named .sms */
    if (smssession_media_is_bios(p.constData())) {
        SettingsDialog::importBios(this, m_session, path);
        return;
    }
    if (smssession_media_is_cartridge(p.constData())) {
        openCart(path);
        return;
    }
    char dest[1024];
    if (smssession_import_media(m_session, p.constData(), dest, sizeof dest) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QString::fromUtf8(smssession_last_error(m_session)));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Copied to FujiNet's SD folder — mount it from the CONFIG client"), 6000);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_F9) { toggleControllers(); return; }
    if (e->key() == Qt::Key_F12) { DebuggerWindow::toggleFor(this, m_session); return; }
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) { QMainWindow::keyPressEvent(e); return; }

    const uint32_t ks = smsKeysymFromQt(e);
    if (!ks) { QMainWindow::keyPressEvent(e); return; }

    const int sa = smssession_key_sysaction(m_session, ks);
    if (sa >= 0) {
        if (!m_sysactDown[sa]) { m_sysactDown[sa] = true; runSysaction(sa); }
        return;
    }
    if (!smssession_key(m_session, ks, 1)) QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = smsKeysymFromQt(e);
    if (!ks) { QMainWindow::keyReleaseEvent(e); return; }
    const int sa = smssession_key_sysaction(m_session, ks);
    if (sa >= 0) { m_sysactDown[sa] = false; return; }
    if (!smssession_key(m_session, ks, 0)) QMainWindow::keyReleaseEvent(e);
}

bool MainWindow::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate) {
        smssession_release_all(m_session);
        for (bool &d : m_sysactDown) d = false;
    }
    return QMainWindow::event(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty()) loadMedia(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_sysactTimer.stop();
    m_padTimer.stop();
    QMainWindow::closeEvent(e);
}
