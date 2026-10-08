/* fm_ym2413.cpp -- the YM2413 (OPLL) of the Japanese Master System and the
 * Mark III FM Sound Unit: an extern "C" shim over ymfm.
 *
 * MAME's ym2413_device is ymfm::ym2413 driven through ymfm_device_base
 * (src/devices/sound/ymopl.cpp, ymfm_mame.h); ymfm (Aaron Giles, BSD-3-
 * Clause) is vendored verbatim in ymfm/ from MAME's 3rdparty tree, so the
 * synthesis is MAME's own. The one difference from MAME is the instrument
 * ROM: MAME loads ym2413_instruments.bin, a dump of the chip's internal
 * patch set, which is not redistributable; it is imported at run time like
 * the BIOS, and without it ymfm's built-in table (close, not identical) is
 * used.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>
#include <new>

#include "ymfm/ymfm_opl.h"

#include "fm.h"

namespace {

/* The OPLL has no timers, IRQs or external memory; ymfm's default
 * interface is enough. */
class fm_interface : public ymfm::ymfm_interface
{
};

}

struct sms_fm {
    fm_interface intf;
    ymfm::ym2413 chip;
    uint8_t instruments[0x90];
    bool have_instruments;
    uint8_t regs[0x40];
    uint8_t address;

    explicit sms_fm(const uint8_t *inst)
        : chip(intf, nullptr), have_instruments(inst != nullptr), address(0)
    {
        std::memset(regs, 0, sizeof regs);
        if (inst)
        {
            std::memcpy(instruments, inst, sizeof instruments);
            chip.set_instrument_data(instruments);
        }
    }
};

extern "C" sms_fm_t *sms_fm_create(const uint8_t *instruments)
{
    sms_fm_t *f = new (std::nothrow) sms_fm(instruments);
    if (f)
        f->chip.reset();
    return f;
}

extern "C" void sms_fm_free(sms_fm_t *f)
{
    delete f;
}

extern "C" void sms_fm_reset(sms_fm_t *f)
{
    if (!f)
        return;
    f->chip.reset();
    std::memset(f->regs, 0, sizeof f->regs);
    f->address = 0;
}

extern "C" void sms_fm_write(sms_fm_t *f, unsigned offset, uint8_t data)
{
    if (!f)
        return;
    if ((offset & 1) == 0)
        f->address = data;
    else
        f->regs[f->address & 0x3f] = data;
    f->chip.write(offset & 1, data);
}

extern "C" int32_t sms_fm_generate(sms_fm_t *f)
{
    if (!f)
        return 0;
    ymfm::ym2413::output_data out;
    f->chip.generate(&out, 1);
    /* MAME routes ALL_OUTPUTS (melody and rhythm) to the mono speaker at
     * gain 1.0 */
    int32_t sum = 0;
    for (uint32_t i = 0; i < ymfm::ym2413::OUTPUTS; i++)
        sum += out.data[i];
    return sum;
}

extern "C" void sms_fm_regs(const sms_fm_t *f, uint8_t regs[0x40])
{
    if (!f)
    {
        std::memset(regs, 0, 0x40);
        return;
    }
    std::memcpy(regs, f->regs, 0x40);
}
