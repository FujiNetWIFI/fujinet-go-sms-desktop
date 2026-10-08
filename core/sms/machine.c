// license:BSD-3-Clause
// copyright-holders:Wilbert Pol, Charles MacDonald,Mathis Rosenhauer,Brad Oliver,Michael Luong,Fabio Priuli,Enik Land
/* machine.c -- the Master System and the Mark III: bus dispatch, the I/O
 * maps, the memory and I/O control ports, BIOS paging, the Japanese FM
 * ports, and the cycle loop.
 *
 * Transposed from MAME src/mame/sega/sms.cpp (the address maps and the
 * machine configurations) and sms_m.cpp (the handlers), and the Mark III FM
 * Sound Unit from src/devices/bus/sg1000_exp/fm_unit.cpp (all BSD-3-Clause;
 * see COMPLIANCE.md). MAME's names are kept.
 *
 *   0000-BFFF  read: BIOS and/or cartridge (ANDed when both are enabled);
 *              0000-03FF is always BIOS page 0
 *              write: the cartridge (its mapper and mailbox)
 *   C000-FFFF  8K RAM, mirrored; a write to FFFC-FFFF also goes to the
 *              mapper and pages the BIOS
 *
 *   I/O (A7, A6, A0 decoded on the export consoles):
 *   00-3F      write: even $3E memory control, odd $3F I/O control
 *   40-7F      read: even V counter, odd H counter; write: the PSG
 *   80-BF      even VDP data, odd VDP control/status
 *   C0-FF      read: even $DC, odd $DD (the joypads, Reset, TH)
 *
 * The Japanese console decodes $3E/$3F, $C0/$C1/$DC/$DD exactly and adds
 * the YM2413 at $F0/$F1 and the audio mute select at $F2; the Mark III has
 * no $3E/$3F at all and its FM Sound Unit sits on the peripheral ports.
 *
 * The cycle loop. One z80_tick() is one T-state. The VDP's timer events run
 * a couple of cycles behind the CPU (VDP_LAG) so that every CPU access can
 * catch the VDP up to the exact cycle MAME would make it at: MAME performs
 * an access at the start of its machine cycle, while z80.h presents it one
 * T-state later for memory and OUT and two later for IN (the SKEW_*
 * constants). The interrupt lines are sampled ahead of the lagging engine
 * (IRQ_AHEAD) without running it, as MAME's CPU sees a timer that fired
 * just before an instruction boundary.
 */

#include <stdlib.h>
#include <string.h>

#define CHIPS_IMPL
#include "z80/z80.h"

#include "sms_internal.h"

#define BIT(x, n) (((x) >> (n)) & 1)

/* The cycle-loop calibration (see the header comment). */
#define VDP_LAG   2
#define SKEW_MEM  1
#define SKEW_IOR  2
#define SKEW_IOW  1
#define IRQ_AHEAD 1

const sms_model_info_t sms_models[SMS_MODEL_COUNT] = {
    [SMS_MODEL_SMS1] = {
        "sms1", "Master System", SMS_VDP_5124, false, 0x20000,
        true, false, false, false, false, false, true, false, 0xff },
    [SMS_MODEL_SMS1_PAL] = {
        "sms1pal", "Master System (PAL)", SMS_VDP_5124, true, 0x20000,
        true, false, false, false, false, false, true, false, 0xff },
    [SMS_MODEL_SMS2] = {
        "sms", "Master System II", SMS_VDP_5246, false, 0x20000,
        true, false, false, false, false, false, false, false, 0xff },
    [SMS_MODEL_SMS2_PAL] = {
        "smspal", "Master System II (PAL)", SMS_VDP_5246, true, 0x40000,
        true, false, false, false, false, false, false, false, 0xff },
    [SMS_MODEL_SMSJ] = {
        "smsj", "Master System (Japan)", SMS_VDP_5124, false, 0x4000,
        false, true, true, false, true, true, false, true, 0xff },
    [SMS_MODEL_MARK3] = {
        "sg1000m3", "Mark III", SMS_VDP_5124, false, 0,
        false, false, false, true, false, true, false, false, 0x00 },
};

/* ---------------------------------------------------------------------- */
/* audio                                                                  */
/* ---------------------------------------------------------------------- */

void sms_mix_init(sms_mix_t *x, double cpu_hz)
{
    memset(x, 0, sizeof *x);
    x->step = (double)SMS_AUDIO_RATE / cpu_hz;
    x->psg_gain = 1.0f;
    x->fm_gain = 1.0f;
}

