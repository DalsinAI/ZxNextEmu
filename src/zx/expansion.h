/*
 * Interfaces on the classic Spectrums' expansion port.
 *
 * One struct, allocated when an interface is plugged in, holds every
 * device the interface brings; the feature bits say which are fitted:
 *
 *   divMMC        esxDOS ROM, 512K RAM, two SD cards on SPI (ports E3 E7 EB),
 *                 automapping at the ROM entry points
 *   TurboSound    three AY-3-8912s on FFFD/BFFD, picked by writing FF FE FD
 *                 to FFFD (replaces the machine's own AY; adds one to a 48K)
 *   DACs          SpecDrum (DF), Covox (FB), SounDrive / stereo Covox
 *                 (1F 0F 4F 5F): A and B on the left, C and D on the right
 *   Kempston mouse  ports FBDF FFDF FADF
 *   Timex video   port FF: second screen, hi-colour (8x1 attributes),
 *                 hi-res (512x192 in two colours), interrupt disable. The
 *                 machine's picture becomes 640 wide (zx.h, screen_w) so
 *                 hi-res keeps every pixel
 *   Radastan      the ZX-Uno's 128x96 16-colour mode: ZX-Uno registers on
 *                 FC3B/FD3B (40 on, 41 offset, 42 padding, 43 palette
 *                 quarter), or 3 in the ULA+ mode register
 *   GigaScreen    colours mixed for a steady picture (m_zxi_00B or
 *                 g_zxi_008): 1 the two 128K screens, 2 this frame with the
 *                 last, 3 that only while the program flips screens
 *   SAA1099       register select on 6F3B or 1FF, data on 6E3B or FF (FF
 *                 is shared with the Timex port, as on the hardware)
 *   General Sound the GS card with its own Z80 (devices/gs.h) on B3/BB,
 *                 when its ROM is given (zx_exp_attach_gs)
 *   SID           6581/8580 on 6C3B (register) and 6D3B (data); register
 *                 1F picks the model and the PAL or NTSC clock
 *   ZXi registers the MB03+'s settings (its Programmer's Reference): 703B
 *                 selects a column and 713B-773B read or write it in the
 *                 eight m_zxi rows; 7C3B and 7D3B do the same for the
 *                 general g_zxi registers. m_zxi_001 switches the sound
 *                 devices (zx_exp_on), m_zxi_00C the Timex modes and
 *                 m_zxi_019 the SID model; the rest are kept and read back
 *                 but do nothing yet.
 *
 * The "divmmc" interface is the divMMC alone; the MB03+ Ultimate fits them
 * all (its own ULA+ is the machine's, always present on the classic models).
 * Without the ZXi registers every fitted device is on; with them, they start
 * as the MB03+ SetUp leaves them: all on but the SAA1099.
 *
 * zx.c calls in from its memory, I/O, audio and video paths only when
 * m->exp is set, so a machine without an interface runs as before.
 */
#ifndef ZXN_EXPANSION_H
#define ZXN_EXPANSION_H

#include <stddef.h>
#include <stdint.h>

#include "ay.h"
#include "kmouse.h"
#include "../devices/saa1099.h"
#include "../devices/sid.h"
#include "../devices/gs.h"

struct zx;
struct nx_sd;
struct zx_blockdev;

enum {
    ZX_EXP_DIVMMC     = 1,
    ZX_EXP_TURBOSOUND = 2,
    ZX_EXP_DAC        = 4,
    ZX_EXP_MOUSE      = 8,
    ZX_EXP_TIMEX      = 16,
    ZX_EXP_SAA        = 32,
    ZX_EXP_SID        = 64,
    ZX_EXP_ZXI        = 128,
    ZX_EXP_RADASTAN   = 256,
    ZX_EXP_GIGA       = 512,
    ZX_EXP_GS         = 1024     /* set by zx_exp_attach_gs */
};

#define ZX_EXP_ALL 0xffffffffu

#define ZX_EXP_MB03 (ZX_EXP_DIVMMC | ZX_EXP_TURBOSOUND | ZX_EXP_DAC | ZX_EXP_MOUSE | ZX_EXP_TIMEX \
                     | ZX_EXP_SAA | ZX_EXP_SID | ZX_EXP_ZXI | ZX_EXP_RADASTAN | ZX_EXP_GIGA)

/* m_zxi_001, audio select */
enum {
    ZXI_BEEPER = 1, ZXI_AY = 2, ZXI_SAA = 4, ZXI_DAC = 8, ZXI_GS = 16, ZXI_SID = 32, ZXI_TAPEIN = 64
};
#define ZXI_AUDIO_DEFAULT (ZXI_BEEPER | ZXI_AY | ZXI_DAC | ZXI_GS | ZXI_SID | ZXI_TAPEIN)

#define ZX_DIV_BANKS 64          /* 512K of divMMC RAM in 8K banks */

