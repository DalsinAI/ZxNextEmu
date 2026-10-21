/*
 * TMS9918A / TMS9929A / V9938 video (vdp.h). The command engine is in
 * vdpcmd.c.
 *
 * Line and interrupt timing follow openMSX (VDP.cc, measured on real
 * machines): display line 0 starts (8 + vertical adjust + 17 or 44 + 10 for
 * 192 lines) lines into the frame, 202 ticks into its line; F is set at the
 * same point of the first line after the display, and FH at the right
 * border of display line R19 - R23.
 */
#include "vdp.h"
#include "vdpcmd.h"

#include <string.h>

#define NONE 0xffffffffu
#define LEFT_BORDER  202       /* VR flips here; lines are drawn from here */
#define LEFT_SPRITES 258       /* display x 0 with neutral adjust */
#define FB_X0_TICK   194       /* fb pixel 0 (two ticks per fb pixel) */

/* TMS9918 colours as RGB333 (bits 8..0 = RRRGGGBBB). */
static const uint16_t tms_palette[16] = {
    0x000, 0x000, 0x0b2, 0x0f3, 0x097, 0x0df, 0x192, 0x0bf,
    0x1d2, 0x1db, 0x1b2, 0x1b4, 0x0a9, 0x195, 0x1b6, 0x1ff,
};

/* V9938 palette after reset (data book appendix 8), as 0GGG0RRR0BBB. */
static const uint16_t v9938_palette[16] = {
    0x000, 0x000, 0x611, 0x733, 0x117, 0x327, 0x151, 0x627,
    0x171, 0x373, 0x661, 0x664, 0x411, 0x265, 0x555, 0x777,
};

/* Sprite colours in G7, fixed, as 0GGG0RRR0BBB. */
static const uint16_t g7_sprite_palette[16] = {
    0x000, 0x002, 0x030, 0x032, 0x300, 0x302, 0x330, 0x332,
    0x472, 0x007, 0x070, 0x077, 0x700, 0x707, 0x770, 0x777,
};

static uint16_t grb_to_rgb(uint16_t grb)
{
    uint16_t g = (grb >> 8) & 7, r = (grb >> 4) & 7, b = grb & 7;
    return (uint16_t)(r << 6 | g << 3 | b);
}

/* G7 pixel byte GGGRRRBB to RGB333. */
static uint16_t g7_rgb(uint8_t c)
{
    static const uint8_t b3[4] = { 0, 2, 5, 7 };
    return (uint16_t)(((c >> 2) & 7) << 6 | (c >> 5) << 3 | b3[c & 3]);
}

/* ---- modes -------------------------------------------------------------- */

enum {
    M_G1 = 0x00, M_T1 = 0x01, M_MC = 0x02, M_G2 = 0x04, M_G3 = 0x08,
    M_T2 = 0x09, M_G4 = 0x0c, M_G5 = 0x10, M_G6 = 0x14, M_G7 = 0x1c
};

static int is_v9938(const vdp *v) { return v->type == VDP_V9938; }

int vdp_mode(const vdp *v)
{
    int m = (v->reg[1] >> 4 & 1) | (v->reg[1] >> 2 & 2) | (v->reg[0] << 1 & 4);
    if (is_v9938(v))
        m |= (v->reg[0] << 1) & 0x18;
    return m;
}

static int is_text(int mode) { return mode == M_T1 || mode == M_T2; }

int vdp_planar(const vdp *v)
{
    int m = vdp_mode(v);
    return m == M_G6 || m == M_G7;
}

/* Logical VRAM address (as the CPU, commands and display see it) to the
 * byte in vram[]: in G6 and G7 the two 64K banks interleave. */
uint32_t vdp_phys(const vdp *v, uint32_t a)
{
    if (vdp_planar(v))
        a = ((a & 1) << 16) | ((a >> 1) & 0xffff);
    return a & v->vram_mask;
}

static inline uint8_t vr(const vdp *v, uint32_t a)
{
    return v->vram[a & v->vram_mask];
}

/* ---- timing ------------------------------------------------------------- */

static int vert_adjust(const vdp *v)
{
    return is_v9938(v) ? ((v->reg[18] >> 4) ^ 7) : 7;
}

static int horiz_adjust(const vdp *v)
{
    return is_v9938(v) ? ((v->reg[18] & 15) ^ 7) : 7;
}