/* One CPU clock: accumulate, and emit a 48 kHz frame when the window
 * closes. MAME mixes the PSG (VDP route gain 1.0) and the YM2413 (gain 1.0)
 * into the mono speaker; each stream value is put_int(..., 32768). */
void sms_mix_tick(sms_mix_t *x, int16_t psg_sample)
{
    x->sum += (double)psg_sample * x->psg_gain + (double)x->fm_hold * x->fm_gain;
    x->n++;
    x->acc += x->step;
    if (x->acc >= 1.0)
    {
        x->acc -= 1.0;
        float v = (float)(x->sum / (double)x->n / 32768.0);
        if (v > 1.0f)
            v = 1.0f;
        else if (v < -1.0f)
            v = -1.0f;
        x->sum = 0;
        x->n = 0;
        if (x->out_frames < (int)(sizeof x->out / sizeof x->out[0] / 2))
        {
            x->out[2 * x->out_frames] = v;
            x->out[2 * x->out_frames + 1] = v;
            x->out_frames++;
        }
    }
}

/* smsj_set_audio_control. Mute settings:
 *   0,0 : PSG only (power-on default)
 *   0,1 : FM only
 *   1,0 : Both PSG and FM disabled
 *   1,1 : Both PSG and FM enabled */
static void smsj_set_audio_control(sms_machine_t *m, uint8_t data)
{
    m->smsj_audio_control = data & 0x03;
    m->mix.psg_gain = (m->smsj_audio_control == 0x00 || m->smsj_audio_control == 0x03) ? 1.0f : 0.0f;
    m->mix.fm_gain = (m->smsj_audio_control == 0x01 || m->smsj_audio_control == 0x03) ? 1.0f : 0.0f;
}

/* sega_fm_unit_device::set_audio_control. MAME assumes the PSG is muted
 * while FM is active, but its PSG lookup does not resolve on the Mark III
 * (the PSG lives inside the VDP), so in MAME the mute never happens;
 * fm_unit_mutes_psg chooses the hardware's assumed behaviour instead. */
static void fm_unit_set_audio_control(sms_machine_t *m, uint8_t data)
{
    m->fm_audio_control = data & 0x01;
    if (m->fm_audio_control == 0x01)
    {
        m->mix.fm_gain = 1.0f;
        m->mix.psg_gain = m->fm_unit_mutes_psg ? 0.0f : 1.0f;
    }
    else
    {
        m->mix.fm_gain = 0.0f;
        m->mix.psg_gain = 1.0f;
    }
}

/* ---------------------------------------------------------------------- */
/* inputs                                                                 */
/* ---------------------------------------------------------------------- */

/* The joypad's in_r(): bits 0-5 active low, 6-7 high. */
static uint8_t ctrl_in(const sms_machine_t *m, int port)
{
    return (uint8_t)(0xc0 | (~m->pad[port] & 0x3f));
}

static void sms_get_inputs(sms_machine_t *m)
{
    m->port_dc_reg = 0xff;
    m->port_dd_reg = 0xff;

    m->port_dc_reg &= (uint8_t)(~0x3f | ctrl_in(m, 0));          /* Up, Down, Left, Right, TL, TR */
    const uint8_t data2 = ctrl_in(m, 1);
    m->port_dc_reg &= (uint8_t)(~0xc0 | (data2 << 6));            /* Up, Down */
    m->port_dd_reg &= (uint8_t)(~0x0f | (data2 >> 2));            /* Left, Right, TL, TR */
    m->port_dd_reg &= (uint8_t)(~0x40 | (m->ctrl1_th_state << 6)); /* TH ctrl1 */
    m->port_dd_reg &= (uint8_t)(~0x80 | (m->ctrl2_th_state << 7)); /* TH ctrl2 */
}

static uint8_t sms_input_port_dc_r(sms_machine_t *m, bool commit)
{
    (void)commit;
    /* Return if the I/O chip is disabled (1) */
    if (m->mem_ctrl_reg & SMS_IO_CHIP)
        return 0xff;

    sms_get_inputs(m);

    /* Check if TR of controller port 1 is set to output (0) */
    if (!(m->io_ctrl_reg & 0x01))
    {
        /* Read TR state set through IO control port */
        m->port_dc_reg &= (uint8_t)(~0x20 | ((m->io_ctrl_reg & 0x10) << 1));
    }

    if (m->model->has_rapid_button)
    {
        /* Check if Rapid Fire is enabled for Button 1 / Button 2 */
        if (m->rapid_mode & 0x01)
            m->port_dc_reg |= m->rapid_read_state & 0x10;
        if (m->rapid_mode & 0x02)
            m->port_dc_reg |= m->rapid_read_state & 0x20;
    }

    return m->port_dc_reg;
}

