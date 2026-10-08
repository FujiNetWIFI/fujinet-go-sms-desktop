/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"

/* ---- PadButton ------------------------------------------------------------ */

PadButton::PadButton(const QString &face, int target, QWidget *parent)
    : QPushButton(face, parent), m_target(target), m_face(face)
{
    setFocusPolicy(Qt::NoFocus);
}

void PadButton::setHeld(bool held)
{
    if (held == m_held) return;
    m_held = held;
    setStyleSheet(held ? QStringLiteral("background: %1; color: white;").arg(smsAccentColor().name())
                       : QString());
}

void PadButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !m_down) {
        m_down = true;
        emit pressedTarget(m_target);
    }
    QPushButton::mousePressEvent(e);
}

void PadButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::mouseReleaseEvent(e);
}

/* Dragging off a button must release it. */
void PadButton::leaveEvent(QEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::leaveEvent(e);
}

/* ---- ControllersWindow ------------------------------------------------------ */

/* Fixed button sizes, wide enough for a Map-mode label ("Backspace /
 * Back"), so the panel is the same shape in both modes. */
static constexpr int kButtonWidth = 112;
static constexpr int kButtonHeight = 40;

ControllersWindow::ControllersWindow(smssession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a
     * panel of fixed-size buttons. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                      | Qt::CustomizeWindowHint), m_session(session)
{
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    auto *ports = new QHBoxLayout;
    ports->addWidget(buildController(0));
    ports->addWidget(buildController(1));
    root->addLayout(ports);

    /* The console's own buttons: Pause (the NMI) and Reset are machine
     * controls like any other -- Reset only on the consoles that have one;
     * Reset to CONFIG is the power switch of a FujiNet cartridge. */
    auto *console = new QGroupBox(QStringLiteral("Console"));
    auto *crow = new QHBoxLayout(console);
    crow->addStretch();
    PadButton *pause = control(QStringLiteral("Pause"), SMS_TARGET_SWITCH(SMS_SW_PAUSE));
    m_reset = control(QStringLiteral("Reset"), SMS_TARGET_SWITCH(SMS_SW_RESET));
    PadButton *config = control(QStringLiteral("Reset to CONFIG"), SMS_TARGET_SYSACT(SMS_SYSACT_RESET_CONFIG));
    const int cw = qMax(kButtonWidth, config->fontMetrics().horizontalAdvance(config->face()) + 24);
    pause->setFixedWidth(cw);
    m_reset->setFixedWidth(cw);
    config->setFixedWidth(cw);
    crow->addWidget(pause);
    crow->addWidget(m_reset);
    crow->addWidget(config);
    crow->addStretch();
    root->addWidget(console);

    /* Gamepads: one row per pad, rebuilt as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to players in connection order unless chosen here)"));
    m_padForm = new QFormLayout(pads);
    root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        smssession_bindings_reset(m_session);
        refreshLabels();
    });
    m_hint = new QLabel;
    m_hint->setStyleSheet(QStringLiteral("color: gray;"));
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    root->addLayout(maprow);

    connect(&m_captureTimer, &QTimer::timeout, this, &ControllersWindow::pollCapture);
    connect(&m_pollTimer, &QTimer::timeout, this, &ControllersWindow::poll);
    rebuildPads();
    setMapState(-2);
}

PadButton *ControllersWindow::control(const QString &face, int target)
{
    auto *b = new PadButton(face, target);
    b->setFixedSize(kButtonWidth, kButtonHeight);
    connect(b, &PadButton::pressedTarget, this, &ControllersWindow::onPressed);
    connect(b, &PadButton::releasedTarget, this, &ControllersWindow::onReleased);
    m_controls.push_back(b);
    return b;
}

QWidget *ControllersWindow::buildController(int port)
{
    auto *box = new QGroupBox(port ? QStringLiteral("Player 2") : QStringLiteral("Player 1"));
    auto *v = new QVBoxLayout(box);

    /* The joypad's own layout: the direction pad on the left, buttons 1
     * and 2 side by side on the right. */
    auto face = [port](int act) {
        return QString::fromUtf8(sms_target_short_name(SMS_TARGET_PORT(port, act)));
    };
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    grid->setSizeConstraint(QLayout::SetFixedSize);
    grid->addWidget(control(face(SMS_ACT_UP), SMS_TARGET_PORT(port, SMS_ACT_UP)), 0, 1);
    grid->addWidget(control(face(SMS_ACT_LEFT), SMS_TARGET_PORT(port, SMS_ACT_LEFT)), 1, 0);
    grid->addWidget(control(face(SMS_ACT_RIGHT), SMS_TARGET_PORT(port, SMS_ACT_RIGHT)), 1, 2);
    grid->addWidget(control(face(SMS_ACT_DOWN), SMS_TARGET_PORT(port, SMS_ACT_DOWN)), 2, 1);
    grid->setColumnMinimumWidth(3, 18);
    grid->addWidget(control(face(SMS_ACT_1), SMS_TARGET_PORT(port, SMS_ACT_1)), 1, 4);
    grid->addWidget(control(face(SMS_ACT_2), SMS_TARGET_PORT(port, SMS_ACT_2)), 1, 5);
    auto *gridRow = new QHBoxLayout;
    gridRow->addStretch();
    gridRow->addLayout(grid);
    gridRow->addStretch();
    v->addLayout(gridRow);
    return box;
}

