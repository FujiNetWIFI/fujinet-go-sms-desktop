/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

namespace {

const char *const kBiosFilter =
    "BIOS and YM2413 patch ROMs (*.sms *.bin *.rom *.ic2 *.SMS *.BIN *.ROM *.IC2);;"
    "All files (*)";

QCheckBox *check(const char *text, const char *tip, smssession *s, const char *key, int def)
{
    auto *b = new QCheckBox(QString::fromUtf8(text));
    b->setToolTip(QString::fromUtf8(tip));
    b->setChecked(smssession_get_int(s, key, def) != 0);
    return b;
}

bool commit(smssession *s, const char *key, int def, int value)
{
    if (smssession_get_int(s, key, def) == value) return false;
    smssession_set_int(s, key, value);
    return true;
}

} // namespace

int SettingsDialog::importBios(QWidget *parent, smssession *session, const QString &path)
{
    QString f = path;
    if (f.isEmpty())
        f = QFileDialog::getOpenFileName(parent, QStringLiteral("Import BIOS"), QString(),
                                         QString::fromUtf8(kBiosFilter));
    if (f.isEmpty()) return -1;
    char msg[512];
    const int idx = smssession_import_bios(session, f.toLocal8Bit().constData(), msg, sizeof msg);
    const QString text = QString::fromUtf8(msg[0] ? msg : smssession_last_error(session));
    /* an unknown CRC goes in as the custom BIOS, with a warning */
    if (idx < 0)
        QMessageBox::warning(parent, QStringLiteral("Import failed"), text);
    else if (idx == smssession_bios_count())
        QMessageBox::warning(parent, QStringLiteral("Import BIOS"), text);
    else
        QMessageBox::information(parent, QStringLiteral("Import BIOS"), text);
    return idx;
}

