/*
 * ZX Spectrum Next video: ULA (with Timex, ULAnext and ULA+), LoRes,
 * tilemap, Layer 2, sprites and the layer mixer.
 *
 * Rendering is lazy: the picture is drawn up to "now" whenever something
 * that changes it is about to be written, so mid-frame changes (raster
 * effects, the copper, border stripes) land on the right pixel. Each 7 MHz
 * pixel of the 320x256 window is two 14 MHz subpixels in the framebuffer.
 *
 * The functional behaviour follows video/zxula.vhd, lores.vhd, tilemap.vhd,
 * layer2.vhd, sprites.vhd and the mixer at the end of zxnext.vhd; the
 * FPGA's pipeline delays are folded out, so all layers line up on the
 * window coordinate.
 */
#include "next.h"
#include "../zx/zx.h"

#include <string.h>

/* ---- helpers -------------------------------------------------------------- */

static inline uint16_t rgb8to9(uint8_t c)
{
    return (uint16_t)(c << 1 | ((c & 3) ? 1 : 0));
}

static inline uint8_t vram(const zxn *n, int bank7, uint32_t a)
{
    return bank7 ? n->bank7[a & 0x1fff] : n->bank5[a & 0x3fff];
}

/* ---- sprites (video/sprites.vhd) ----------------------------------------- */