void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) m_padForm->removeRow(w);
    m_padRows.clear();
    const int n = smssession_gamepad_count(m_session);
    if (n == 0) {
        auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
        l->setStyleSheet(QStringLiteral("color: gray;"));
        m_padForm->addRow(l);
        m_padRows.push_back(l);
        return;
    }
    for (int i = 0; i < n && i < 8; ++i) {
        char name[128];
        smssession_gamepad_name(m_session, i, name, sizeof name);
        auto *box = new QComboBox;
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
        box->setCurrentIndex(smssession_gamepad_assignment(m_session, i) + 1);
        const int eff = smssession_gamepad_effective_port(m_session, i);
        box->setToolTip(eff >= 0 ? QStringLiteral("Driving player %1").arg(eff + 1)
                                 : QStringLiteral("Driving no player"));
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            smssession_gamepad_assign(m_session, i, idx - 1);
        });
        m_padForm->addRow(QString::fromUtf8(name), box);
        m_padRows.push_back(box);
    }
}

void ControllersWindow::onPressed(int target)
{
    if (m_mapState == -1) { setMapState(target); return; }
    if (m_mapState >= 0) return;
    m_mouseHeld = target;
    if (target >= SMS_TARGET_SYSACT(0)) return;   /* fires on release */
    smssession_press(m_session, target, 1);
}

/* A system action is posted rather than run here, so the main window takes
 * it as it does a gamepad's: with its status message, and with the
 * debugger window for Debugger Stop. */
void ControllersWindow::onReleased(int target)
{
    if (m_mapState != -2) return;
    if (m_mouseHeld == target) m_mouseHeld = -1;
    if (target >= SMS_TARGET_SYSACT(0)) {
        smssession_sysaction_post(m_session, target - SMS_TARGET_SYSACT(0));
        return;
    }
    smssession_press(m_session, target, 0);
}

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_captureTimer.stop();
        smssession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QString());
    } else if (state == -1) {
        m_captureTimer.stop();
        smssession_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Cancel"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: white;").arg(smsAccentColor().name()));
        m_hint->setText(QStringLiteral("Click a control to remap"));
    } else {
        m_hint->setText(QStringLiteral("Press a key or gamepad button for %1")
                            .arg(QString::fromUtf8(sms_target_name(state))));
        smssession_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    refreshLabels();
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (smssession_gamepad_capture_poll(m_session, &button)) {
        char stolen[128];
        smssession_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
        const QString msg = stolen[0]
            ? QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(sms_pad_button_name(button)), QString::fromUtf8(stolen))
            : QString();
        setMapState(-1);
        if (!msg.isEmpty()) m_hint->setText(msg);
    }
}

void ControllersWindow::refreshLabels()
{
    for (PadButton *b : m_controls) {
        if (m_mapState != -2) {
            const sms_binding bind = smssession_binding_get(m_session, b->target());
            char key[32];
            smssession_keysym_name(bind.keysym, key, sizeof key);
            QString text = key[0] ? QString::fromUtf8(key) : QStringLiteral("—");
            if (bind.button != SMS_PAD_BTN_NONE)
                text += QStringLiteral(" / ") + QString::fromUtf8(sms_pad_button_name(bind.button));
            b->setText(text);
            b->setToolTip(QString::fromUtf8(sms_target_name(b->target())));
            b->setHeld(b->target() == m_mapState);
        } else {
            b->setText(b->face());
            b->setToolTip(QString());
            b->setHeld(false);
        }
    }
}

/* What the machine sees held, from every source, lit in the accent colour;
 * the gamepad list follows hot-plugging; the Reset button follows the
 * console chosen in Preferences (the Master System II and the Mark III have
 * none). */
void ControllersWindow::poll()
{
    const unsigned gen = smssession_gamepad_generation(m_session);
    if (gen != m_padGen) { m_padGen = gen; rebuildPads(); }
    const int console = smssession_console(m_session);
    m_reset->setVisible(sms_console_has_reset_button(console) != 0);
    if (m_mapState != -2) return;
    const unsigned held[2] = { smssession_buttons_held(m_session, 0), smssession_buttons_held(m_session, 1) };
    for (PadButton *b : m_controls) {
        const int t = b->target();
        bool on = t == m_mouseHeld;
        if (t < 2 * SMS_ACT_PER_PORT)
            on = on || (held[t / SMS_ACT_PER_PORT] & (1u << (t % SMS_ACT_PER_PORT)));
        else if (t < SMS_TARGET_SYSACT(0))
            on = on || smssession_switch_held(m_session, t - SMS_TARGET_SWITCH(0));
        b->setHeld(on);
    }
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    poll();
    m_pollTimer.start(50);
    QWidget::showEvent(e);
}

void ControllersWindow::hideEvent(QHideEvent *e)
{
    m_pollTimer.stop();
    setMapState(-2);
    QWidget::hideEvent(e);
}

void ControllersWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = smsKeysymFromQt(e);
    if (m_mapState >= 0) {
        if (!ks) return;
        char stolen[128], name[32];
        smssession_binding_set_key(m_session, m_mapState, ks, stolen, sizeof stolen);
        smssession_keysym_name(ks, name, sizeof name);
        setMapState(-1);
        if (stolen[0])
            m_hint->setText(QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(name), QString::fromUtf8(stolen)));
        return;
    }
    if (m_mapState == -1) return;
    if (e->key() == Qt::Key_F9) { hide(); return; }
    if (!ks) { QWidget::keyPressEvent(e); return; }
    const int sa = smssession_key_sysaction(m_session, ks);
    if (sa >= 0) { smssession_sysaction_post(m_session, sa); return; }
    if (!smssession_key(m_session, ks, 1)) QWidget::keyPressEvent(e);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat() || m_mapState != -2) return;
    const uint32_t ks = smsKeysymFromQt(e);
    if (!ks || !smssession_key(m_session, ks, 0)) QWidget::keyReleaseEvent(e);
}
