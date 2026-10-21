/*
 * MSX video: the TMS9918A (MSX1, 60 Hz), TMS9929A (MSX1, 50 Hz) and the
 * Yamaha V9938 (MSX2) with its command engine.
 *
 * Time is counted in VDP ticks of the 21.48 MHz master clock: 1368 to a
 * scanline, 6 to a CPU T-state. A frame starts at the first line of
 * vertical sync. Lines are drawn as the beam reaches them (lazily, when the
 * CPU touches the VDP or the frame ends), so register and VRAM changes
 * made between lines show from the next line on.
 *
 * Picture: 576x240 RGB333. Each pixel of a 256-pixel mode is two pixels
 * wide; 512-pixel modes use one each. 16 pixels (of 256) of border show
 * left and right, and 24 lines above and below a 192-line screen.
 */
#ifndef MSX_VDP_H
#define MSX_VDP_H

#include <stdint.h>

#define VDP_TPL      1368      /* ticks per line */
#define VDP_FB_W     576
#define VDP_FB_H     240

typedef enum {
    VDP_TMS9918,               /* NTSC, 262 lines */
    VDP_TMS9929,               /* PAL, 313 lines */
    VDP_V9938
} vdp_type;

/* The command engine's working state (V9938, vdpcmd.c). */
typedef struct vdp_cmd {
    uint8_t cmd, arg, col;     /* R46, R45, R44 */
    uint8_t busy;              /* CE */
    uint8_t transfer;          /* LMMC/HMMC: R44 holds data; LMCM: S#7 was read */
    uint32_t time;             /* tick of the next VRAM access */
    int32_t sx, sy, dx, dy, nx, ny;
    int32_t asx, adx, anx;
} vdp_cmd;

typedef struct vdp {
    vdp_type type;
    uint8_t *vram;             /* 16K (TMS) or 128K (V9938) */
    uint32_t vram_mask;

    uint8_t reg[48];
    uint8_t stat[10];          /* S#0..S#9 */
    uint16_t palette[16];      /* RGB333 */
    uint16_t pal_raw[16];      /* palette registers as written: 0GGG0RRR0BBB */
    uint8_t latch, latch_full; /* port 99 first byte */
    uint8_t pal_latch, pal_full;
    uint32_t addr;             /* VRAM address counter (17 bits) */
    uint8_t read_ahead;
    uint8_t irq_v, irq_h;      /* F and FH as they drive /INT */

    /* Timing for this frame, fixed at its start. */
    uint8_t pal;               /* 313 lines */
    uint16_t lines;            /* lines per frame */
    uint16_t disp_lines;       /* 192 or 212 */
    int32_t line_zero;         /* frame line of display line 0 */
    uint32_t frame_ticks;
    uint32_t vscan_tick;       /* F set here; ~0u if not this frame */
    uint32_t hscan_tick;       /* FH set here */
    uint32_t next_line;        /* next frame line to draw */
    uint32_t synced;           /* ticks done this frame */
    uint8_t field;             /* odd field, for interlace */
    uint8_t blink_on;          /* T2 / page blink phase */
    uint16_t blink_count;

    vdp_cmd cmd;

    uint16_t fb[VDP_FB_H][VDP_FB_W];
} vdp;

void vdp_init(vdp *v, vdp_type type, uint8_t *vram);
void vdp_reset(vdp *v);
/* Start a frame: fixes the line count (50/60 Hz) for it. */
void vdp_frame_start(vdp *v);
/* Draw what is left of the frame and wrap the clock round. */
void vdp_frame_end(vdp *v);
/* Bring lines, the command engine and the flags up to this tick. */
void vdp_sync(vdp *v, uint32_t tick);
/* The next tick at which /INT may change by itself (~0u: none). */
uint32_t vdp_next_event(const vdp *v);

/* Ports 98..9B (port & 3) at a tick. */
uint8_t vdp_read(vdp *v, int port, uint32_t tick);
void vdp_write(vdp *v, int port, uint8_t val, uint32_t tick);

static inline int vdp_irq(const vdp *v) { return v->irq_v || v->irq_h; }

#endif