static void sprite_line(zxn *n, int y)
{
    int i;
    /* anchor state */
    int a_rel_type = 0, a_h = 0, a_vis = 0, a_x = 0, a_y = 0, a_pattern = 0, a_paloff = 0;
    int a_rot = 0, a_xm = 0, a_ym = 0, a_xs = 0, a_ys = 0;
    int zero_on_top = (n->nr_15 >> 6) & 1;
    uint8_t transp = n->sprite_transp;

    memset(n->spr_line, 0, sizeof n->spr_line);
    n->spr_line_y = y;
    if (!(n->nr_15 & 1))
        return;

    for (i = 0; i < NX_SPRITES; i++) {
        const uint8_t *s = n->spr_attr[i];
        int ext = (s[3] >> 6) & 1;
        int relative = ext && (s[4] >> 6) == 1;
        uint8_t c0, c1, c2, c3, c4;
        int cx, cy, h, n6, pattern, yoff, yidx, xmirr, rot, xs, ys, paloff, k, w;
        int fourbit, addr, delta;

        if (relative) {
            int rx = a_rot ? s[1] : s[0], ry = a_rot ? s[0] : s[1];
            int x1, y1, x2, y2, x3, y3, rel_pal, rel_xm, rel_ym;
            x1 = (a_rot ^ a_xm) ? (uint8_t)(-rx) : rx;
            y1 = a_ym ? (uint8_t)(-ry) : ry;
            x1 = (int8_t)x1;
            y1 = (int8_t)y1;
            x2 = x1 * (1 << a_xs);
            y2 = y1 * (1 << a_ys);
            x3 = (a_x + x2) & 0x1ff;
            y3 = (a_y + y2) & 0x1ff;
            rel_pal = (s[2] & 1) ? ((a_paloff + (s[2] >> 4)) & 15) : (s[2] >> 4);
            rel_xm = a_rot ? (((s[2] >> 2) ^ (s[2] >> 1)) & 1) : ((s[2] >> 3) & 1);
            rel_ym = a_rot ? (((s[2] >> 3) ^ (s[2] >> 1)) & 1) : ((s[2] >> 2) & 1);
            c0 = (uint8_t)x3;
            c1 = (uint8_t)y3;
            if (!a_rel_type)
                c2 = (uint8_t)(rel_pal << 4 | (s[2] & 0x0e) | (x3 >> 8));
            else
                c2 = (uint8_t)(rel_pal << 4 | (a_xm ^ rel_xm) << 3 | (a_ym ^ rel_ym) << 2 |
                               (a_rot ^ ((s[2] >> 1) & 1)) << 1 | (x3 >> 8));
            c3 = (uint8_t)((a_vis & (s[3] >> 7)) << 7 | 0x40 | (s[3] & 0x3f));
            if (!a_rel_type)
                c4 = (uint8_t)(a_h << 7 | ((s[4] >> 5) & 1) << 6 | (s[4] & 0x1e) | (y3 >> 8));
            else
                c4 = (uint8_t)(a_h << 7 | ((s[4] >> 5) & 1) << 6 | a_xs << 3 | a_ys << 1 | (y3 >> 8));
        } else {
            c0 = s[0]; c1 = s[1]; c2 = s[2]; c3 = s[3]; c4 = s[4];
        }

        cy = (ext ? (c4 & 1) : 0) << 8 | c1;
        cx = (c2 & 1) << 8 | c0;
        h = (c4 >> 7) & ext;
        n6 = (c4 >> 6) & h;
        pattern = (s[3] & 0x3f) << 1 | n6;
        if (relative && (s[4] & 1))
            pattern = (pattern + a_pattern) & 0x7f;

        if (!relative) {
            a_rel_type = (s[4] >> 5) & ext;
            a_h = (s[4] >> 7) & ext;
            a_vis = s[3] >> 7;
            a_x = cx;
            a_y = cy;
            a_pattern = pattern;
            a_paloff = s[2] >> 4;
            if (ext && (s[4] & 0x20)) {
                a_rot = (s[2] >> 1) & 1;
                a_xm = (s[2] >> 3) & 1;
                a_ym = (s[2] >> 2) & 1;
                a_xs = (s[4] >> 3) & 3;
                a_ys = (s[4] >> 1) & 3;
            } else {
                a_rot = a_xm = a_ym = a_xs = a_ys = 0;
            }
        }

        if (!(c3 & 0x80))
            continue;
        xs = ext ? (c4 >> 3) & 3 : 0;
        ys = ext ? (c4 >> 1) & 3 : 0;
        yoff = (y - cy) & 0x1ff;
        if (yoff & 0x100)
            yoff |= ~0x1ff;     /* sign extend before scaling */
        yoff >>= ys;
        if (yoff < 0 || yoff > 15)
            continue;
        yidx = (c2 & 4) ? 15 - yoff : yoff;
        rot = (c2 >> 1) & 1;
        xmirr = ((c2 >> 3) ^ rot) & 1;
        paloff = c2 >> 4;
        fourbit = h;

        addr = (pattern >> 1) << 8;
        addr |= rot ? ((xmirr ? 15 : 0) << 4 | yidx) : (yidx << 4 | (xmirr ? 15 : 0));
        delta = rot ? (xmirr ? -16 : 16) : (xmirr ? -1 : 1);

        w = 16 << xs;
        for (k = 0; k < w; k++) {
            int x = (cx + k) & 0x1ff;
            int a = addr & 0x3fff, pix, opaque;
            if (x < 320) {
                if (!fourbit) {
                    uint8_t d = n->spr_pattern[a];
                    opaque = d != transp;
                    pix = (((d >> 4) + paloff) & 15) << 4 | (d & 15);
                } else {
                    uint8_t d = n->spr_pattern[((a >> 8) << 8 | (pattern & 1) << 7 | (a & 0xff) >> 1) & 0x3fff];
                    uint8_t nib = (a & 1) ? (d & 15) : (d >> 4);
                    opaque = nib != (transp & 15);
                    pix = paloff << 4 | nib;
                }
                if (opaque) {
                    if (n->spr_line[x] & 0x100)
                        n->spr_status |= 1;
                    if (!zero_on_top || !(n->spr_line[x] & 0x100))
                        n->spr_line[x] = (uint16_t)(0x100 | pix);
                }
            }
            /* step the pattern address once per (scaled) source pixel */
            if (((k + 1) & ((1 << xs) - 1)) == 0)
                addr += delta;
        }
    }
}

