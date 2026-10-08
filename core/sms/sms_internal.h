/* sms_internal.h -- the emulated machine, shared by the core modules.
 *
 * The machine logic is transposed from MAME's Sega Master System driver
 * (BSD-3-Clause; see COMPLIANCE.md):
 *
 *   src/mame/sega/sms.cpp, sms.h, sms_m.cpp   memory/IO maps, the memory and
 *                                             I/O control ports, BIOS paging,
 *                                             the Japanese FM ports, models
 *   src/devices/video/315_5124.cpp/.h         the 315-5124 / 315-5246 VDP
 *   src/devices/sound/sn76496.cpp             the VDP's PSG (SEGAPSG)
 *   src/devices/bus/sg1000_exp/fm_unit.cpp    the Mark III FM Sound Unit
 *
 * Field names deliberately keep MAME's own (minus the m_ prefix) so the two
 * implementations can be diffed side by side. The Game Gear, the store
 * display unit, the SegaScope glasses, the light phaser and the other
 * peripherals are not carried over: this app's machines are the Master
 * Systems and the Mark III with joypads.
 *
 * The CPU is floooh's cycle-stepped chips z80.h (zlib, vendored in z80/),
 * not MAME's: one z80_tick() is one T-state = 3 clocks of the VDP's master
 * clock, and a scanline is exactly 228 T-states (684 master clocks, 342
 * pixels), so the CPU and the beam never drift.
 *
 * MAME's VDP is driven by emu_timers placed with screen().time_until_pos();
 * here they are a per-line event table run against the CPU's cycle count
 * (vdp.c). The conversions MAME's screen does (vpos/hpos from the elapsed
 * time, rounded to the nearest pixel) are transposed exactly; the beam's
 * time origin is MAME's too: the start of VBLANK, the line after the visible
 * area, at power-on.
 */

#ifndef SMS_INTERNAL_H
#define SMS_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "z80/z80.h"

#include "fm.h"
#include "fujinet_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- clocks (sms.cpp) ---- */
#define SMS_XTAL_NTSC         10738635.0   /* XTAL(10'738'635), the VDP clock */
#define SMS_MASTER_CLOCK_PAL  53203425.0   /* MASTER_CLOCK_PAL; the VDP gets /5 */
#define SMS_MCLK_PER_CYCLE    3            /* Z80 = VDP clock / 3 */
#define SMS_MCLK_PER_PIXEL    2            /* pixel clock = VDP clock / 2 */

/* ---- the VDP's raster (315_5124.h) ---- */
#define SMS_VDP_WIDTH         342
#define SMS_VDP_HEIGHT_NTSC   262
#define SMS_VDP_HEIGHT_PAL    313
#define SMS_MCLK_PER_LINE     (SMS_VDP_WIDTH * SMS_MCLK_PER_PIXEL)   /* 684 */
#define SMS_CYCLES_PER_LINE   (SMS_MCLK_PER_LINE / SMS_MCLK_PER_CYCLE) /* 228 */
#define SMS_LBORDER_START     (26 + 2 + 14 + 8)
#define SMS_LBORDER_WIDTH     13
#define SMS_RBORDER_WIDTH     15
#define SMS_TBORDER_START     (3 + 13)
#define SMS_NTSC_224_TBORDER  0x0b
#define SMS_PAL_240_TBORDER   0x1e

/* The visible area MAME's screen_sms_*_raw_params set up: 268 columns
 * (2 px of left border, the 256 active, 10 of right border) and the
 * 224-line NTSC / 240-line PAL windows. MAME's snapshots are exactly this. */