static int left_sprites(const vdp *v)
{
    int t = LEFT_SPRITES + (horiz_adjust(v) - 7) * 4;
    if (is_text(vdp_mode(v)))
        t += (is_v9938(v) ? 9 : 6) * 4;
    return t;
}

static int right_border(const vdp *v)
{
    return left_sprites(v) + (is_text(vdp_mode(v)) ? 960 : 1024);
}

static int first_visible_line(const vdp *v)
{
    return v->pal ? 45 : 18;
}

static void schedule_vscan(vdp *v)
{
    uint32_t lines = is_v9938(v) && (v->reg[9] & 0x80) ? 212 : 192;
    uint32_t t = (uint32_t)(v->line_zero + (int32_t)lines) * VDP_TPL + LEFT_BORDER;
    v->disp_lines = (uint16_t)lines;
    /* Passed already (or moved behind the beam): no F this frame. */
    v->vscan_tick = t > v->synced && t < v->frame_ticks ? t : NONE;
}

static void schedule_hscan(vdp *v)
{
    uint32_t t;
    if (!is_v9938(v)) {
        v->hscan_tick = NONE;
        return;
    }
    t = (uint32_t)v->line_zero * VDP_TPL + (uint32_t)((v->reg[19] - v->reg[23]) & 0xff) * VDP_TPL
        + (uint32_t)right_border(v);
    v->hscan_tick = t > v->synced && t < v->frame_ticks ? t : NONE;
}

static void update_irq(vdp *v)
{
    v->irq_v = (v->stat[0] & 0x80) && (v->reg[1] & 0x20);
    v->irq_h = is_v9938(v) && (v->stat[1] & 0x01) && (v->reg[0] & 0x10);
}

/* Is the beam inside the display area at this tick (for command timing
 * and the VR / HR flags)? */
int vdp_in_display(const vdp *v, uint32_t tick, int *hblank)
{
    int32_t line = (int32_t)(tick / VDP_TPL) - v->line_zero;
    uint32_t x = tick % VDP_TPL;
    if (hblank)
        *hblank = x < (uint32_t)left_sprites(v) || x >= (uint32_t)right_border(v);
    return line >= 0 && line < v->disp_lines;
}

/* ---- drawing ------------------------------------------------------------ */

static uint16_t colour(const vdp *v, int c)
{
    if (!c && !(is_v9938(v) && (v->reg[8] & 0x20)))
        c = v->reg[7] & 15;
    return v->palette[c];
}

static void border_line(const vdp *v, uint16_t *out)
{
    int i, mode = vdp_mode(v);
    if (mode == M_G7) {
        uint16_t c = g7_rgb(v->reg[7]);
        for (i = 0; i < VDP_FB_W; i++)
            out[i] = c;
    } else if (mode == M_G5) {
        uint16_t a = v->palette[(v->reg[7] >> 2) & 3], b = v->palette[v->reg[7] & 3];
        for (i = 0; i < VDP_FB_W; i += 2) {
            out[i] = a;
            out[i + 1] = b;
        }
    } else {
        uint16_t c = v->palette[v->reg[7] & 15];
        for (i = 0; i < VDP_FB_W; i++)
            out[i] = c;
    }
}

/* Sprites for display line y: spr[x] is the colour (bit 8 set = drawn)
 * of each of the 256 positions. Updates the status registers. */