void nx_sprite_port_57(zxn *n, uint8_t v)
{
    int b = n->spr_attr_byte;
    n->spr_attr[n->spr_attr_index & 0x7f][b] = v;
    if (b == 4 || (b == 3 && !(v & 0x40))) {
        n->spr_attr_index = (n->spr_attr_index + 1) & 0x7f;
        n->spr_attr_byte = 0;
        if (n->nr_09 & 0x10)
            n->spr_mirror_sprite = (uint8_t)((n->spr_pattern_index >> 7 & 1) << 7 | n->spr_attr_index);
    } else {
        n->spr_attr_byte++;
    }
}

void nx_sprite_port_5b(zxn *n, uint8_t v)
{
    n->spr_pattern[n->spr_pattern_index & 0x3fff] = v;
    n->spr_pattern_index = (n->spr_pattern_index + 1) & 0x3fff;
}

void nx_sprite_port_303b(zxn *n, uint8_t v)
{
    n->spr_attr_index = v & 0x7f;
    n->spr_attr_byte = 0;
    n->spr_pattern_index = (uint16_t)((v & 0x3f) << 8 | (v & 0x80));
    if (n->nr_09 & 0x10)
        n->spr_mirror_sprite = (uint8_t)((v & 0x80) | (v & 0x7f));
}

void nx_sprite_mirror(zxn *n, uint8_t reg, uint8_t v)
{
    int tie = (n->nr_09 >> 4) & 1;
    if (reg == 0x34) {
        n->spr_mirror_sprite = v;
    } else {
        int idx = (reg & 0x0f) - 5;
        n->spr_attr[n->spr_mirror_sprite & 0x7f][idx] = v;
        if (!(reg & 0x40))
            return;
        n->spr_mirror_sprite = (uint8_t)((n->spr_pattern_index >> 7 & 1) << 7 |
                                         ((n->spr_mirror_sprite + 1) & 0x7f));
    }
    if (tie) {
        n->spr_attr_index = n->spr_mirror_sprite & 0x7f;
        n->spr_attr_byte = 0;
        n->spr_pattern_index = (uint16_t)((n->spr_mirror_sprite & 0x3f) << 8 | (n->spr_mirror_sprite & 0x80));
    }
}

/* ---- per-pixel layers ----------------------------------------------------- */

typedef struct layer_px {
    uint16_t rgb;       /* 9-bit RGB */
    uint8_t transparent;
} layer_px;

/* ULA (or LoRes) at window (wx, wy), subpixel s. Returns the RGB, sets
 * *transp (the mix transparency: global key colour or clip) and *border. */