#define SMS_VIS_X0            (SMS_LBORDER_START + SMS_LBORDER_WIDTH - 2)        /* 61 */
#define SMS_VIS_X1            (SMS_LBORDER_START + SMS_LBORDER_WIDTH + 256 + 10) /* 329, exclusive */
#define SMS_FB_WIDTH          (SMS_VIS_X1 - SMS_VIS_X0)                          /* 268 */
#define SMS_VIS_Y0_NTSC       (SMS_TBORDER_START + SMS_NTSC_224_TBORDER)         /* 27 */
#define SMS_VIS_Y0_PAL        (SMS_TBORDER_START + SMS_PAL_240_TBORDER)          /* 46 */
#define SMS_FB_HEIGHT_NTSC    224
#define SMS_FB_HEIGHT_PAL     240
#define SMS_FB_MAX_HEIGHT     240

#define SMS_VRAM_SIZE         0x4000
#define SMS_CRAM_SIZE         0x20
#define SMS_PALETTE_SIZE      (64 + 16)
#define SMS_RAM_SIZE          0x2000

#define SMS_AUDIO_RATE        48000

/* ---- the machines ---- */

typedef enum {
    SMS_MODEL_SMS1 = 0,   /* sms1     Master System (315-5124), Reset button */
    SMS_MODEL_SMS1_PAL,   /* sms1pal  Master System (PAL) */
    SMS_MODEL_SMS2,       /* sms      Master System II (315-5246) */
    SMS_MODEL_SMS2_PAL,   /* smspal   Master System II (PAL) */
    SMS_MODEL_SMSJ,       /* smsj     Master System (Japan): YM2413, Rapid button */
    SMS_MODEL_MARK3,      /* sg1000m3 Mark III: no BIOS, optional FM Sound Unit */
    SMS_MODEL_COUNT
} sms_model_t;

typedef enum { SMS_VDP_5124 = 0, SMS_VDP_5246 } sms_vdp_kind_t;

typedef struct {
    const char *id;              /* the MAME machine name */
    const char *name;            /* for menus */
    sms_vdp_kind_t vdp;
    bool is_pal;
    uint32_t bios_region;        /* MAME's "user1" region size; 0 = no BIOS socket */
    bool has_bios_full;          /* the mapper pages the BIOS */
    bool has_bios_2000;          /* the BIOS is not paged (smsj) */
    bool is_smsj;                /* smsj_io, YM2413, csync counter */
    bool is_mark_iii;            /* sg1000m3_io, no $3E/$3F */
    bool ioctrl_region_is_japan; /* TH reads 0 when output */
    bool has_jpn_sms_cart_slot;  /* RAM comes up filled with $F0 */
    bool has_reset_button;       /* the RESET port (sms1 input set) */
    bool has_rapid_button;       /* the RAPID port (smsj input set) */
    uint8_t unmapped_fill;       /* the "maincpu" region's fill byte */
} sms_model_info_t;

extern const sms_model_info_t sms_models[SMS_MODEL_COUNT];

/* ---- the PSG (sn76496.cpp, SEGAPSG: feedback 0x8000, taps 0x01/0x08,
 * negate, divider 8, Sega style) ---- */
typedef struct {
    int32_t vol_table[16];
    int32_t reg[8];
    int32_t last_register;
    int32_t volume[4];
    uint32_t RNG;
    int32_t current_clock;
    int32_t period[4];
    int32_t count[4];
    int32_t output[4];
    uint8_t phase;          /* MAME streams at clock/2: one sample per 2 ticks */
    int16_t sample;         /* the current output sample, already negated */
} sms_psg_t;

void sms_psg_init(sms_psg_t *p);
void sms_psg_write(sms_psg_t *p, uint8_t data);
/* One CPU clock; the PSG advances one MAME sample on every second call. */
void sms_psg_tick(sms_psg_t *p);

/* ---- the VDP (315_5124.cpp) ---- */

/* The per-line events, in hpos order. process_line (hpos 14) arms the
 * ones that apply to its line, as MAME's process_line_timer adjusts its
 * one-shot timers. */