static void sprites(vdp *v, int y, int mode, uint16_t spr[256])
{
    int mode2 = mode != M_G1 && mode != M_G2 && mode != M_MC;
    int size = (v->reg[1] & 2) ? 16 : 8, mag = v->reg[1] & 1, h = size << mag;
    int max = mode2 ? 8 : 4, stop = mode2 ? 216 : 208;
    uint32_t attr, ctab = 0, pat = (uint32_t)(v->reg[6] & (is_v9938(v) ? 0x3f : 7)) << 11;
    int vis[8], vrow[8], n = 0, i, x, ys;
    uint8_t occ[256];
    int group = -1;

    memset(spr, 0, 256 * sizeof spr[0]);
    if (is_v9938(v) && (v->reg[8] & 2))
        return;
    if (mode2) {
        attr = (((uint32_t)v->reg[11] & 3) << 15 | ((uint32_t)v->reg[5] & 0xfc) << 7) + 0x200;
        ctab = attr - 0x200;
    } else {
        attr = ((uint32_t)v->reg[5] & 0x7f) << 7;
        if (is_v9938(v))
            attr |= ((uint32_t)v->reg[11] & 3) << 15;
    }
    ys = is_v9938(v) ? (y + v->reg[23]) & 0xff : y;

    for (i = 0; i < 32; i++) {
        int sy = vr(v, attr + i * 4u), d;
        if (sy == stop)
            break;
        d = (ys - sy - 1) & 0xff;
        if (d >= h)
            continue;
        if (n == max) {
            if (!(v->stat[0] & 0x40))
                v->stat[0] = (uint8_t)((v->stat[0] & 0xa0) | 0x40 | i);
            break;
        }
        vis[n] = i;
        vrow[n] = d >> mag;
        n++;
    }
    /* Without a fifth (ninth) sprite the number is the last one looked at. */
    if (!(v->stat[0] & 0x40))
        v->stat[0] = (uint8_t)((v->stat[0] & 0xe0) | (i > 31 ? 31 : i));

    memset(occ, 0, sizeof occ);
    for (i = 0; i < n; i++) {
        int s = vis[i], row = vrow[i], cc = 0, ic = 0, ec, c, px;
        uint32_t a = attr + (uint32_t)s * 4;
        int sx = vr(v, a + 1), p = vr(v, a + 2);
        uint16_t bits;
        if (mode2) {
            int cb = vr(v, ctab + (uint32_t)s * 16 + (uint32_t)row);
            ec = cb & 0x80;
            cc = cb & 0x40;
            ic = cb & 0x20;
            c = cb & 15;
            if (cc && group < 0)
                continue;       /* CC with nothing before it: hidden */
            if (!cc)
                group = i;
        } else {
            int cb = vr(v, a + 3);
            ec = cb & 0x80;
            c = cb & 15;
        }
        if (size == 16)
            p &= 0xfc;
        bits = (uint16_t)(vr(v, pat + (uint32_t)p * 8 + (uint32_t)row) << 8);
        if (size == 16)
            bits |= vr(v, pat + (uint32_t)p * 8 + 16 + (uint32_t)row);
        if (ec)
            sx -= 32;
        for (px = 0; px < (size << mag); px++) {
            if (!(bits & (0x8000 >> (px >> mag))))
                continue;
            x = sx + px;
            if (x < 0 || x > 255)
                continue;
            if (!cc && !ic) {
                if (occ[x]) {
                    if (!(v->stat[0] & 0x20) && is_v9938(v)) {
                        v->stat[3] = (uint8_t)(x + 12);
                        v->stat[4] = (uint8_t)((x + 12) >> 8);
                        v->stat[5] = (uint8_t)(ys + 8);
                        v->stat[6] = (uint8_t)((ys + 8) >> 8);
                    }
                    v->stat[0] |= 0x20;
                }
                occ[x] = 1;
            }
            if (spr[x] & 0x100) {
                if (cc && (spr[x] >> 9) == (unsigned)group)
                    spr[x] |= (uint16_t)c;
                continue;
            }
            if (!c && !(mode2 && is_v9938(v) && (v->reg[8] & 0x20)))
                continue;       /* colour 0: transparent */
            spr[x] = (uint16_t)(0x100 | (group < 0 ? 0 : group) << 9 | c);
        }
    }
}