static uint16_t ula_pixel(const zxn *n, int wx, int wy, int s, int *mix_transp, int *border)
{
    int phc = wx - 32, vc = wy - 32;
    int is_border = phc < 0 || phc > 255 || vc < 0 || vc > 191;
    int ulanext = n->nr_43 & 1, ulap = n->ulap_en;
    int shadow = (n->port_7ffd >> 3) & 1;
    int mode = shadow ? 0 : (n->port_ff & 7);
    uint8_t attr = 0, pbits = 0;
    int pixel_en = 0, bgnd = 0, idx, clipped = 0, lores = 0;
    uint16_t rgb;
    const uint8_t *clip = n->clip[2];

    *border = is_border;

    /* LoRes replaces the ULA inside the ULA clip window */
    if ((n->nr_15 & 0x80) && phc >= clip[0] && phc <= clip[1] && vc >= clip[2] && vc <= clip[3] &&
        phc >= 0 && vc >= 0) {
        int x = (phc + n->lores_scroll_x) & 255;
        int y = vc + n->lores_scroll_y;
        uint8_t d;
        if (y >= 192)
            y -= 192;
        if (n->nr_6a & 0x20) {
            int dfile = (n->port_ff & 1) ^ ((n->nr_6a >> 4) & 1);
            int hi = ulap && !ulanext ? (0x0c | (n->nr_6a & 3)) : (n->nr_6a & 15);
            d = n->bank5[(dfile << 13 | (y >> 1) << 6 | x >> 2) & 0x3fff];
            idx = hi << 4 | ((x & 2) ? (d & 15) : (d >> 4));
        } else {
            uint32_t a = (uint32_t)((y >> 1) << 7 | x >> 1);
            if (y >= 96)
                a += 0x800;
            d = n->bank5[a & 0x3fff];
            idx = (((d >> 4) + n->nr_6a) & 15) << 4 | (d & 15);
        }
        lores = 1;
        rgb = n->pal_ulatm[((n->nr_43 >> 1) & 1) << 8 | idx] & 0x1ff;
        *mix_transp = (rgb >> 1) == n->transparent;
        return rgb;
    }

    if (mode & 4) {
        uint8_t i = (n->port_ff >> 3) & 7;
        attr = (uint8_t)(0x40 | (~i & 7) << 3 | i);
    } else if (is_border) {
        attr = (uint8_t)((n->port_fe & 7) << 3 | (n->port_fe & 7));
    }

    if (!is_border) {
        int src = (2 * phc + s + 2 * n->ula_scroll_x + ((n->nr_68 >> 2) & 1)) & 511;
        int col = src >> 4;
        int y = vc + n->ula_scroll_y;
        uint32_t pa;
        if (y >= 192)
            y -= 192;
        pa = (uint32_t)((y & 0xc0) << 5 | (y & 7) << 8 | (y & 0x38) << 2 | col);
        if (mode & 4) {
            uint8_t b0 = vram(n, shadow, pa), b1 = vram(n, shadow, 0x2000 | pa);
            pbits = (uint8_t)(((src & 8) ? b1 : b0) << (src & 7));
        } else {
            uint8_t b = vram(n, shadow, ((mode & 1) ? 0x2000 : 0) | pa);
            pbits = (uint8_t)(b << ((src >> 1) & 7));
            if (mode & 2)
                attr = vram(n, shadow, 0x2000 | pa);
            else
                attr = vram(n, shadow, ((mode & 1) ? 0x2000 : 0) | 0x1800 | (y >> 3) << 5 | col);
        }
        pixel_en = (pbits >> 7) & 1;
        if ((attr & 0x80) && (n->flash & 16) && !ulanext && !ulap)
            pixel_en ^= 1;
        clipped = !(phc >= clip[0] && phc <= clip[1] && vc >= clip[2] && vc <= clip[3]);
    }

    if (ulanext) {
        uint8_t f = n->ulanext_format;
        if (is_border) {
            if (f == 0xff)
                bgnd = 1;
            idx = 0x80 | ((attr >> 3) & 7);
        } else if (pixel_en) {
            idx = attr & f;
        } else {
            switch (f) {
            case 0x01: idx = 0x80 | attr >> 1; break;
            case 0x03: idx = 0x80 | attr >> 2; break;
            case 0x07: idx = 0x80 | attr >> 3; break;
            case 0x0f: idx = 0x80 | attr >> 4; break;
            case 0x1f: idx = 0x80 | attr >> 5; break;
            case 0x3f: idx = 0x80 | attr >> 6; break;
            case 0x7f: idx = 0x80 | attr >> 7; break;
            default:   idx = 0; bgnd = 1; break;
            }
        }
    } else if (ulap) {
        idx = 0xc0 | (attr >> 6) << 4 | (((mode >> 2) | !pixel_en) & 1) << 3 |
              (pixel_en ? (attr & 7) : ((attr >> 3) & 7));
    } else {
        idx = (!pixel_en) << 4 | ((attr >> 6) & 1) << 3 | (pixel_en ? (attr & 7) : ((attr >> 3) & 7));
    }

    (void)lores;
    if (bgnd)
        rgb = rgb8to9(n->fallback);
    else
        rgb = n->pal_ulatm[((n->nr_43 >> 1) & 1) << 8 | idx] & 0x1ff;
    *mix_transp = clipped || (rgb >> 1) == n->transparent;
    return rgb;
}