static uint8_t sms_input_port_dd_r(sms_machine_t *m, bool commit)
{
    /* Return if the I/O chip is disabled (1) */
    if (m->mem_ctrl_reg & SMS_IO_CHIP)
        return 0xff;

    sms_get_inputs(m);

    /* Check if TR of controller port 2 is set to output (0) */
    if (!(m->io_ctrl_reg & 0x04))
    {
        /* Read TR state set through IO control port */
        m->port_dd_reg &= (uint8_t)(~0x08 | ((m->io_ctrl_reg & 0x40) >> 3));
    }

    /* Reset Button (active low) */
    if (m->model->has_reset_button)
        m->port_dd_reg &= (uint8_t)(~0x10 | ((m->reset_btn ? 0 : 1) << 4));

    /* Check if TH of controller port 1 is set to output (0) */
    if (!(m->io_ctrl_reg & 0x02))
    {
        m->port_dd_reg &= (uint8_t)~0x40;
        if (!m->model->ioctrl_region_is_japan)
        {
            /* Read TH state set through IO control port */
            m->port_dd_reg |= (uint8_t)((m->io_ctrl_reg & 0x20) << 1);
        }
    }
    else   /* TH set to input (1) */
    {
        if (m->ctrl1_th_latch)
        {
            if (m->vdp.hcounter_latched)
                m->port_dd_reg &= (uint8_t)~0x40;
            if (commit)
                m->ctrl1_th_latch = 0;
        }
    }

    /* Check if TH of controller port 2 is set to output (0) */
    if (!(m->io_ctrl_reg & 0x08))
    {
        m->port_dd_reg &= (uint8_t)~0x80;
        if (!m->model->ioctrl_region_is_japan)
        {
            /* Read TH state set through IO control port */
            m->port_dd_reg |= (uint8_t)(m->io_ctrl_reg & 0x80);
        }
    }
    else   /* TH set to input (1) */
    {
        if (m->ctrl2_th_latch)
        {
            if (m->vdp.hcounter_latched)
                m->port_dd_reg &= (uint8_t)~0x80;
            if (commit)
                m->ctrl2_th_latch = 0;
        }
    }

    if (m->model->has_rapid_button)
    {
        /* Check if Rapid Fire is enabled for Button 1 / Button 2 */
        if (m->rapid_mode & 0x04)
            m->port_dd_reg |= m->rapid_read_state & 0x04;
        if (m->rapid_mode & 0x08)
            m->port_dd_reg |= m->rapid_read_state & 0x08;
    }

    return m->port_dd_reg;
}

/* rapid_n_csync_callback: the smsj's 12-bit C-Sync counter and its Rapid
 * button, which toggles rapid fire for whichever joypad buttons are held
 * while it is pressed. */
static void rapid_n_csync_callback(void *user)
{
    sms_machine_t *m = user;

    if (!m->model->has_rapid_button)
        return;

    const uint8_t rapid_previous_mode = m->rapid_mode;

    m->csync_counter++;
    /* counter is 12 bits wide (for 4096 pulses) */
    m->csync_counter &= 0xfff;

    if (m->reset_btn)   /* Rapid button is pressed */
    {
        sms_get_inputs(m);
        if (m->port_dc_reg != m->rapid_last_dc)
        {
            /* Enable/disable rapid fire for any joypad button pressed. */
            m->rapid_mode ^= (uint8_t)((~m->port_dc_reg & 0x30) >> 4);
            m->rapid_last_dc = m->port_dc_reg;
        }
        if (m->port_dd_reg != m->rapid_last_dd)
        {
            m->rapid_mode ^= (uint8_t)(~m->port_dd_reg & 0x0c);
            m->rapid_last_dd = m->port_dd_reg;
        }
    }
    else
    {
        m->rapid_last_dc = 0xff;
        m->rapid_last_dd = 0xff;
    }

    if ((m->rapid_mode & 0x0f) != 0)
    {
        /* Read state is probably changed at each 256 C-Sync pulses */
        if ((m->csync_counter & 0xff) == 0)
            m->rapid_read_state ^= 0xff;
    }
    else if ((rapid_previous_mode & 0x0f) != 0)
    {
        m->rapid_read_state = 0x00;
    }
}