bool SettingsDialog::run(QWidget *parent, smssession *session)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.setMinimumWidth(640);
    auto *outer = new QVBoxLayout(&dlg);

    /* Machine (power cycle) */
    auto *machine = new QGroupBox(QStringLiteral("Machine (applied by power-cycling the console)"));
    auto *mform = new QFormLayout(machine);
    auto *console = new QComboBox;
    for (int i = 0; sms_console_name(i); ++i) console->addItem(QString::fromUtf8(sms_console_name(i)));
    console->setCurrentIndex(smssession_get_int(session, "console", SMS_CONSOLE_SMS1));
    mform->addRow(QStringLiteral("Console"), console);

    /* The BIOS is chosen per console; the choices are held here until OK,
     * so moving between consoles in the combo loses none of them. */
    QString pending[SMS_CONSOLE_COUNT];
    bool touched[SMS_CONSOLE_COUNT] = {};
    for (int c = 0; c < SMS_CONSOLE_COUNT; ++c)
        pending[c] = QString::fromUtf8(smssession_console_bios(session, c));
    auto *bios = new QComboBox;
    bios->setToolTip(QStringLiteral("Never required: with None the console boots the cartridge "
                                    "directly. Import BIOS takes your own image."));
    auto *importBtn = new QPushButton(QStringLiteral("Import BIOS..."));
    auto *biosRow = new QHBoxLayout;
    biosRow->addWidget(bios, 1);
    biosRow->addWidget(importBtn);
    mform->addRow(QStringLiteral("BIOS"), biosRow);

    auto *fmNote = new QLabel;
    mform->addRow(QStringLiteral("FM sound"), fmNote);
    QCheckBox *fmUnit = check("FM Sound Unit fitted", "The Mark III's optional YM2413 add-on", session, "fm_unit", 1);
    QCheckBox *fmMutes = check("FM mutes the PSG", "With the FM sound on, the unit silences the PSG "
                               "(as the hardware is assumed to; MAME's unit does not)",
                               session, "fm_unit_mutes_psg", 0);
    mform->addRow(fmUnit);
    mform->addRow(fmMutes);
    auto *patch = new QLabel;
    patch->setStyleSheet(QStringLiteral("color: gray;"));
    mform->addRow(QStringLiteral("YM2413 patches"), patch);
    outer->addWidget(machine);

    /* the BIOS list for console c: None, then every imported image that
     * fits it (the custom one included), and a choice whose image has gone
     * stays listed so it is not silently dropped */
    auto fillBios = [&, session](int c) {
        QSignalBlocker block(bios);
        bios->clear();
        if (!sms_console_has_bios_socket(c)) {
            bios->addItem(QStringLiteral("None (no BIOS socket)"), QString());
            bios->setEnabled(false);
            return;
        }
        bios->setEnabled(true);
        bios->addItem(QStringLiteral("None (boot the cartridge)"), QString());
        for (int i = 0; i <= smssession_bios_count(); ++i) {
            const sms_bios_info *b = smssession_bios_info(i);
            if (!b || !((b->consoles >> c) & 1u) || !smssession_bios_available(session, i)) continue;
            bios->addItem(QString::fromUtf8(b->desc), QString::fromUtf8(b->name));
        }
        int at = bios->findData(pending[c]);
        if (at < 0 && !pending[c].isEmpty()) {
            bios->addItem(QStringLiteral("%1 (not imported)").arg(pending[c]), pending[c]);
            at = bios->count() - 1;
        }
        bios->setCurrentIndex(at < 0 ? 0 : at);
    };
    auto syncMachine = [&, session]() {
        const int c = console->currentIndex();
        fillBios(c);
        const bool mark3 = c == SMS_CONSOLE_MARK3;
        fmUnit->setEnabled(mark3);
        fmMutes->setEnabled(mark3 && fmUnit->isChecked());
        if (c == SMS_CONSOLE_SMSJ)
            fmNote->setText(QStringLiteral("YM2413 built in"));
        else if (mark3)
            fmNote->setText(QStringLiteral("The FM Sound Unit is an option on the Mark III"));
        else
            fmNote->setText(QStringLiteral("None on this console"));
        const bool have = smssession_bios_available(session, smssession_bios_count() - 1) != 0;
        patch->setText(have ? QStringLiteral("Instrument ROM imported: the FM sound uses it")
                            : QStringLiteral("Instrument ROM not imported: ymfm's own table stands in"));
        patch->setToolTip(QStringLiteral("The YM2413's built-in instrument patches, as a ROM image of "
                                         "your own; Import BIOS takes it too"));
    };
    QObject::connect(console, &QComboBox::currentIndexChanged, &dlg, [&](int) { syncMachine(); });
    QObject::connect(bios, &QComboBox::currentIndexChanged, &dlg, [&](int) {
        const int c = console->currentIndex();
        pending[c] = bios->currentData().toString();
        touched[c] = true;
    });
    QObject::connect(fmUnit, &QCheckBox::toggled, &dlg, [&](bool) { syncMachine(); });
    /* Importing is a file operation, done now; an image imported for the
     * running console becomes its BIOS there and then, so the choices not
     * yet touched here follow it. */
    QObject::connect(importBtn, &QPushButton::clicked, &dlg, [&, session]() {
        if (importBios(&dlg, session) < 0) return;
        for (int c = 0; c < SMS_CONSOLE_COUNT; ++c)
            if (!touched[c]) pending[c] = QString::fromUtf8(smssession_console_bios(session, c));
        syncMachine();
    });
    syncMachine();

    /* Picture (live) */
    auto *picture = new QGroupBox(QStringLiteral("Picture"));
    auto *picForm = new QFormLayout(picture);
    /* "aspect" is 0 for the TV's pixel aspect (the default), 1 for square
     * pixels: the box shows the first */
    auto *tv = new QCheckBox(QStringLiteral("TV aspect"));
    tv->setToolTip(QStringLiteral("The television's pixel aspect: 8:7 on NTSC, about 1.39 on PAL; "
                                  "off shows square pixels"));
    tv->setChecked(smssession_get_int(session, "aspect", 0) == 0);
    QCheckBox *smooth = check("Smooth scaling", "Filter the picture when scaling it; off keeps the pixels sharp", session, "smooth", 0);
    picForm->addRow(tv);
    picForm->addRow(smooth);
    QObject::connect(tv, &QCheckBox::toggled, &dlg,
                     [=](bool on) { smssession_set_int(session, "aspect", on ? 0 : 1); });
    QObject::connect(smooth, &QCheckBox::toggled, &dlg,
                     [=](bool on) { smssession_set_int(session, "smooth", on ? 1 : 0); });
    outer->addWidget(picture);

    /* Audio: the device needs a restart, the volume is live */
    auto *audio = new QGroupBox(QStringLiteral("Audio"));
    auto *audioForm = new QFormLayout(audio);
    QCheckBox *snd = check("Audio output (applied by restarting the session)", "Open the system audio device", session, "enable_audio", 1);
    audioForm->addRow(snd);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(smssession_get_int(session, "volume", 100));
    QObject::connect(volume, &QSlider::valueChanged, &dlg, [=](int v) { smssession_set_volume(session, v); });
    audioForm->addRow(QStringLiteral("Volume"), volume);
    outer->addWidget(audio);

    /* Input: the gamepad thread needs a restart, the rest is live */
    auto *input = new QGroupBox(QStringLiteral("Input"));
    auto *inForm = new QFormLayout(input);
    QCheckBox *gp = check("Gamepads (applied by restarting the session)", "Poll USB/Bluetooth gamepads", session, "enable_gamepad", 1);
    inForm->addRow(gp);
    QCheckBox *aj = check("Analog sticks drive the D-pad", "The left stick as the joypad's direction pad, as well as the D-pad", session, "analog_joystick", 1);
    QObject::connect(aj, &QCheckBox::toggled, &dlg, [=](bool on) { smssession_set_analog(session, on ? 1 : 0); });
    inForm->addRow(aj);
    outer->addWidget(input);

    /* Gamepads: one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to players in connection order unless chosen here)"));
    auto *padForm = new QFormLayout(pads);
    std::vector<QWidget *> padRows;
    unsigned seenGen = smssession_gamepad_generation(session) + 1;
    auto rebuildPads = [&, padForm, session]() mutable {
        for (QWidget *w : padRows) { padForm->removeRow(w); }
        padRows.clear();
        const int n = smssession_gamepad_count(session);
        if (n == 0) {
            auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
            padForm->addRow(l);
            padRows.push_back(l);
            return;
        }
        for (int i = 0; i < n && i < 8; ++i) {
            char name[128];
            smssession_gamepad_name(session, i, name, sizeof name);
            auto *box = new QComboBox;
            box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Player 1"), QStringLiteral("Player 2") });
            box->setCurrentIndex(smssession_gamepad_assignment(session, i) + 1);
            const int eff = smssession_gamepad_effective_port(session, i);
            box->setToolTip(eff >= 0 ? QStringLiteral("Driving player %1").arg(eff + 1)
                                     : QStringLiteral("Driving no player"));
            QObject::connect(box, &QComboBox::currentIndexChanged, box, [=](int idx) {
                smssession_gamepad_assign(session, i, idx - 1);
            });
            padForm->addRow(QString::fromUtf8(name), box);
            padRows.push_back(box);
        }
    };
    auto *padTimer = new QTimer(&dlg);
    QObject::connect(padTimer, &QTimer::timeout, &dlg, [&, session]() mutable {
        const unsigned gen = smssession_gamepad_generation(session);
        if (gen != seenGen) { seenGen = gen; rebuildPads(); }
    });
    padTimer->start(1000);
    seenGen = smssession_gamepad_generation(session);
    rebuildPads();
    outer->addWidget(pads);

    /* FujiNet (restart) */
    auto *fujiBox = new QGroupBox(QStringLiteral("FujiNet"));
    auto *fform = new QFormLayout(fujiBox);
    QCheckBox *fuji = check("Enable FujiNet (applied by restarting the session)", "Run the in-process FujiNet the cartridge dials into. Off means no network and a link-down CONFIG client.", session, "enable_fujinet", 1);
    fform->addRow(fuji);
    const QString url = QString::fromUtf8(smssession_fujinet_webui_url(session));
    auto *web = new QLabel(QStringLiteral("<a href=\"%1\">%1</a>").arg(url.toHtmlEscaped()));
    web->setOpenExternalLinks(true);
    web->setTextInteractionFlags(Qt::TextBrowserInteraction);
    fform->addRow(QStringLiteral("Web UI"), web);
    outer->addWidget(fujiBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    bool changed = false;
    const int chosen = console->currentIndex();
    changed |= commit(session, "console", SMS_CONSOLE_SMS1, chosen);
    /* every console's BIOS is kept; only the one that boots needs the power
     * cycle */
    for (int c = 0; c < SMS_CONSOLE_COUNT; ++c) {
        if (!sms_console_has_bios_socket(c)) continue;
        const QByteArray want = pending[c].toUtf8();
        if (want == QByteArray(smssession_console_bios(session, c))) continue;
        smssession_set_console_bios(session, c, want.constData());
        if (c == chosen) changed = true;
    }
    changed |= commit(session, "fm_unit", 1, fmUnit->isChecked() ? 1 : 0);
    changed |= commit(session, "fm_unit_mutes_psg", 0, fmMutes->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_fujinet", 1, fuji->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_audio", 1, snd->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_gamepad", 1, gp->isChecked() ? 1 : 0);
    return changed;
}