/* Tilemap at window (wx, wy) subpixel s. */
static void tm_pixel(const zxn *n, int wx, int wy, int s, int *en, uint16_t *rgb, int *below, int *textmode)
{
    uint8_t ctl = n->nr_6b;
    const uint8_t *clip = n->clip[3];
    int mode80 = (ctl >> 6) & 1, strip = (ctl >> 5) & 1, text = (ctl >> 3) & 1;
    int m512 = (ctl >> 1) & 1, on_top = ctl & 1;
    int tx, ty, col, row, w, tile, attr, tile9, pix, opaque;
    int mbank7 = n->nr_6e >> 7, tbank7 = n->nr_6f >> 7;
    uint32_t mbase = (uint32_t)(n->nr_6e & 0x3f) << 8, tbase = (uint32_t)(n->nr_6f & 0x3f) << 8, off;

    *en = 0;
    *textmode = text;
    if (wx < clip[0] * 2 || wx > clip[1] * 2 + 1 || wy < clip[2] || wy > clip[3]) {
        *below = 0;
        return;
    }
    if (mode80)
        tx = (2 * wx + s + n->tm_scroll_x) % 640;
    else
        tx = (wx + n->tm_scroll_x) % 320;
    ty = (wy + n->tm_scroll_y) & 255;
    w = mode80 ? 80 : 40;
    col = tx >> 3;
    row = ty >> 3;
    off = (uint32_t)(row * w + col) * (strip ? 1 : 2);
    tile = vram(n, mbank7, (mbase + off) & 0x3fff);
    attr = strip ? n->nr_6c : vram(n, mbank7, (mbase + off + 1) & 0x3fff);
    tile9 = (m512 && (attr & 1)) << 8 | tile;
    *below = ((attr & 1) || m512) && !on_top;

    if (text) {
        uint8_t b = vram(n, tbank7, (tbase + (uint32_t)tile9 * 8 + (ty & 7)) & 0x3fff);
        pix = (attr & 0xfe) | ((b << (tx & 7)) >> 7 & 1);
        opaque = 1;
    } else {
        int ex = tx & 7, ey = ty & 7, rot = (attr >> 1) & 1, ttx, tty;
        uint8_t b;
        if (((attr >> 3) ^ rot) & 1)
            ex = 7 - ex;
        if (attr & 4)
            ey = 7 - ey;
        ttx = rot ? ey : ex;
        tty = rot ? ex : ey;
        b = vram(n, tbank7, (tbase + (uint32_t)tile9 * 32 + (uint32_t)(tty * 4 + (ttx >> 1))) & 0x3fff);
        b = (ttx & 1) ? (b & 15) : (b >> 4);
        pix = (attr & 0xf0) | b;
        opaque = b != n->tm_transp;
    }
    *rgb = n->pal_ulatm[0x200 | ((ctl >> 4) & 1) << 8 | pix] & 0x1ff;
    *en = opaque;
}

/* Layer 2 at window (wx, wy) subpixel s. */
static int l2_pixel(const zxn *n, int wx, int wy, int s, uint16_t *rgb, int *prio)
{
    int res = (n->nr_70 >> 4) & 3;
    const uint8_t *clip = n->clip[0];
    uint32_t addr, a;
    int x, y, X, Y, pix;
    uint16_t e;

    if (!n->l2_en)
        return 0;
    if (res == 0) {
        x = wx - 32;
        y = wy - 32;
        if (x < 0 || x > 255 || y < 0 || y > 191)
            return 0;
        if (x < clip[0] || x > clip[1] || y < clip[2] || y > clip[3])
            return 0;
        X = (x + (n->l2_scroll_x_msb << 8 | n->l2_scroll_x)) & 255;
        Y = y + n->l2_scroll_y;
        if (Y >= 192)
            Y = (Y - 192) & 255;
        addr = (uint32_t)(Y << 8 | X);
    } else {
        int xp;
        x = wx;
        y = wy;
        if (x < clip[0] * 2 || x > clip[1] * 2 + 1 || y < clip[2] || y > clip[3])
            return 0;
        xp = x + (n->l2_scroll_x_msb << 8 | n->l2_scroll_x);
        X = xp < 320 ? xp : (((xp & 511) + 192) & 511);
        Y = (y + n->l2_scroll_y) & 255;
        addr = (uint32_t)(X << 8 | Y);
    }
    a = (((uint32_t)n->l2_bank + 16 + (addr >> 14)) << 14) | (addr & 0x3fff);
    if (a >= NX_SRAM_SIZE)
        return 0;
    pix = n->sram[a];
    if (res == 2)
        pix = (n->nr_70 & 15) << 4 | (s ? (pix & 15) : (pix >> 4));
    else
        pix = (((pix >> 4) + n->nr_70) & 15) << 4 | (pix & 15);
    e = n->pal_l2s[((n->nr_43 >> 2) & 1) << 8 | pix];
    *rgb = e & 0x1ff;
    *prio = (e >> 10) & 1;
    return (*rgb >> 1) != n->transparent;
}