static uint8_t smsj_audio_control_r(const sms_machine_t *m)
{
    /* Charles MacDonald's internal 12-bit counter of the C-Sync pulses:
     * D7 bit 11, D6 bit 7, D5 bit 3, D1-D0 the mute control */
    uint8_t data = 0x00;
    data |= (m->smsj_audio_control & 0x03);
    data |= (uint8_t)((m->csync_counter & 0x008) << 2);
    data |= (uint8_t)((m->csync_counter & 0x080) >> 1);
    data |= (uint8_t)((m->csync_counter & 0x800) >> 4);
    return data;
}

void sms_machine_latch_inputs(sms_machine_t *m)
{
    m->pad[0] = m->pad_live[0];
    m->pad[1] = m->pad_live[1];
    m->reset_btn = m->reset_live;
    /* the PAUSE port's write-line to the VDP's /NMI-IN (active low) */
    m->vdp.n_nmi_in_state = m->pause_live ? 0 : 1;
}

/* ---------------------------------------------------------------------- */
/* memory                                                                 */
/* ---------------------------------------------------------------------- */

static void setup_enabled_slots(sms_machine_t *m)
{
    m->mem_device_enabled = SMS_ENABLE_NONE;

    if (m->model->is_mark_iii)
    {
        /* Mark III uses the card slot by default, but its /CART pin gives
         * the cartridge priority when one is inserted -- and the FujiNet
         * cartridge always is. */
        m->mem_device_enabled |= SMS_ENABLE_CART;
        return;
    }

    if (!(m->mem_ctrl_reg & SMS_IO_CARTRIDGE))
        m->mem_device_enabled |= SMS_ENABLE_CART;

    if (!(m->mem_ctrl_reg & SMS_IO_BIOS_ROM) && m->BIOS)
        m->mem_device_enabled |= SMS_ENABLE_BIOS;
}

static void setup_media_slots(sms_machine_t *m)
{
    setup_enabled_slots(m);

    if (m->mem_device_enabled == SMS_ENABLE_NONE)
        return;

    /* Setup cartridge RAM: work RAM disabled (1) reads the cart's RAM
     * space, which the FujiNet cartridge does not decode */
    if (!m->model->is_mark_iii && (m->mem_ctrl_reg & SMS_IO_WORK_RAM))
        m->mem_device_enabled |= SMS_ENABLE_EXT_RAM;
}

/* MAME's setup_bios; m->BIOS is set at power-on from the imported image. */
static bool s_bios_paged(const sms_machine_t *m)
{
    return m->BIOS && m->model->has_bios_full;
}

static void setup_bios(sms_machine_t *m)
{
    if (m->BIOS)
    {
        m->bios_page[3] = 0;
        m->bios_page[0] = 0;
        m->bios_page[1] = (1 < m->bios_page_count) ? 1 : 0;
        m->bios_page[2] = (2 < m->bios_page_count) ? 2 : 0;
    }

    if (!m->model->is_mark_iii)
    {
        m->mem_ctrl_reg = (SMS_IO_EXPANSION | SMS_IO_CARTRIDGE | SMS_IO_CARD);
        if (!m->BIOS)
        {
            m->mem_ctrl_reg &= (uint8_t)~SMS_IO_CARTRIDGE;
            m->mem_ctrl_reg |= SMS_IO_BIOS_ROM;
        }
    }
}

static uint8_t read_bus(sms_machine_t *m, unsigned page, uint16_t base_addr, uint16_t offset,
                        bool m1, bool commit)
{
    if (m->mem_device_enabled != SMS_ENABLE_NONE)
    {
        uint8_t data = 0xff;

        /* "If the BIOS is enabled at the same time the cartridge slot is,
         * the data from both sources are logically ANDed together when
         * read." (Charles MacDonald) */
        if (m->mem_device_enabled & SMS_ENABLE_BIOS)
        {
            /* MAME indexes with an unmasked page in one corner case
             * (sms_mapper_w case 0); mask it so it stays in the region */
            const unsigned bank = m->bios_page[page] & (m->bios_page_count - 1u);
            data &= m->BIOS[(bank * 0x4000) + (offset & 0x3fff)];
        }
        if (m->mem_device_enabled & SMS_ENABLE_CART)
            data &= sms_cart_read(m->cart, (uint16_t)(base_addr + offset), m1, commit);
        return data;
    }
    return m->model->unmapped_fill;
}