typedef enum {
    SMS_EV_PROCESS_LINE = 0,  /* DISPLAY_CB_HPOS 14 */
    SMS_EV_VINT,              /* VINT_HPOS 24 */
    SMS_EV_HINT,              /* HINT_HPOS 26 */
    SMS_EV_NMI,               /* NMI_HPOS 28 */
    SMS_EV_LBORDER,           /* LBORDER_START 50 */
    SMS_EV_DRAW,              /* DRAW_TIME_SMS 63 */
    SMS_EV_RBORDER,           /* LBORDER_START + 13 + 256 = 319 */
    SMS_EV_EOL,               /* WIDTH - 1 = 341, eol_flag_check */
    SMS_EV_COUNT
} sms_vdp_event_t;

typedef struct sms_vdp {
    /* configuration */
    sms_vdp_kind_t kind;
    bool is_pal;
    int lines;                    /* 262 / 313 */
    int vblank_vpos;              /* the line VBLANK begins on (visarea bottom + 1) */
    int vis_y0;                   /* first visible line */
    int fb_height;                /* visible lines */
    int max_sprite_zoom_hcount, max_sprite_zoom_vcount;
    uint32_t pens[SMS_PALETTE_SIZE]; /* XRGB8888 per palette_lut entry */

    /* MAME state (names as in 315_5124.h) */
    uint8_t reg[16];
    uint8_t status;
    uint8_t pending_status;
    uint8_t reg8copy, reg9copy;
    uint8_t addrmode;
    uint16_t addr;
    bool cram_dirty;
    bool hcounter_latched;
    bool hint_occurred;
    bool pending_hint;
    bool pending_control_write;
    int pending_sprcol_x;
    uint8_t buffer;
    int n_int_state;              /* /INT: 0 asserted */
    int n_nmi_state;              /* /NMI: 0 asserted */
    int n_nmi_in_state;           /* /NMI-IN, the Pause button (0 pressed) */
    int vdp_mode;                 /* 0..4 */
    int y_pixels;                 /* 192, 224, 240 */
    int draw_time;
    uint8_t line_counter;
    uint8_t hcounter;
    uint8_t CRAM[SMS_CRAM_SIZE];
    const uint8_t *frame_timing;
    bool display_disabled;
    uint16_t sprite_attribute_base;
    uint16_t sprite_pattern_line[8];
    int sprite_tile_selected[8];
    int sprite_x[8];
    uint8_t sprite_flags[8];
    int sprite_count;
    int sprite_height;
    int sprite_zoom_scale;
    int current_palette[32];
    uint8_t vram[SMS_VRAM_SIZE];

    /* the beam (replaces screen_device). Absolute master clocks since
     * power-on; MAME's VBLANK origin is master clock 0. */
    uint64_t line_mclk;           /* start of the line `vpos` */
    int vpos;                     /* the line the event engine is on */
    int ev_next;                  /* next event index on that line */
    uint8_t ev_armed;             /* bit per sms_vdp_event_t, set by process_line */
    int draw_param, lborder_param, rborder_param;
    bool line_engine_on;          /* MAME's display timer first fires at (0, 14) */
    uint64_t frame_count;         /* frames completed (VBLANK begins) */
    bool frame_ready;             /* a VBLANK began since the host last looked */

    /* /C-SYNC pulses (MAME's n_csync callback; the smsj counts them) */
    void (*csync_cb)(void *user);
    void *cb_user;

    /* the framebuffer: visible rows only, MAME's visible columns */
    uint32_t fb[SMS_FB_WIDTH * SMS_FB_MAX_HEIGHT];
} sms_vdp_t;

void sms_vdp_init(sms_vdp_t *v, sms_vdp_kind_t kind, bool is_pal);
void sms_vdp_reset(sms_vdp_t *v);   /* MAME device_reset; the beam keeps running */

/* Run every event whose time is visible at CPU cycle `cycle` (see vdp.c). */
void sms_vdp_run_until(sms_vdp_t *v, uint64_t cycle);