static int sprite_clip(const zxn *n, int wx, int wy)
{
    const uint8_t *c = n->clip[1];
    if (n->nr_15 & 2) {
        if (!(n->nr_15 & 0x20))
            return 1;
        return wx >= c[0] * 2 && wx <= c[1] * 2 + 1 && wy >= c[2] && wy <= c[3];
    }
    return wx >= c[0] + 32 && wx <= c[1] + 32 && wy >= c[2] + 32 && wy <= c[3] + 32 && wy < 224;
}

/* ---- mixer ----------------------------------------------------------------- */

static uint16_t add_rgb(uint16_t a, uint16_t b, int minus5)
{
    int r = (a >> 6) + (b >> 6), g = (a >> 3 & 7) + (b >> 3 & 7), bl = (a & 7) + (b & 7);
    if (minus5) {
        r = r <= 4 ? 0 : r >= 12 ? 7 : r - 5;
        g = g <= 4 ? 0 : g >= 12 ? 7 : g - 5;
        bl = bl <= 4 ? 0 : bl >= 12 ? 7 : bl - 5;
    } else {
        if (r > 7) r = 7;
        if (g > 7) g = 7;
        if (bl > 7) bl = 7;
    }
    return (uint16_t)(r << 6 | g << 3 | bl);
}