static uint8_t read_ram(sms_machine_t *m, uint16_t offset)
{
    if (m->mem_device_enabled & SMS_ENABLE_EXT_RAM)
        return 0xff;   /* the cart's read_ram: not decoded */
    return m->mainram[offset & 0x1fff];
}

static void write_ram(sms_machine_t *m, uint16_t offset, uint8_t data)
{
    if (m->mem_device_enabled & SMS_ENABLE_EXT_RAM)
        return;        /* the cart's write_ram: not decoded */
    m->mainram[offset & 0x1fff] = data;
}

static void sms_mapper_w(sms_machine_t *m, unsigned offset, uint8_t data)
{
    m->mapper[offset] = data;
    write_ram(m, (uint16_t)(0x3ffc + offset), data);

    switch (offset)
    {
    case 0: /* Control RAM/ROM */
        if (!(data & 0x08) && (m->mem_device_enabled & SMS_ENABLE_BIOS))
        {
            if (s_bios_paged(m))
                m->bios_page[2] = m->mapper[3];
        }
        if (m->mem_device_enabled & SMS_ENABLE_CART)
            sms_cart_write_mapper(m->cart, (uint16_t)(0xfffc + offset), data);
        break;

    case 1: /* Select 16k ROM bank for 0400-3fff */
    case 2: /* Select 16k ROM bank for 4000-7fff */
    case 3: /* Select 16k ROM bank for 8000-bfff */
        if (m->mem_device_enabled & SMS_ENABLE_BIOS)
        {
            if (s_bios_paged(m))
                m->bios_page[offset - 1] = (uint8_t)(data & (m->bios_page_count - 1));
        }
        if (m->mem_device_enabled & SMS_ENABLE_CART)
            sms_cart_write_mapper(m->cart, (uint16_t)(0xfffc + offset), data);
        break;
    }
}

static uint8_t mem_read(sms_machine_t *m, uint16_t addr, bool m1, bool commit)
{
    if (addr < 0x0400)
        return read_bus(m, 3, 0x0000, addr, m1, commit);
    if (addr < 0x4000)
        return read_bus(m, 0, 0x0000, addr, m1, commit);
    if (addr < 0x8000)
        return read_bus(m, 1, 0x4000, (uint16_t)(addr - 0x4000), m1, commit);
    if (addr < 0xc000)
        return read_bus(m, 2, 0x8000, (uint16_t)(addr - 0x8000), m1, commit);
    /* RAM, the SegaScope mirror (sms1) and sms_mapper_r all read RAM */
    return read_ram(m, (uint16_t)(addr - 0xc000));
}

static void mem_write(sms_machine_t *m, uint16_t addr, uint8_t data)
{
    if (addr < 0xc000)
    {
        if (m->mem_device_enabled & SMS_ENABLE_CART)
            sms_cart_write(m->cart, addr, data);
        return;
    }

    /* the cartridge edge sees every write; its BIOS snoop keeps $C000 */
    if (addr == 0xc000)
        sms_cart_snoop_c000(m->cart, data);

    if (addr < 0xfffc)
        write_ram(m, (uint16_t)(addr - 0xc000), data);
    else
        sms_mapper_w(m, (unsigned)(addr - 0xfffc), data);
}

/* ---------------------------------------------------------------------- */
/* I/O                                                                    */
/* ---------------------------------------------------------------------- */

static void sms_mem_control_w(sms_machine_t *m, uint8_t data)
{
    m->mem_ctrl_reg = data;
    setup_media_slots(m);
}

static void sms_io_control_w(sms_machine_t *m, uint8_t data, uint64_t stamp)
{
    bool latch_hcount = false;

    /* (the joypads ignore TR/TH being driven as outputs) */

    /* check if TH input level is high (1) and was output/low (0) */
    if ((data & 0x02) && !(m->io_ctrl_reg & 0x22) && m->ctrl1_th_state)
        latch_hcount = true;

    /* check if TH input level is high (1) and was output/low (0) */
    if ((data & 0x08) && !(m->io_ctrl_reg & 0x88) && m->ctrl2_th_state)
        latch_hcount = true;

    if (latch_hcount)
        sms_vdp_hcount_latch(&m->vdp, stamp);

    m->io_ctrl_reg = data;
}

static uint8_t vdp_read(sms_machine_t *m, uint8_t port, uint64_t stamp, bool commit)
{
    if (commit)
        sms_vdp_run_until(&m->vdp, stamp);
    if ((port & 0xc0) == 0x40)
        return (port & 1) ? (commit ? sms_vdp_hcount_read(&m->vdp) : m->vdp.hcounter)
                          : sms_vdp_vcount_read(&m->vdp, stamp);
    return (port & 1) ? sms_vdp_control_read(&m->vdp, stamp, commit)
                      : sms_vdp_data_read(&m->vdp, commit);
}

