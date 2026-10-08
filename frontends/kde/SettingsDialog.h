/*
 * SettingsDialog -- the Preferences dialog. The analog stick switch, the
 * gamepad assignments, the picture and the volume apply live; the console,
 * its BIOS, the FM options and the host options need a power cycle, which
 * the caller does when run() says one of those changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>
#include <QString>

extern "C" {
#include "smssession.h"
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /* Returns true if a restart option changed. */
    static bool run(QWidget *parent, smssession *session);
    /* Import BIOS: the user's own BIOS or YM2413 patch ROM, from `path` or,
     * when it is empty, a file chooser. Shows the session's message, which
     * always says what happened. Returns the image's index, or -1 (also
     * when the chooser was cancelled). */
    static int importBios(QWidget *parent, smssession *session, const QString &path = QString());
};
