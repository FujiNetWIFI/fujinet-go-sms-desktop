/*
 * The main window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "smssession.h"

class DisplayWidget;
class ControllersWindow;
class QAction;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(smssession *session, QWidget *parent = nullptr);
    /* A dropped (or command-line) file: a BIOS is imported, a cartridge is
     * opened, anything else is copied to FujiNet's SD folder. */
    void loadMedia(const QString &path);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool event(QEvent *e) override;

private:
    void buildMenus();
    void syncMachineMenu();
    void updateStatus();
    void drainSysactions();
    void runSysaction(int sa);
    void tapSwitch(int sw);
    void openCart(const QString &path);
    void toggleControllers();
    void pollGamepads();
    void applyPicture();
    void showSettings();
    void restartSession();
    void showAbout();

    smssession *m_session;
    DisplayWidget *m_display = nullptr;
    ControllersWindow *m_controllers = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_dot = nullptr;
    QTimer m_statusTimer;
    QTimer m_sysactTimer;
    QTimer m_padTimer;
    QAction *m_tvAction = nullptr;
    QAction *m_smoothAction = nullptr;
    /* the Machine menu's console controls, labelled with their bound keys */
    QAction *m_pauseAction = nullptr;
    QAction *m_resetButtonAction = nullptr;
    QAction *m_softResetAction = nullptr;
    QAction *m_resetConfigAction = nullptr;
    unsigned m_padGen = 0;
    bool m_sysactDown[SMS_SYSACT_COUNT] = {};
};