static void vdp_write(sms_machine_t *m, uint8_t port, uint8_t data, uint64_t stamp)
{
    sms_vdp_run_until(&m->vdp, stamp);
    if (port & 1)
        sms_vdp_control_write(&m->vdp, data, stamp);
    else
        sms_vdp_data_write(&m->vdp, data);
}

static uint8_t sg1000m3_peripheral_r(sms_machine_t *m, unsigned offset)
{
    /* the FM unit answers offsets 0-3 with its control bit, over the
     * joypads */
    if (m->fm_unit && offset <= 3)
        return m->fm_audio_control & 0x01;
    sms_get_inputs(m);
    return (offset & 0x01) ? m->port_dd_reg : m->port_dc_reg;
}

static void sg1000m3_peripheral_w(sms_machine_t *m, unsigned offset, uint8_t data)
{
    if (!m->fm_unit || offset > 3)
        return;
    switch (offset)
    {
    case 0: /* register port */
        sms_fm_write(m->fm, 0, data & 0x3f);
        break;
    case 1: /* data port */
        sms_fm_write(m->fm, 1, data);
        break;
    case 2: /* control port */
    case 3: /* mirror */
        fm_unit_set_audio_control(m, data);
        break;
    }
}

static uint8_t io_read(sms_machine_t *m, uint8_t port, uint64_t stamp, bool commit)
{
    const sms_model_info_t *mi = m->model;

    if (commit)
        sms_cart_snoop_ioread(m->cart, port);

    if ((port & 0xc0) == 0x40 || (port & 0xc0) == 0x80)
        return vdp_read(m, port, stamp, commit);

    if (mi->is_mark_iii)
    {
        if ((port & 0xc0) == 0xc0)
            return sg1000m3_peripheral_r(m, port & 0x07);
        return 0xff;
    }
    if (mi->is_smsj)
    {
        switch (port)
        {
        case 0xc0: case 0xdc: return sms_input_port_dc_r(m, commit);
        case 0xc1: case 0xdd: return sms_input_port_dd_r(m, commit);
        case 0xf2:            return smsj_audio_control_r(m);
        default:              return 0xff;
        }
    }
    if ((port & 0xc1) == 0xc0)
        return sms_input_port_dc_r(m, commit);
    if ((port & 0xc1) == 0xc1)
        return sms_input_port_dd_r(m, commit);
    return 0xff;   /* unmap_value_high */
}

static void io_write(sms_machine_t *m, uint8_t port, uint8_t data, uint64_t stamp)
{
    const sms_model_info_t *mi = m->model;

    /* the cartridge's BIOS snoop sees every OUT */
    sms_cart_snoop_iowrite(m->cart, port, data);

    if ((port & 0xc0) == 0x40)
    {
        sms_psg_write(&m->psg, data);
        return;
    }
    if ((port & 0xc0) == 0x80)
    {
        vdp_write(m, port, data, stamp);
        return;
    }

    if (mi->is_mark_iii)
    {
        if ((port & 0xc0) == 0xc0)
            sg1000m3_peripheral_w(m, port & 0x07, data);
        return;
    }
    if (mi->is_smsj)
    {
        switch (port)
        {
        case 0x3e: sms_mem_control_w(m, data); break;
        case 0x3f: sms_io_control_w(m, data, stamp); break;
        case 0xf0: sms_fm_write(m->fm, 0, data & 0x3f); break;
        case 0xf1: sms_fm_write(m->fm, 1, data); break;
        case 0xf2: smsj_set_audio_control(m, data); break;
        }
        return;
    }
    if ((port & 0xc1) == 0x00)
        sms_mem_control_w(m, data);
    else if ((port & 0xc1) == 0x01)
        sms_io_control_w(m, data, stamp);
}

/* ---------------------------------------------------------------------- */
/* the cycle loop                                                         */
/* ---------------------------------------------------------------------- */