/* Draw one frame line into the picture. */
static void draw_line(vdp *v, uint32_t line)
{
    int r = (int)line - first_visible_line(v);
    int y = (int)line - v->line_zero, mode, x, i, ys, x0;
    uint16_t *out, img[512], spr[256];
    int wide = 0, text_w = 0;

    if (r < 0 || r >= VDP_FB_H) {
        /* Not on the picture; sprites are still checked. */
        if (y >= 0 && y < v->disp_lines && (v->reg[1] & 0x40)) {
            mode = vdp_mode(v);
            if (!is_text(mode))
                sprites(v, y, mode, spr);
        }
        return;
    }
    out = v->fb[r];
    border_line(v, out);
    if (y < 0 || y >= v->disp_lines || !(v->reg[1] & 0x40))
        return;

    mode = vdp_mode(v);
    ys = is_v9938(v) ? (y + v->reg[23]) & 0xff : y;

    switch (mode) {
    case M_G1: {
        uint32_t name = ((uint32_t)v->reg[2] & 0x7f) << 10;
        uint32_t col = ((uint32_t)v->reg[10] & 7) << 14 | (uint32_t)v->reg[3] << 6;
        uint32_t pat = ((uint32_t)v->reg[4] & 0x3f) << 11;
        for (x = 0; x < 32; x++) {
            int n = vr(v, name + (uint32_t)(ys >> 3) * 32 + (uint32_t)x);
            int p = vr(v, pat + (uint32_t)n * 8 + (uint32_t)(ys & 7));
            int c = vr(v, col + (uint32_t)(n >> 3));
            for (i = 0; i < 8; i++)
                img[x * 8 + i] = colour(v, (p & (0x80 >> i)) ? c >> 4 : c & 15);
        }
        break;
    }
    case M_G2:
    case M_G3: {
        uint32_t name = ((uint32_t)v->reg[2] & 0x7f) << 10;
        uint32_t pmask = ((uint32_t)(v->reg[4] & 3) << 11) | 0x7ff;
        uint32_t pbase = ((uint32_t)v->reg[4] & 0x3c) << 11;
        uint32_t cmask = ((uint32_t)(v->reg[3] & 0x7f) << 6) | 0x3f;
        uint32_t cbase = ((uint32_t)v->reg[10] & 7) << 14 | ((uint32_t)v->reg[3] & 0x80) << 6;
        if (!is_v9938(v)) {
            pbase &= 0x2000;
            cbase &= 0x2000;
        }
        for (x = 0; x < 32; x++) {
            uint32_t n = vr(v, name + (uint32_t)(ys >> 3) * 32 + (uint32_t)x);
            uint32_t idx = (uint32_t)(ys >> 6) << 11 | n << 3 | (uint32_t)(ys & 7);
            int p = vr(v, pbase | (idx & pmask));
            int c = vr(v, cbase | (idx & cmask));
            for (i = 0; i < 8; i++)
                img[x * 8 + i] = colour(v, (p & (0x80 >> i)) ? c >> 4 : c & 15);
        }
        break;
    }
    case M_MC: {
        uint32_t name = ((uint32_t)v->reg[2] & 0x7f) << 10;
        uint32_t pat = ((uint32_t)v->reg[4] & 0x3f) << 11;
        for (x = 0; x < 32; x++) {
            int n = vr(v, name + (uint32_t)(ys >> 3) * 32 + (uint32_t)x);
            int p = vr(v, pat + (uint32_t)n * 8 + (uint32_t)((ys >> 3) & 3) * 2 + (uint32_t)((ys >> 2) & 1));
            for (i = 0; i < 8; i++)
                img[x * 8 + i] = colour(v, i < 4 ? p >> 4 : p & 15);
        }
        break;
    }
    case M_T1: {
        uint32_t name = ((uint32_t)v->reg[2] & 0x7f) << 10;
        uint32_t pat = ((uint32_t)v->reg[4] & 0x3f) << 11;
        uint16_t fg = colour(v, v->reg[7] >> 4), bg = colour(v, v->reg[7] & 15);
        for (x = 0; x < 40; x++) {
            int n = vr(v, name + (uint32_t)(ys >> 3) * 40 + (uint32_t)x);
            int p = vr(v, pat + (uint32_t)n * 8 + (uint32_t)(ys & 7));
            for (i = 0; i < 6; i++)
                img[x * 6 + i] = (p & (0x80 >> i)) ? fg : bg;
        }
        text_w = 240;
        break;
    }
    case M_T2: {
        uint32_t name = ((uint32_t)v->reg[2] & 0x7c) << 10;
        uint32_t pat = ((uint32_t)v->reg[4] & 0x3f) << 11;
        uint32_t blink = ((uint32_t)v->reg[10] & 7) << 14 | ((uint32_t)v->reg[3] & 0xf8) << 6;
        uint16_t fg = colour(v, v->reg[7] >> 4), bg = colour(v, v->reg[7] & 15);
        uint16_t bfg = colour(v, v->reg[12] >> 4), bbg = colour(v, v->reg[12] & 15);
        int row = ys >> 3;
        for (x = 0; x < 80; x++) {
            int n = vr(v, name + (uint32_t)row * 80 + (uint32_t)x);
            int p = vr(v, pat + (uint32_t)n * 8 + (uint32_t)(ys & 7));
            int b = v->blink_on && (vr(v, blink + (uint32_t)row * 10 + (uint32_t)(x >> 3)) & (0x80 >> (x & 7)));
            for (i = 0; i < 6; i++)
                img[x * 6 + i] = (p & (0x80 >> i)) ? (b ? bfg : fg) : (b ? bbg : bg);
        }
        text_w = 480;
        wide = 1;
        break;
    }
    case M_G4:
    case M_G5:
    case M_G6:
    case M_G7: {
        int page = v->reg[2];
        uint32_t base;
        if (((v->reg[9] & 0x0c) == 0x0c && !v->field) || (v->reg[13] && !v->blink_on))
            page &= ~0x20;
        if (mode == M_G4 || mode == M_G5) {
            base = ((uint32_t)page & 0x60) << 10 | (uint32_t)ys << 7;
            for (x = 0; x < 128; x++) {
                int b = vr(v, base + (uint32_t)x);
                if (mode == M_G4) {
                    img[x * 2] = colour(v, b >> 4);
                    img[x * 2 + 1] = colour(v, b & 15);
                } else {
                    img[x * 4] = colour(v, b >> 6);
                    img[x * 4 + 1] = colour(v, (b >> 4) & 3);
                    img[x * 4 + 2] = colour(v, (b >> 2) & 3);
                    img[x * 4 + 3] = colour(v, b & 3);
                }
            }
            wide = mode == M_G5;
        } else {
            base = ((uint32_t)page & 0x20) << 11 | (uint32_t)ys << 8;
            for (x = 0; x < 256; x++) {
                uint32_t a = base + (uint32_t)x;
                int b = v->vram[((a & 1) << 16) | (a >> 1 & 0xffff)];
                if (mode == M_G6) {
                    img[x * 2] = colour(v, b >> 4);
                    img[x * 2 + 1] = colour(v, b & 15);
                } else {
                    img[x] = (b || (v->reg[8] & 0x20)) ? g7_rgb((uint8_t)b) : g7_rgb(v->reg[7]);
                }
            }
            wide = mode == M_G6;
        }
        break;
    }
    default:
        /* Mixed and undefined modes: the V9938 shows the backdrop. */
        return;
    }

    if (!is_text(mode)) {
        sprites(v, y, mode, spr);
        for (x = 0; x < 256; x++) {
            uint16_t s = spr[x], c;
            if (!(s & 0x100))
                continue;
            if (mode == M_G7)
                c = grb_to_rgb(g7_sprite_palette[s & 15]);
            else if (mode == M_G5) {
                img[x * 2] = v->palette[(s >> 2) & 3];
                img[x * 2 + 1] = v->palette[s & 3];
                continue;
            } else
                c = v->palette[s & 15];
            if (wide) {
                img[x * 2] = c;
                img[x * 2 + 1] = c;
            } else {
                img[x] = c;
            }
        }
    }

    /* Into the picture: fb x = (tick - 194) / 2. */
    x0 = (left_sprites(v) - FB_X0_TICK) / 2;
    if (text_w) {
        if (wide) {
            for (i = 0; i < text_w; i++)
                if (x0 + i < VDP_FB_W)
                    out[x0 + i] = img[i];
        } else {
            for (i = 0; i < text_w; i++)
                if (x0 + 2 * i + 1 < VDP_FB_W) {
                    out[x0 + 2 * i] = img[i];
                    out[x0 + 2 * i + 1] = img[i];
                }
        }
        return;
    }
    for (i = 0; i < 256; i++) {
        if (x0 + 2 * i + 1 >= VDP_FB_W || x0 + 2 * i < 0)
            continue;
        if (wide) {
            out[x0 + 2 * i] = img[2 * i];
            out[x0 + 2 * i + 1] = img[2 * i + 1];
        } else {
            out[x0 + 2 * i] = img[i];
            out[x0 + 2 * i + 1] = img[i];
        }
    }
}