/* Port handlers, called at the CPU cycle the access is made at (after
 * sms_vdp_run_until for that cycle). `commit` false is a debugger peek. */
uint8_t sms_vdp_data_read(sms_vdp_t *v, bool commit);
void sms_vdp_data_write(sms_vdp_t *v, uint8_t data);
uint8_t sms_vdp_control_read(sms_vdp_t *v, uint64_t cycle, bool commit);
void sms_vdp_control_write(sms_vdp_t *v, uint8_t data, uint64_t cycle);
uint8_t sms_vdp_vcount_read(const sms_vdp_t *v, uint64_t cycle);
uint8_t sms_vdp_hcount_read(sms_vdp_t *v);
void sms_vdp_hcount_latch(sms_vdp_t *v, uint64_t cycle);

/* The beam at a CPU cycle (MAME screen().vpos() / hpos(), rounded to the
 * nearest pixel). */
int sms_vdp_beam_vpos(const sms_vdp_t *v, uint64_t cycle);
int sms_vdp_beam_hpos(const sms_vdp_t *v, uint64_t cycle);

/* The /INT and /NMI lines as they will be once every event up to `cycle`
 * has run, without running them (the CPU samples its lines ahead of the
 * lagging event engine; see machine.c). */
void sms_vdp_lines_at(const sms_vdp_t *v, uint64_t cycle, bool *irq, bool *nmi);

/* ---- audio: the PSG (every 2 ticks) and the FM (every 72) box-averaged to
 * SMS_AUDIO_RATE stereo float ---- */
typedef struct {
    double acc;               /* fractional output-sample position */
    double step;              /* output samples per CPU clock */
    double sum;
    uint32_t n;
    int32_t fm_hold;          /* the FM's latest sample, zero-order hold */
    float psg_gain, fm_gain;
    float out[2 * 1600];      /* one frame of interleaved stereo at 48 kHz */
    int out_frames;
} sms_mix_t;

void sms_mix_init(sms_mix_t *x, double cpu_hz);
void sms_mix_tick(sms_mix_t *x, int16_t psg_sample);

/* ---- the machine ---- */

#define SMS_IO_EXPANSION 0x80
#define SMS_IO_CARTRIDGE 0x40
#define SMS_IO_CARD      0x20
#define SMS_IO_WORK_RAM  0x10
#define SMS_IO_BIOS_ROM  0x08
#define SMS_IO_CHIP      0x04

#define SMS_ENABLE_NONE    0x00
#define SMS_ENABLE_CART    0x04
#define SMS_ENABLE_BIOS    0x08
#define SMS_ENABLE_EXT_RAM 0x10

/* Joypad bits, active high here (MAME's ports are active low). */
#define SMS_PAD_UP    0x01
#define SMS_PAD_DOWN  0x02
#define SMS_PAD_LEFT  0x04
#define SMS_PAD_RIGHT 0x08
#define SMS_PAD_1     0x10   /* TL */
#define SMS_PAD_2     0x20   /* TR */

typedef struct sms_machine sms_machine_t;

struct sms_machine {
    const sms_model_info_t *model;
    sms_model_t model_id;
    z80_t cpu;
    uint64_t pins;
    uint64_t cycles;              /* T-states since power-on */

    /* memory (sms_state) */
    uint8_t mainram[SMS_RAM_SIZE];
    uint8_t *BIOS;                /* bios_region bytes, or NULL (no BIOS) */
    uint8_t bios_page[4];
    uint8_t bios_page_count;
    uint8_t mapper[4];
    uint8_t io_ctrl_reg;
    uint8_t mem_ctrl_reg;
    uint8_t mem_device_enabled;
    uint8_t smsj_audio_control;
    uint8_t port_dc_reg, port_dd_reg;
    uint8_t ctrl1_th_state, ctrl2_th_state;
    uint8_t ctrl1_th_latch, ctrl2_th_latch;
    uint16_t csync_counter;
    uint8_t rapid_mode, rapid_read_state, rapid_last_dc, rapid_last_dd;