static void tick(sms_machine_t *m)
{
    const uint64_t c = m->cycles;
    bool irq, nmi;
    uint64_t pins;

    sms_vdp_run_until(&m->vdp, c >= VDP_LAG ? c - VDP_LAG : 0);
    sms_vdp_lines_at(&m->vdp, c + IRQ_AHEAD, &irq, &nmi);

    pins = m->pins & ~(Z80_INT | Z80_NMI);
    if (irq)
        pins |= Z80_INT;
    if (nmi)
        pins |= Z80_NMI;

    pins = z80_tick(&m->cpu, pins);

    if (pins & Z80_MREQ)
    {
        const uint16_t addr = Z80_GET_ADDR(pins);
        if (pins & Z80_RD)
        {
            const uint8_t d = mem_read(m, addr, (pins & Z80_M1) != 0, true);
            Z80_SET_DATA(pins, d);
            if (m->watch & SMS_WATCH_MEMR)
                m->bus_hook(m, m->bus_hook_user, SMS_BUS_MEMR, addr, d);
        }
        else if (pins & Z80_WR)
        {
            const uint8_t d = Z80_GET_DATA(pins);
            mem_write(m, addr, d);
            if (m->watch & SMS_WATCH_MEMW)
                m->bus_hook(m, m->bus_hook_user, SMS_BUS_MEMW, addr, d);
        }
    }
    else if (pins & Z80_IORQ)
    {
        const uint8_t port = (uint8_t)Z80_GET_ADDR(pins);
        if (pins & Z80_M1)
        {
            /* interrupt acknowledge: nothing drives the bus, the pull-ups
             * read $FF (RST 38h in IM 0) */
            Z80_SET_DATA(pins, 0xff);
        }
        else if (pins & Z80_RD)
        {
            const uint8_t d = io_read(m, port, c - SKEW_IOR, true);
            Z80_SET_DATA(pins, d);
            if (m->watch & SMS_WATCH_IOR)
                m->bus_hook(m, m->bus_hook_user, SMS_BUS_IOR, port, d);
        }
        else if (pins & Z80_WR)
        {
            const uint8_t d = Z80_GET_DATA(pins);
            io_write(m, port, d, c - SKEW_IOW);
            if (m->watch & SMS_WATCH_IOW)
                m->bus_hook(m, m->bus_hook_user, SMS_BUS_IOW, port, d);
        }
    }
    (void)SKEW_MEM;

    sms_psg_tick(&m->psg);
    if (m->fm && ++m->fm_phase >= SMS_FM_CLOCKS_PER_SAMPLE)
    {
        m->fm_phase = 0;
        m->mix.fm_hold = sms_fm_generate(m->fm);
    }
    sms_mix_tick(&m->mix, m->psg.sample);

    m->pins = pins;
    m->cycles = c + 1;

    if (z80_opdone(&m->cpu) && m->instr_hook)
        m->instr_hook(m, m->instr_hook_user);
}

void sms_machine_run_frame(sms_machine_t *m)
{
    m->vdp.frame_ready = false;
    while (!m->vdp.frame_ready)
        tick(m);
}