/* ---- the clock ---------------------------------------------------------- */

void vdp_sync(vdp *v, uint32_t tick)
{
    if (tick > v->frame_ticks)
        tick = v->frame_ticks;
    if (tick < v->synced)
        return;
    while (v->next_line < v->lines) {
        uint32_t at = v->next_line * VDP_TPL + LEFT_BORDER;
        if (at > tick)
            break;
        if (v->cmd.busy)
            vdp_cmd_run(v, at);
        draw_line(v, v->next_line);
        v->next_line++;
    }
    if (v->cmd.busy)
        vdp_cmd_run(v, tick);
    if (v->vscan_tick <= tick) {
        v->stat[0] |= 0x80;
        v->vscan_tick = NONE;
    }
    if (v->hscan_tick <= tick) {
        v->stat[1] |= 0x01;
        v->hscan_tick = NONE;
    }
    v->synced = tick;
    update_irq(v);
}

uint32_t vdp_next_event(const vdp *v)
{
    return v->vscan_tick < v->hscan_tick ? v->vscan_tick : v->hscan_tick;
}

void vdp_frame_start(vdp *v)
{
    int lines192;
    if (is_v9938(v))
        v->pal = (v->reg[9] & 2) != 0;
    v->lines = v->pal ? 313 : 262;
    v->frame_ticks = (uint32_t)v->lines * VDP_TPL;
    lines192 = !(is_v9938(v) && (v->reg[9] & 0x80));
    v->line_zero = 8 + vert_adjust(v) + (v->pal ? 44 : 17) + (lines192 ? 10 : 0);
    v->disp_lines = lines192 ? 192 : 212;
    v->next_line = 0;
    v->synced = 0;
    schedule_vscan(v);
    schedule_hscan(v);
}

