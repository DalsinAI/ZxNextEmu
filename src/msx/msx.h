/*
 * MSX1 and MSX2 machines.
 *
 * The Z80 runs at 3.579545 MHz with the MSX's extra wait state on every
 * M1 cycle (opcode fetches, prefixes and interrupt acknowledge take one
 * T-state more). All timing is in CPU T-states from the start of the
 * frame, which is the start of the VDP's vertical sync; the VDP counts
 * six of its ticks to each T-state.
 *
 * Slots (as on a Philips NMS 8250):
 *   0     main BIOS (0000-7FFF) and, with C-BIOS, its logo ROM (8000-BFFF)
 *   1, 2  cartridge slots
 *   3-0   MSX1: 64K RAM; MSX2: the sub ROM (0000-3FFF)
 *   3-2   MSX2: the memory mapper (1 MB)
 *   3-3   the disk ROM: a 16K disk ROM drives a WD2793 floppy interface;
 *         a larger one is a Sunrise IDE flash ROM (Nextor)
 */
#ifndef MSX_MSX_H
#define MSX_MSX_H

#include <stddef.h>
#include <stdint.h>

#include "../z80/z80.h"
#include "../zx/ay.h"
#include "vdp.h"
#include "rtc.h"
#include "cart.h"
#include "fdc.h"
#include "ide.h"

#define MSX_CPU_HZ     3579545
#define MSX_AUDIO_RATE 44100

typedef struct msx_model {
    const char *id, *name;
    vdp_type vdp;
    uint8_t msx2;
    uint32_t ram_kb;           /* MSX1: plain RAM; MSX2: memory mapper */
} msx_model;

enum { EXT_NONE, EXT_DISK, EXT_IDE };

/* What occupies each (primary, secondary) slot. */
enum { DEV_NONE, DEV_BIOS, DEV_SUB, DEV_RAM, DEV_MAPPER, DEV_CART0, DEV_CART1, DEV_EXT };

typedef struct msx_typer {
    char buf[1024];
    uint32_t pos, len;
    int32_t phase, wait;
} msx_typer;

typedef struct msx_mouse {
    int32_t acc_x, acc_y;      /* movement not yet read */
    uint8_t lx, ly;            /* latched at the start of a read */
    uint8_t phase, pin8;
    uint8_t buttons;
    uint8_t present;
    uint64_t last;             /* T-state of the last strobe */
} msx_mouse;

typedef struct msx {
    const msx_model *model;
    z80 cpu;
    z80_bus bus;
    uint32_t t, frame_t;       /* T-state in the frame, T-states in it */
    uint32_t next_event;       /* the VDP's next interrupt change */
    uint64_t frames;

    /* Memory */
    uint8_t *rom_main, *rom_logo, *rom_sub, *rom_ext;   /* owned */
    uint32_t ext_len;
    uint8_t ext_type;
    uint8_t *ram;              /* owned */
    uint32_t ram_size;
    uint8_t mapper[4];
    uint8_t slot_reg;          /* PPI port A */
    uint8_t sub_reg[4];
    uint8_t expanded[4];
    uint8_t slot_dev[4][4];
    const uint8_t *rd[8];      /* 8K banks: where reads come from, NULL = device */
    uint8_t *wr[8];            /* where writes go, NULL = device */
    uint8_t dev[8];
    uint8_t page3_exp;         /* FFFF is the secondary slot register */

    /* PPI, keyboard, PSG, joysticks */
    uint8_t ppi_c;
    uint8_t keys[11];          /* active low */
    ay_chip psg;
    uint8_t psg_sel;
    uint8_t joy[2];            /* zxm bits: 0 right, 1 left, 2 down, 3 up, 4 A, 5 B */
    msx_mouse mouse;

    vdp vdp;
    uint8_t *vram;             /* owned */

    msx_rtc rtc;
    msx_cart cart[2];
    msx_fdc fdc;
    msx_ide ide;

    /* Sound */
    int16_t audio[MSX_AUDIO_RATE / 40 * 2];
    int audio_samples;
    uint32_t audio_t;
    uint64_t audio_frac;
    int32_t click_acc;
    uint32_t psg_half;
    uint64_t time_base;        /* T-states before this frame */
    uint64_t rtc_last;         /* when the RTC was last brought up to date */

    msx_typer typer;
} msx;

extern const msx_model msx_models[];
#define MSX_MODELS 4

/* roms: main BIOS (32K); sub ROM (16K, MSX2); logo ROM (16K, optional);
 * disk ROM (optional: 16K floppy, or 64K-512K Sunrise IDE). */
int msx_create(msx *m, const msx_model *model, const uint8_t *main_rom, uint32_t main_len,
               const uint8_t *sub, uint32_t sub_len, const uint8_t *logo, uint32_t logo_len,
               const uint8_t *ext, uint32_t ext_len);
void msx_free(msx *m);
void msx_reset(msx *m, int power);
void msx_run_frame(msx *m);

void msx_key(msx *m, int row, int bit, int down);
void msx_joystick(msx *m, int port, uint8_t state);
void msx_mouse_move(msx *m, int dx, int dy, int buttons);

void msx_type(msx *m, const char *text, int delay_frames);
int msx_typing(const msx *m);

/* Media: 0 on success. Cartridge slot 0/1 (a copy is kept); floppy and
 * IDE units own the block device from then on. */
int msx_cart_insert(msx *m, int slot, const uint8_t *data, uint32_t len, const char *name);
void msx_cart_eject(msx *m, int slot);
int msx_disk_insert(msx *m, int drive, struct zx_blockdev *bd);
int msx_ide_attach(msx *m, int unit, struct zx_blockdev *bd);

size_t msx_state_size(const msx *m);
int msx_state_save(const msx *m, void *buf, size_t len);
int msx_state_load(msx *m, const void *buf, size_t len);

uint8_t msx_peek(const msx *m, uint16_t addr);
void msx_poke(msx *m, uint16_t addr, uint8_t v);

#endif