void sms_machine_step_instruction(sms_machine_t *m)
{
    do
        tick(m);
    while (!z80_opdone(&m->cpu));
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                              */
/* ---------------------------------------------------------------------- */

/* MAME's machine_reset (and the devices it resets with it). */
static void machine_reset(sms_machine_t *m)
{
    if (m->model->is_smsj)
        smsj_set_audio_control(m, 0x00);

    if (m->model->has_rapid_button)
    {
        m->csync_counter = 0;
        m->rapid_mode = 0x00;
        m->rapid_read_state = 0;
        m->rapid_last_dc = 0xff;
        m->rapid_last_dd = 0xff;
    }

    if (!m->model->is_mark_iii)
    {
        m->io_ctrl_reg = 0xff;
        m->ctrl1_th_latch = 0;
        m->ctrl2_th_latch = 0;
    }

    setup_bios(m);
    setup_media_slots(m);

    if (m->fm_unit)
        fm_unit_set_audio_control(m, 0x00);   /* sega_fm_unit_device::device_reset */
}

/* MAME's Z80 at power-on: every register zero (z80_device::device_start),
 * where z80.h's z80_init sets most of them to $FFFF. It matters on a
 * BIOS-less boot: a program that pushes before it loads SP writes the top
 * of RAM (and the mapper registers) differently. */
static void cpu_power_on(sms_machine_t *m)
{
    z80_t *cpu = &m->cpu;

    m->pins = z80_init(cpu);
    cpu->af = cpu->bc = cpu->de = cpu->hl = 0;
    cpu->ix = cpu->iy = cpu->wz = cpu->sp = 0;
    cpu->af2 = cpu->bc2 = cpu->de2 = cpu->hl2 = 0;
    cpu->ir = 0;
    cpu->im = 0;
    cpu->iff1 = cpu->iff2 = false;
}

/* z80_device::device_reset: PC, WZ, I, R and the interrupt flip-flops only;
 * the other registers, SP and the interrupt mode survive. */
static void cpu_soft_reset(sms_machine_t *m)
{
    const z80_t keep = m->cpu;
    z80_t *cpu = &m->cpu;

    m->pins = z80_reset(cpu);
    cpu->af = keep.af;
    cpu->bc = keep.bc;
    cpu->de = keep.de;
    cpu->hl = keep.hl;
    cpu->ix = keep.ix;
    cpu->iy = keep.iy;
    cpu->sp = keep.sp;
    cpu->af2 = keep.af2;
    cpu->bc2 = keep.bc2;
    cpu->de2 = keep.de2;
    cpu->hl2 = keep.hl2;
    cpu->im = keep.im;
    cpu->wz = 0;
    cpu->ir = 0;
    cpu->iff1 = cpu->iff2 = false;
}

void sms_machine_init(sms_machine_t *m, sms_model_t model, const uint8_t *bios,
                      uint32_t bios_size, const uint8_t *instruments,
                      bool fm_unit, bool fm_unit_mutes_psg, sms_cart_t *cart)
{
    const sms_model_info_t *mi;

    if ((unsigned)model >= SMS_MODEL_COUNT)
        model = SMS_MODEL_SMS1;
    mi = &sms_models[model];

    memset(m, 0, sizeof *m);
    m->model = mi;
    m->model_id = model;
    m->cart = cart;

    sms_vdp_init(&m->vdp, mi->vdp, mi->is_pal);
    m->vdp.csync_cb = rapid_n_csync_callback;
    m->vdp.cb_user = m;
    sms_psg_init(&m->psg);

    {
        const double vdp_clock = mi->is_pal ? SMS_MASTER_CLOCK_PAL / 5.0 : SMS_XTAL_NTSC;
        sms_mix_init(&m->mix, vdp_clock / SMS_MCLK_PER_CYCLE);
    }

    m->fm_unit = mi->is_mark_iii && fm_unit;
    m->fm_unit_mutes_psg = fm_unit_mutes_psg;
    if (mi->is_smsj || m->fm_unit)
        m->fm = sms_fm_create(instruments);

    /* machine_start: RAM (the $F0 pattern on the consoles with the Japanese
     * cartridge slot -- alibaba and blockhol need RET P at the IRQ vector) */
    if (mi->has_jpn_sms_cart_slot)
        memset(m->mainram, 0xf0, sizeof m->mainram);
    m->ctrl1_th_state = 1;
    m->ctrl2_th_state = 1;

    /* setup_bios: the image goes into the model's "user1" region, the rest
     * of which is zero; a region whose first byte is zero is no BIOS */
    if (mi->bios_region && bios && bios_size && bios[0] != 0x00)
    {
        m->BIOS = calloc(1, mi->bios_region);
        if (m->BIOS)
        {
            memcpy(m->BIOS, bios, bios_size < mi->bios_region ? bios_size : mi->bios_region);
            m->bios_page_count = (uint8_t)(mi->bios_region / 0x4000);
        }
    }

    cpu_power_on(m);
    machine_reset(m);
    sms_machine_latch_inputs(m);
}

void sms_machine_free(sms_machine_t *m)
{
    sms_fm_free(m->fm);
    m->fm = NULL;
    free(m->BIOS);
    m->BIOS = NULL;
}

void sms_machine_soft_reset(sms_machine_t *m)
{
    cpu_soft_reset(m);
    sms_vdp_reset(&m->vdp);
    sms_fm_reset(m->fm);
    sms_cart_console_reset(m->cart);
    machine_reset(m);
}

/* ---------------------------------------------------------------------- */
/* debugger access                                                        */
/* ---------------------------------------------------------------------- */

uint8_t sms_machine_peek(sms_machine_t *m, uint16_t addr)
{
    return mem_read(m, addr, false, false);
}

void sms_machine_poke(sms_machine_t *m, uint16_t addr, uint8_t data)
{
    if (addr < 0xc000)
    {
        /* into what serves the address (the cartridge's memory) */
        sms_cart_poke(m->cart, addr, data);
        return;
    }
    mem_write(m, addr, data);
}

uint8_t sms_machine_io_peek(sms_machine_t *m, uint8_t port)
{
    return io_read(m, port, m->cycles, false);
}