static uint16_t mix(const zxn *n, int wx, int wy, int s, int spr_pix)
{
    int ula_mix_t, border, ula_en = !(n->nr_68 & 0x80);
    int tm_en_reg = n->nr_6b >> 7, tm_en = 0, tm_below = 0, tm_text = 0;
    uint16_t ula_rgb, tm_rgb = 0, l2_rgb = 0, spr_rgb = 0;
    int ula_t, tm_t, l2_t, spr_t, l2_prio = 0;
    int ulatm_t, ula_final_t;
    uint16_t ula_final_rgb, ulatm_rgb;
    uint16_t mix_rgb, top_rgb, bot_rgb;
    int mix_t, top_t, bot_t;
    uint16_t fb = rgb8to9(n->fallback);
    int blend = (n->nr_68 >> 5) & 3;

    ula_rgb = ula_pixel(n, wx, wy, s, &ula_mix_t, &border);
    ula_t = ula_mix_t || !ula_en;
    if (ula_mix_t)
        ula_rgb = 0;
    if (tm_en_reg) {
        tm_pixel(n, wx, wy, s, &tm_en, &tm_rgb, &tm_below, &tm_text);
    } else {
        tm_below = !(n->nr_6b & 1);
    }
    tm_t = !tm_en || !tm_en_reg || (tm_text && (tm_rgb >> 1) == n->transparent);
    if (tm_t)
        tm_rgb = 0;

    l2_t = !l2_pixel(n, wx, wy, s, &l2_rgb, &l2_prio);
    if (l2_t) {
        l2_rgb = 0;
        l2_prio = 0;
    }
    spr_t = !(spr_pix & 0x100) || !sprite_clip(n, wx, wy);
    if (!spr_t)
        spr_rgb = n->pal_l2s[0x200 | ((n->nr_43 >> 3) & 1) << 8 | (spr_pix & 0xff)] & 0x1ff;

    /* ULA and tilemap combine */
    {
        uint16_t u = ula_t ? 0 : ula_rgb;
        int stencil_t = ula_t || tm_t;
        ulatm_t = ula_t && tm_t;
        ulatm_rgb = (!tm_t && (!tm_below || ula_t)) ? tm_rgb : u;
        if ((n->nr_68 & 1) && ula_en && tm_en_reg) {
            ula_final_rgb = stencil_t ? 0 : (uint16_t)(u & tm_rgb);
            ula_final_t = stencil_t;
        } else {
            ula_final_rgb = ulatm_rgb;
            ula_final_t = ulatm_t;
        }
        switch (blend) {
        case 0:
            mix_rgb = ula_mix_t ? 0 : ula_rgb; mix_t = ula_mix_t;
            top_t = tm_t || tm_below; top_rgb = tm_rgb;
            bot_t = tm_t || !tm_below; bot_rgb = tm_rgb;
            break;
        case 2:
            mix_rgb = ula_final_rgb; mix_t = ula_final_t;
            top_t = 1; top_rgb = tm_rgb;
            bot_t = 1; bot_rgb = tm_rgb;
            break;
        case 3:
            mix_rgb = tm_rgb; mix_t = tm_t;
            top_t = ula_t || !tm_below; top_rgb = u;
            bot_t = ula_t || tm_below; bot_rgb = u;
            break;
        default:
            mix_rgb = 0; mix_t = 1;
            if (tm_below) {
                top_t = ula_t; top_rgb = u;
                bot_t = tm_t; bot_rgb = tm_rgb;
            } else {
                top_t = tm_t; top_rgb = tm_rgb;
                bot_t = ula_t; bot_rgb = u;
            }
            break;
        }
    }

    switch ((n->nr_15 >> 2) & 7) {
    case 0: /* SLU */
        if (l2_prio) return l2_rgb;
        if (!spr_t) return spr_rgb;
        if (!l2_t) return l2_rgb;
        if (!ula_final_t) return ula_final_rgb;
        return fb;
    case 1: /* LSU */
        if (!l2_t) return l2_rgb;
        if (!spr_t) return spr_rgb;
        if (!ula_final_t) return ula_final_rgb;
        return fb;
    case 2: /* SUL */
        if (l2_prio) return l2_rgb;
        if (!spr_t) return spr_rgb;
        if (!ula_final_t) return ula_final_rgb;
        if (!l2_t) return l2_rgb;
        return fb;
    case 3: /* LUS */
        if (!l2_t) return l2_rgb;
        if (!ula_final_t && !(border && tm_t && !spr_t)) return ula_final_rgb;
        if (!spr_t) return spr_rgb;
        return fb;
    case 4: /* USL */
        if (l2_prio) return l2_rgb;
        if (!ula_final_t && !(border && tm_t && !spr_t)) return ula_final_rgb;
        if (!spr_t) return spr_rgb;
        if (!l2_t) return l2_rgb;
        return fb;
    case 5: /* ULS */
        if (l2_prio) return l2_rgb;
        if (!ula_final_t && !(border && tm_t && !spr_t)) return ula_final_rgb;
        if (!l2_t) return l2_rgb;
        if (!spr_t) return spr_rgb;
        return fb;
    case 6: { /* (U|T)S(T|U)(B+L) */
        uint16_t sum = add_rgb(l2_rgb, mix_rgb, 0);
        if (l2_prio) return sum;
        if (!top_t) return top_rgb;
        if (!spr_t) return spr_rgb;
        if (!bot_t) return bot_rgb;
        if (!l2_t) return sum;
        return fb;
    }
    default: { /* (U|T)S(T|U)(B+L-5) */
        uint16_t sum = mix_t ? add_rgb(l2_rgb, mix_rgb, 0) : add_rgb(l2_rgb, mix_rgb, 1);
        if (l2_prio) return sum;
        if (!top_t) return top_rgb;
        if (!spr_t) return spr_rgb;
        if (!bot_t) return bot_rgb;
        if (!l2_t) return sum;
        return fb;
    }
    }
}

