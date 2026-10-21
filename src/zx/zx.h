/*
 * Classic Spectrum family: 48K, 128K, +2, +2A, +3 and Pentagon 128.
 *
 * One machine struct covers all of them; the model picks the timing table,
 * the paging rules and the contention behaviour. All timing is in CPU
 * T-states counted from the start of the frame (the rising edge of /INT).
 */
#ifndef ZXN_ZX_H
#define ZXN_ZX_H

#include <stdint.h>
#include <stddef.h>

#include "../z80/z80.h"
#include "ay.h"

typedef enum {
    ZX_48K,
    ZX_128K,
    ZX_PLUS2,
    ZX_PLUS2A,
    ZX_PLUS3,
    ZX_PENTAGON,
    ZX_NEXT,
    ZX_MODEL_COUNT
} zx_model;

/* Visible picture: 256x192 paper with a 32 pixel border all round. */
#define ZX_SCREEN_W 320
#define ZX_SCREEN_H 256
#define ZX_BORDER_X 32
#define ZX_BORDER_Y 32

#define ZX_AUDIO_RATE 44100

typedef struct zx_timing {
    const char *name;
    uint32_t frame_t;     /* T-states per frame */
    uint32_t line_t;      /* T-states per scanline */
    uint32_t int_len;     /* /INT low for this many T-states */
    uint32_t paper_t;     /* T-state at which the top-left paper pixel is drawn */
    uint32_t contend_t;   /* first contended T-state (-1 for none) */
    uint8_t pattern[8];   /* contention delay by (t - contend_t) % 8 */
    uint8_t contend_io;   /* ULA-style I/O contention (48K/128K) */
    uint8_t contend_internal; /* contention on non-MREQ cycles (not +2A/+3) */
    uint32_t cpu_hz;
    uint32_t ay_hz;
} zx_timing;

extern const zx_timing zx_timings[ZX_MODEL_COUNT];

typedef struct zx {
    zx_model model;
    const zx_timing *tm;
    z80 cpu;
    z80_bus bus;

    uint32_t t;            /* T-state within the current frame */
    uint64_t frames;

    /* Memory: 16K pages. ROM pages 0..3, RAM pages 0..7. */
    uint8_t rom[4][0x4000];
    uint8_t ram[8][0x4000];
    uint8_t *rd[4];        /* read map for the four 16K slots */
    uint8_t *wr[4];        /* write map; NULL = ROM */
    uint8_t contended[4];  /* slot is contended memory */
    int screen_page;       /* RAM page the ULA displays (5 or 7) */
    uint8_t port_7ffd, port_1ffd;
    int paging_locked;

    /* ULA */
    uint8_t border;
    uint8_t speaker;       /* bit 4 of the last FE write: EAR out */
    uint8_t mic;           /* bit 3 */
    uint8_t keys[8];       /* keyboard half-rows, active low, as read */
    uint8_t ear_in;        /* EAR input level from the tape (0/1) */
    uint8_t kempston;      /* Kempston joystick state (active high) */

    /* ULA+ (ports 0xBF3B / 0xFF3B): 64-colour palette, GGGRRRBB. */
    uint8_t ulaplus_reg;   /* selected register: 00pppppp palette, 01000000 mode */
    uint8_t ulaplus_mode;  /* bit 0: palette on */
    uint8_t ulaplus_pal[64];

    /* Video: rendered lazily up to the current T-state. */
    uint16_t screen[ZX_SCREEN_H][ZX_SCREEN_W]; /* RGB333 */
    uint32_t video_chunk;  /* next 8-pixel chunk to draw */

    /* Audio */
    ay_chip ay;
    int16_t audio[ZX_AUDIO_RATE / 25 * 2]; /* stereo, one frame (and change) */
    int audio_samples;     /* samples produced this frame */
    uint32_t audio_t;      /* T-state the audio has been generated up to */
    uint64_t audio_frac;   /* fractional sample position, 32.32 */
    int32_t beeper_acc;
    uint32_t ay_half;

    struct zx_tape *tape;

    /* The picture to show: RGB333, screen_w x screen_h, pitch in pixels.
     * 320x256 on the classic models; 640x256 on the Next, whose pixels are
     * half as wide (each classic pixel is two). */
    const uint16_t *pixels;
    int screen_w, screen_h, pitch;

    struct zxn *next;      /* ZX Spectrum Next state (ZX_NEXT only) */
    struct s256 *s256;     /* Spec256 shadow CPUs, or NULL (not owned) */
} zx;

/* Create a machine from its ROM images: roms[i] is 16K for each name
 * zx_rom_file(model, i) gives (the Next needs none). No file I/O. */
int zx_create(zx *m, zx_model model, const uint8_t *const roms[4]);
const char *zx_rom_file(zx_model model, int i);
/* Host side (files.c): create with the ROM files read from roms_dir
 * (see roms/README.md). */
int zx_init(zx *m, zx_model model, const char *roms_dir);
void zx_reset(zx *m);
/* Run one video frame. Video and audio for it are then in m->screen/audio. */
void zx_run_frame(zx *m);

/* Keyboard matrix: row 0..7 (port bit A8..A15), bit 0..4. */
void zx_key(zx *m, int row, int bit, int down);
/* Kempston mouse (Next): host movement in pixels, dy down is positive;
   buttons 1 left, 2 right, 4 middle. */
void zx_mouse(zx *m, int dx, int dy, int dz, int buttons);

/* Write a byte without timing or video side effects (snapshots, traps). */
void zx_poke(zx *m, uint16_t addr, uint8_t v);
uint8_t zx_peek(const zx *m, uint16_t addr);

int zx_load_sna(zx *m, const uint8_t *data, size_t len);
int zx_load_z80(zx *m, const uint8_t *data, size_t len);

/* Release memory owned by the machine (the Next allocates its RAM). */
void zx_free(zx *m);

/* Attach an SD card (Next only), slot 0 or 1. The machine closes the
 * device when it is replaced or the machine is freed. */
struct zx_blockdev;
int zx_attach_sd_dev(zx *m, int slot, struct zx_blockdev *bd);
/* Host side (files.c): an image file or a folder, see storage/blockdev.h. */
int zx_attach_sd(zx *m, int slot, const char *path);

/* Full machine state, media contents excepted (they stay attached). Load
 * into a machine of the same model built by the same core; returns 0 on
 * success, -1 if the state does not fit this machine. */
size_t zx_state_size(const zx *m);
int zx_state_save(const zx *m, void *buf, size_t len);
int zx_state_load(zx *m, const void *buf, size_t len);

/* Model by short name: 48, 128, plus2, plus2a, plus3, pentagon, next. */
int zx_model_from_name(const char *name, zx_model *out);

#endif