void vdp_frame_end(vdp *v)
{
    int on, off;
    vdp_sync(v, v->frame_ticks);
    if (v->cmd.busy)
        v->cmd.time = v->cmd.time > v->frame_ticks ? v->cmd.time - v->frame_ticks : 0;
    /* Blinking (R13, in units of ten frames) and the interlace field. */
    on = v->reg[13] >> 4;
    off = v->reg[13] & 15;
    if (!is_v9938(v) || (!on && !off)) {
        v->blink_on = 0;
        v->blink_count = 0;
    } else if (!off) {
        v->blink_on = 1;
    } else if (!on) {
        v->blink_on = 0;
    } else if (++v->blink_count >= (v->blink_on ? on : off) * 10) {
        v->blink_on ^= 1;
        v->blink_count = 0;
    }
    v->field = (v->reg[9] & 0x08) ? v->field ^ 1 : 0;
}

/* ---- ports -------------------------------------------------------------- */

static void addr_inc(vdp *v)
{
    if (is_v9938(v)) {
        int mode = vdp_mode(v);
        v->addr = (v->addr + 1) & 0x1ffff;
        /* Only the bitmap and 80-column modes carry into R14. */
        if (!(v->addr & 0x3fff) && !(mode >= M_G4 || mode == M_T2))
            v->addr = (v->addr - 0x4000) & 0x1ffff;
        v->reg[14] = (uint8_t)(v->addr >> 14);
    } else {
        v->addr = (v->addr + 1) & 0x3fff;
    }
}

static void write_reg(vdp *v, int r, uint8_t val)
{
    static const uint8_t tms_mask[8] = { 0x03, 0xfb, 0x0f, 0xff, 0x07, 0x7f, 0x07, 0xff };
    if (!is_v9938(v)) {
        r &= 7;
        val &= tms_mask[r];
    } else if (r >= 47 || (r >= 24 && r < 32)) {
        return;
    }
    v->reg[r] = val;
    if (r >= 32) {
        vdp_cmd_write(v, r, val);
        return;
    }
    switch (r) {
    case 0:
    case 1:
        update_irq(v);
        break;
    case 9:
    case 18:
    case 19:
    case 23:
        schedule_vscan(v);
        schedule_hscan(v);
        break;
    case 14:
        v->addr = ((uint32_t)(val & 7) << 14) | (v->addr & 0x3fff);
        break;
    case 16:
        v->pal_full = 0;
        break;
    }
}