/* ---- the lazy renderer ---------------------------------------------------- */

static void draw_span(zxn *n, int wy, int xa, int xb)
{
    uint16_t *row = n->screen[wy];
    int x;
    if (n->spr_line_y != wy)
        sprite_line(n, wy);
    for (x = xa; x < xb; x++) {
        int sp = n->spr_line[x];
        row[2 * x] = mix(n, x, wy, 0, sp);
        row[2 * x + 1] = mix(n, x, wy, 1, sp);
    }
}

void nx_video_run(zxn *n, uint64_t upto_clk)
{
    const nx_timing *t = &n->tm;
    int64_t total = (int64_t)t->hc * t->vc;
    int64_t base = (int64_t)t->int_v * t->hc + t->int_h + 1;
    int64_t target, p;

    if (upto_clk <= n->frame_start)
        return;
    target = (int64_t)((upto_clk - n->frame_start) >> 2);
    if (target > total)
        target = total;
    p = (int64_t)n->video_pix;
    while (p < target) {
        int64_t raw = (base + p) % total;
        int vc = (int)(raw / t->hc), h = (int)(raw % t->hc);
        int64_t run = t->hc - h;
        int wy = vc - t->min_vactive + 32;
        if (run > target - p)
            run = target - p;
        if (wy >= 0 && wy < NX_H) {
            int x0 = h - (t->min_hactive + 1) + 32, x1 = x0 + (int)run;
            if (x0 < 0) x0 = 0;
            if (x1 > 320) x1 = 320;
            if (x0 < x1)
                draw_span(n, wy, x0, x1);
        }
        p += run;
    }
    n->video_pix = (uint64_t)p;
}

void nx_video_frame_end(zxn *n)
{
    n->video_pix = 0;
    n->flash = (n->flash + 1) & 31;
    n->spr_line_y = -1;
}

void nx_video_reset(zxn *n)
{
    n->video_pix = 0;
    n->spr_attr_index = 0;
    n->spr_attr_byte = 0;
    n->spr_pattern_index = 0;
    n->spr_mirror_sprite = 0;
    n->spr_status = 0;
    n->spr_line_y = -1;
}

/* Byte the ULA is reading now (port 0xFF on 48K/128K timing), else 0xFF.
 * Same fetch pattern as the classic machines, on the Next's frame. */
uint8_t nx_floating_bus(zxn *n)
{
    static const uint32_t paper_t[3] = { 14336, 14364, 14364 };
    int kind = (n->eff_timing & 4) ? 3 : (n->eff_timing & 3) == 2 ? 1 : (n->eff_timing & 3) == 3 ? 2 : 0;
    uint32_t line_t = kind == 0 ? 224 : 228;
    int64_t t, p0, d;
    uint32_t line, x, col;
    int shadow = (n->port_7ffd >> 3) & 1;
    if (kind == 3)
        return 0xff;
    t = (int64_t)((n->clk - n->frame_start) >> 3) - 1;
    p0 = (int64_t)paper_t[kind] - (int64_t)(64 - n->tm.min_vactive) * line_t;
    if (t < p0)
        return 0xff;
    d = t - p0;
    line = (uint32_t)(d / line_t);
    x = (uint32_t)(d % line_t);
    if (line >= 192 || x >= 128)
        return 0xff;
    col = (x / 8) * 2;
    switch (x & 7) {
    case 2: col++; /* fall through */
    case 0: return vram(n, shadow, ((line & 0xc0) << 5) | ((line & 7) << 8) | ((line & 0x38) << 2) | col);
    case 3: col++; /* fall through */
    case 1: return vram(n, shadow, 6144 + (line >> 3) * 32 + col);
    default: return 0xff;
    }
}