    /* the Mark III FM Sound Unit (bus/sg1000_exp/fm_unit.cpp) */
    bool fm_unit;                 /* fitted (Mark III only) */
    bool fm_unit_mutes_psg;       /* the hardware behaviour; MAME's is no-op */
    uint8_t fm_audio_control;

    sms_vdp_t vdp;
    sms_psg_t psg;
    sms_fm_t *fm;                 /* NULL unless smsj or an FM unit */
    uint32_t fm_phase;
    sms_mix_t mix;
    sms_cart_t *cart;             /* the FujiNet cartridge, always present */

    /* inputs, latched once a frame at VBLANK (MAME's ioport frame update) */
    uint8_t pad_live[2], pad[2];
    bool pause_live, reset_live;
    bool reset_btn;               /* the Reset (sms1) / Rapid (smsj) button */

    /* ---- debugger ----
     * instr_hook fires at every instruction boundary (z80_opdone), on the
     * emulator thread, BEFORE the next instruction's first tick; it may
     * block (that is how the debugger pauses the machine). bus_hook fires
     * on memory and I/O accesses, only while `watch` is nonzero, so a
     * session that never opens the debugger pays one byte test per tick. */
    void (*instr_hook)(sms_machine_t *m, void *user);
    void *instr_hook_user;
    void (*bus_hook)(sms_machine_t *m, void *user, int kind, uint16_t addr,
                     uint8_t data);
    void *bus_hook_user;
    uint8_t watch;                /* SMS_WATCH_* */
};

#define SMS_WATCH_MEMR 0x01
#define SMS_WATCH_MEMW 0x02
#define SMS_WATCH_IOR  0x04
#define SMS_WATCH_IOW  0x08

#define SMS_BUS_MEMR 1
#define SMS_BUS_MEMW 2
#define SMS_BUS_IOR  4
#define SMS_BUS_IOW  8

/* machine.c */
/* Power on: everything comes up fresh. `bios` (bios_size bytes, or NULL) is
 * copied into the model's BIOS region; `instruments` (0x90 bytes or NULL)
 * is the YM2413 patch ROM. The cart is attached, not owned. */
void sms_machine_init(sms_machine_t *m, sms_model_t model, const uint8_t *bios,
                      uint32_t bios_size, const uint8_t *instruments,
                      bool fm_unit, bool fm_unit_mutes_psg, sms_cart_t *cart);
void sms_machine_free(sms_machine_t *m);
/* The console /RESET line (MAME's soft reset): the CPU, the VDP, the FM
 * chip, the machine registers and the cart reset; RAM, the PSG and the
 * beam carry on. */
void sms_machine_soft_reset(sms_machine_t *m);
/* Run until the next VBLANK begins (one frame); returns when a frame is
 * ready in m->vdp.fb. */
void sms_machine_run_frame(sms_machine_t *m);
/* Run one instruction (debugger). */
void sms_machine_step_instruction(sms_machine_t *m);
/* One T-state with no debugger hooks (the debugger's own PC change). */
void sms_machine_tick_quiet(sms_machine_t *m);
/* Latch the live inputs (the frame boundary). */
void sms_machine_latch_inputs(sms_machine_t *m);
/* Debugger access: peek never disturbs the machine (cart hotspots do not
 * fire, the VDP's buffer and latches are untouched); poke is a real bus
 * write. */
uint8_t sms_machine_peek(sms_machine_t *m, uint16_t addr);
void sms_machine_poke(sms_machine_t *m, uint16_t addr, uint8_t data);
/* What the CPU would read from a port right now, without side effects
 * where the hardware has none to spare (VDP data/status return the latched
 * values). */
uint8_t sms_machine_io_peek(sms_machine_t *m, uint8_t port);

#ifdef __cplusplus
}
#endif

#endif /* SMS_INTERNAL_H */
