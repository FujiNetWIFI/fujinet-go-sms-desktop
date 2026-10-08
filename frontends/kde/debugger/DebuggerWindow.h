/*
 * Debugger window (Qt6 Widgets) over the Z80 / VDP debugger engine, via
 * core/include/smsdebug.h. Mirrors the GNOME one tab for tab.
 *
 * The engine is attached while the window is shown (which stops the
 * machine, as on every sibling) and detached when it is hidden, which lets
 * the machine run on.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <cstdint>
#include <vector>

extern "C" {
#include "smsdebug.h"
#include "smssession.h"
}

class DebuggerWindow : public QMainWindow {
    Q_OBJECT
public:
    /* Shows the window (attaching, so the machine stops). */
    static void showFor(QWidget *parent, smssession *session);
    /* F12: shows it, or hides it (detaching) when it is already up. */
    static void toggleFor(QWidget *parent, smssession *session);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    explicit DebuggerWindow(smssession *session, QWidget *parent);
    QWidget *buildToolbar();
    QWidget *buildPrompt();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildVdp();
    QWidget *buildSound();
    QWidget *buildBreaks();
    QWidget *buildCart();

    void refreshAll();
    void refreshStatus();
    void refreshCpu();
    void refreshRam();
    void refreshDisasm();
    void refreshVdp();
    void refreshSound();
    void refreshBps();
    void refreshCart();
    void tick();
    void toggleRun();
    void runPrompt();
    void appendPrompt(const QString &text);
    void jumpTo(const QString &text);
    void stepAnd(void (*fn)(smsdebug *));
    void loadSymbols(bool fromFile);
    bool addressOf(const QString &text, long *out);

    smssession *m_session;
    smsdebug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    int m_runningTicks = 0;
    QTimer m_timer;

    QLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    QPlainTextEdit *m_promptOut = nullptr;
    QLineEdit *m_promptIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    QLineEdit *m_reg[17] = {};
    QCheckBox *m_flag[6] = {};
    QLabel *m_beam = nullptr;
    QPlainTextEdit *m_ram = nullptr;
    QLineEdit *m_ramGoto = nullptr;
    QLineEdit *m_ramAddr = nullptr, *m_ramVal = nullptr;

    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_disasmTop = 0;
    std::vector<uint16_t> m_lineAddr;

    QPlainTextEdit *m_vdpText = nullptr;
    QComboBox *m_vdpView = nullptr;
    QComboBox *m_vdpPalette = nullptr;
    QLabel *m_vdpPic = nullptr;
    QPlainTextEdit *m_sprites = nullptr;
    QLineEdit *m_cram[32] = {};
    std::vector<uint32_t> m_vdpPx;

    QPlainTextEdit *m_sound = nullptr;

    QTableWidget *m_bpTable = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpCond = nullptr;
    bool m_bpFilling = false;

    QPlainTextEdit *m_cart = nullptr;
};