uint8_t vdp_read(vdp *v, int port, uint32_t tick)
{
    uint8_t val;
    vdp_sync(v, tick);
    v->latch_full = 0;
    if ((port & 1) == 0) {
        val = v->read_ahead;
        v->read_ahead = v->vram[vdp_phys(v, v->addr)];
        addr_inc(v);
        return val;
    }
    if (!is_v9938(v)) {
        val = v->stat[0];
        v->stat[0] &= 0x1f;
        update_irq(v);
        return val;
    }
    switch (v->reg[15] & 15) {
    case 0:
        val = v->stat[0];
        v->stat[0] &= 0x1f;
        break;
    case 1:
        val = v->stat[1] & 0x01;   /* V9938: ID 0 */
        v->stat[1] &= ~0x01;
        break;
    case 2: {
        int hb, disp = vdp_in_display(v, tick, &hb);
        val = (uint8_t)(0x0c | (v->stat[2] & 0x91) | (disp ? 0 : 0x40) | (hb ? 0x20 : 0)
                        | (v->field ? 2 : 0));
        break;
    }
    case 5:
        val = v->stat[5];
        v->stat[3] = v->stat[4] = v->stat[5] = v->stat[6] = 0;
        break;
    case 7:
        val = v->stat[7];
        vdp_cmd_read_s7(v);
        break;
    case 9:
        val = (uint8_t)(v->stat[9] | 0xfe);
        break;
    default:
        val = v->stat[v->reg[15] & 15] ;
        if ((v->reg[15] & 15) > 9)
            val = 0xff;
        break;
    }
    update_irq(v);
    return val;
}

void vdp_write(vdp *v, int port, uint8_t val, uint32_t tick)
{
    vdp_sync(v, tick);
    if (!is_v9938(v))
        port &= 1;
    switch (port & 3) {
    case 0:
        v->latch_full = 0;
        v->vram[vdp_phys(v, v->addr)] = val;
        v->read_ahead = val;
        addr_inc(v);
        break;
    case 1:
        if (!v->latch_full) {
            v->latch = val;
            v->latch_full = 1;
            if (!is_v9938(v))
                v->addr = (v->addr & 0x3f00) | val;
            break;
        }
        v->latch_full = 0;
        if (val & 0x80) {
            write_reg(v, val & 0x3f, v->latch);
        } else {
            v->addr = ((uint32_t)(val & 0x3f) << 8) | v->latch;
            if (is_v9938(v))
                v->addr |= (uint32_t)(v->reg[14] & 7) << 14;
            if (!(val & 0x40)) {
                v->read_ahead = v->vram[vdp_phys(v, v->addr)];
                addr_inc(v);
            }
        }
        break;
    case 2:
        if (!v->pal_full) {
            v->pal_latch = val;
            v->pal_full = 1;
        } else {
            int i = v->reg[16] & 15;
            v->pal_raw[i] = (uint16_t)((val & 7) << 8 | (v->pal_latch & 0x70) | (v->pal_latch & 7));
            v->palette[i] = grb_to_rgb(v->pal_raw[i]);
            v->reg[16] = (uint8_t)((i + 1) & 15);
            v->pal_full = 0;
        }
        break;
    case 3: {
        int r = v->reg[17] & 0x3f;
        if (r != 17)
            write_reg(v, r, val);
        if (!(v->reg[17] & 0x80))
            v->reg[17] = (uint8_t)((v->reg[17] & 0xc0) | ((r + 1) & 0x3f));
        break;
    }
    }
}

/* ---- setup -------------------------------------------------------------- */

void vdp_init(vdp *v, vdp_type type, uint8_t *vram)
{
    memset(v, 0, sizeof *v);
    v->type = type;
    v->vram = vram;
    v->vram_mask = type == VDP_V9938 ? 0x1ffff : 0x3fff;
    vdp_reset(v);
}

void vdp_reset(vdp *v)
{
    int i;
    memset(v->reg, 0, sizeof v->reg);
    memset(v->stat, 0, sizeof v->stat);
    memset(&v->cmd, 0, sizeof v->cmd);
    v->latch_full = v->pal_full = 0;
    v->addr = 0;
    v->read_ahead = 0;
    v->pal = v->type == VDP_TMS9929;
    v->blink_on = 0;
    v->blink_count = 0;
    v->field = 0;
    for (i = 0; i < 16; i++) {
        v->pal_raw[i] = v9938_palette[i];
        v->palette[i] = is_v9938(v) ? grb_to_rgb(v9938_palette[i]) : tms_palette[i];
    }
    v->stat[2] = 0x0c;
    update_irq(v);
    vdp_frame_start(v);
}
