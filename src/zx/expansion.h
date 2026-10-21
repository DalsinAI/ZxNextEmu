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
 *                 interrupt disable
 *
 * The "divmmc" interface is the divMMC alone; the MB03+ Ultimate fits them
 * all (its own ULA+ is the machine's, always present on the classic models).
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

struct zx;
struct nx_sd;
struct zx_blockdev;

enum {
    ZX_EXP_DIVMMC     = 1,
    ZX_EXP_TURBOSOUND = 2,
    ZX_EXP_DAC        = 4,
    ZX_EXP_MOUSE      = 8,
    ZX_EXP_TIMEX      = 16
};

#define ZX_EXP_MB03 (ZX_EXP_DIVMMC | ZX_EXP_TURBOSOUND | ZX_EXP_DAC | ZX_EXP_MOUSE | ZX_EXP_TIMEX)

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

    /* DACs: unsigned 8-bit, 0x80 is silence */
    uint8_t dac[4];
    int64_t dac_l, dac_r;        /* level x T-states since the last sample */

    zx_kmouse mouse;

    /* Timex */
    uint8_t port_ff;
} zx_exp;

/* Plug an interface into a classic machine (not the Next). div_rom is the
 * 8K esxDOS ROM when the features include the divMMC. 0 on success. */
int zx_exp_attach(struct zx *m, uint32_t features, const uint8_t *div_rom, size_t rom_len);
void zx_exp_free(struct zx *m);
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

/* Video: where the ULA reads a pixel byte and its attribute (offsets in
 * the screen page), given the standard offsets. */
static inline void zx_exp_screen(const zx_exp *e, uint32_t *pix, uint32_t *attr)
{
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