typedef struct zx_exp {
    uint32_t features;

    /* divMMC */
    uint8_t div_rom[0x2000];     /* esxDOS */
    uint8_t div_ram[ZX_DIV_BANKS][0x2000];
    uint8_t port_e3;             /* bit 7 CONMEM, bit 6 MAPRAM (sticky), 5-0 bank */
    uint8_t port_e7;             /* SPI chip selects, active low: bit 0 card 0, bit 1 card 1 */
    uint8_t spi_miso;            /* byte the last SPI transfer brought in */
    uint8_t automap;             /* divMMC memory paged in by an entry point */
    struct nx_sd *sd[2];

    /* TurboSound */
    ay_chip ay[3];
    uint8_t ay_select;
    uint8_t acb;                 /* stereo: 0 ABC, 1 ACB */
    uint32_t ay_half;

    /* DACs: unsigned 8-bit, 0x80 is silence. 0-1 left, 2-3 right, 4 (the
     * Czech DAC's B on 3F) both. */
    uint8_t dac[5];
    int64_t dac_l, dac_r;        /* level x T-states since the last sample */

    zx_kmouse mouse;

    /* Timex */
    uint8_t port_ff;

    saa1099 saa;
    uint64_t saa_frac;           /* 8 MHz clocks x cpu_hz still to run */

    sid sid;
    uint8_t sid_sel;
    uint64_t sid_frac;

    /* ZXi: m_zxi_000-7FF as zxi[row][column], g_zxi_000-0FF */
    uint8_t zxi_sel, gzxi_sel;
    uint8_t zxi[8][256];
    uint8_t gzxi[256];

    /* ZX-Uno registers (u_zxi): Radastan, the core ID */
    uint8_t uno_sel, uno_hi, uno_id;
    uint8_t uno[256];
    uint16_t radas_offset;

    /* The 640-wide picture is the interface's (not an ATM's own). */
    uint8_t own_wide;

    /* GigaScreen: the last frame as drawn, and how the program flips */
    uint16_t *giga_prev;
    uint8_t giga_buf, giga_flips;

    gs *gs;                      /* General Sound, or NULL */
    uint64_t gs_frac;
} zx_exp;

/* A device is fitted and, where the ZXi registers are, switched on
 * (ZX_EXP_ALL for the machine's own sound: the beeper, the tape input). */
static inline int zx_exp_on(const zx_exp *e, uint32_t feature, uint8_t zxi_audio)
{
    return (e->features & feature) && (!(e->features & ZX_EXP_ZXI) || !zxi_audio || (e->zxi[0][1] & zxi_audio));
}

/* The interface's AY chips stand in for the machine's own. */
static inline int zx_exp_turbosound(const zx_exp *e)
{
    return zx_exp_on(e, ZX_EXP_TURBOSOUND, ZXI_AY);
}

/* Plug an interface into a classic machine (not the Next). div_rom is the
 * 8K esxDOS ROM when the features include the divMMC. 0 on success. */
int zx_exp_attach(struct zx *m, uint32_t features, const uint8_t *div_rom, size_t rom_len);
void zx_exp_free(struct zx *m);
/* Fit the General Sound, with its 32K ROM, to an attached MB03+. */
int zx_exp_attach_gs(struct zx *m, const uint8_t *rom, size_t rom_len);
void zx_exp_power_on(struct zx *m);   /* MAPRAM clears only at power-on */
void zx_exp_reset(struct zx *m);
void zx_exp_frame_end(struct zx *m);

/* SD card in the divMMC, slot 0 or 1; NULL ejects. The interface owns the
 * device once this succeeds. */
int zx_exp_attach_sd(struct zx *m, int slot, struct zx_blockdev *bd);

/* Memory below 0x4000 while the divMMC is paged in (m->div_map). */
uint8_t zx_exp_read(const struct zx *m, uint16_t addr);
void zx_exp_write(struct zx *m, uint16_t addr, uint8_t v);
/* An opcode fetch: the divMMC's automapping. Returns the byte. */
uint8_t zx_exp_fetch(struct zx *m, uint16_t addr);

/* I/O: read returns 1 and sets *v when an interface device answers. */
int zx_exp_io_read(struct zx *m, uint16_t port, uint8_t *v);
void zx_exp_io_write(struct zx *m, uint16_t port, uint8_t v);

/* Audio: run for dt T-states, then take one sample's worth (span T-states)
 * and add it to l and r. */
void zx_exp_audio_run(struct zx *m, uint32_t dt);
void zx_exp_audio_take(struct zx *m, uint32_t span, int32_t *l, int32_t *r);

/* Video: draw the 8 pixels of a chunk (paper row and column, or outside
 * the paper for the border) into px, and 16 into wide when the picture is
 * 640 wide, if a mode of the interface's own shows there. 0 to leave it to
 * the ULA. */
int zx_exp_draw(struct zx *m, int row, int col, uint16_t *px, uint16_t *wide);

/* Where the ULA reads a pixel byte and its attribute (offsets in the
 * screen page), given the standard offsets. */
static inline void zx_exp_screen(const zx_exp *e, uint32_t *pix, uint32_t *attr)
{
    if (!(e->features & ZX_EXP_TIMEX))
        return;
    switch (e->port_ff & 7) {
    case 1:                      /* second screen at 0x6000 */
        *pix += 0x2000;
        *attr += 0x2000;
        break;
    case 2:                      /* hi-colour: an attribute byte per pixel byte */
        *attr = *pix + 0x2000;
        break;
    default:
        break;
    }
}

size_t zx_exp_state_size(const struct zx *m);
void zx_exp_state_save(const struct zx *m, uint8_t *out);
/* has_sd: whether the saved machine had each card; a card attached since
 * keeps its own state. */
void zx_exp_state_load(struct zx *m, const uint8_t *in, const int has_sd[2]);

#endif
