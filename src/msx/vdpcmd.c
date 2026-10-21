/*
 * The V9938 command engine: LINE, SRCH, PSET, POINT and the block moves.
 *
 * Every VRAM access the engine makes waits for an access slot: the slot
 * tables (which tick of a line the VDP can give to the command engine or
 * the CPU) and the delay each command needs between its accesses were
 * measured on real V9938s by the openMSX developers (openMSX
 * src/video/VDPAccessSlots.cc and VDPCmdEngine.cc, GPL-2.0-or-later). The
 * engine runs a whole pixel or byte at a time: one unit happens once the
 * time of its write has been reached.
 */
#include "vdpcmd.h"

#include <string.h>

/* ---- access slots ------------------------------------------------------- */

/* Display off or vertical border. */
static const int16_t slots_screen_off[] = {
       0,    8,   16,   24,   32,   40,   48,   56,   64,   72,
      80,   88,   96,  104,  112,  120,  164,  172,  180,  188,
     196,  204,  212,  220,  228,  236,  244,  252,  260,  268,
     276,  292,  300,  308,  316,  324,  332,  340,  348,  356,
     364,  372,  380,  388,  396,  404,  420,  428,  436,  444,
     452,  460,  468,  476,  484,  492,  500,  508,  516,  524,
     532,  548,  556,  564,  572,  580,  588,  596,  604,  612,
     620,  628,  636,  644,  652,  660,  676,  684,  692,  700,
     708,  716,  724,  732,  740,  748,  756,  764,  772,  780,
     788,  804,  812,  820,  828,  836,  844,  852,  860,  868,
     876,  884,  892,  900,  908,  916,  932,  940,  948,  956,
     964,  972,  980,  988,  996, 1004, 1012, 1020, 1028, 1036,
    1044, 1060, 1068, 1076, 1084, 1092, 1100, 1108, 1116, 1124,
    1132, 1140, 1148, 1156, 1164, 1172, 1188, 1196, 1204, 1212,
    1220, 1228, 1268, 1276, 1284, 1292, 1300, 1308, 1316, 1324,
    1334, 1344, 1352, 1360, -1
};

/* Bitmap modes, sprites off. */
static const int16_t slots_sprites_off[] = {
       6,   14,   22,   30,   38,   46,   54,   62,   70,   78,
      86,   94,  102,  110,  118,  162,  170,  182,  188,  214,
     220,  246,  252,  278,  310,  316,  342,  348,  374,  380,
     406,  438,  444,  470,  476,  502,  508,  534,  566,  572,
     598,  604,  630,  636,  662,  694,  700,  726,  732,  758,
     764,  790,  822,  828,  854,  860,  886,  892,  918,  950,
     956,  982,  988, 1014, 1020, 1046, 1078, 1084, 1110, 1116,
    1142, 1148, 1174, 1206, 1212, 1266, 1274, 1282, 1290, 1298,
    1306, 1314, 1322, 1332, 1342, 1350, 1358, 1366, -1
};

/* Bitmap modes, sprites on. */
static const int16_t slots_sprites_on[] = {
      28,   92,  162,  170,  188,  220,  252,  316,  348,  380,
     444,  476,  508,  572,  604,  636,  700,  732,  764,  828,
     860,  892,  956,  988, 1020, 1084, 1116, 1148, 1212, 1264,
    1330, -1
};

/* Character modes. */
static const int16_t slots_char[] = {
      32,   96,  166,  174,  188,  220,  252,  316,  348,  380,
     444,  476,  508,  572,  604,  636,  700,  732,  764,  828,
     860,  892,  956,  988, 1020, 1084, 1116, 1148, 1212, 1268,
    1334, -1
};

/* Text modes. */
static const int16_t slots_text[] = {
       2,   10,   18,   26,   34,   42,   50,   58,   66,  166,
     174,  182,  190,  198,  206,  214,  222,  312,  408,  504,
     600,  696,  792,  888,  984, 1080, 1176, 1206, 1214, 1222,
    1230, 1238, 1246, 1254, 1262, 1270, 1278, 1286, 1294, 1302,
    1310, 1318, 1326, 1336, 1346, 1354, 1362, -1
};

enum { T_OFF, T_SPR_OFF, T_SPR_ON, T_CHAR, T_TEXT, T_COUNT };

static const int16_t *const slot_lists[T_COUNT] = {
    slots_screen_off, slots_sprites_off, slots_sprites_on, slots_char, slots_text
};

/* next[t][x]: the first slot at or after tick x of a line (may be in the
 * next line: then it is more than VDP_TPL). Built once; constant after. */
static uint16_t next_slot[T_COUNT][VDP_TPL];
static int next_built;

static void build_slots(void)
{
    int t, x, i;
    for (t = 0; t < T_COUNT; t++) {
        const int16_t *s = slot_lists[t];
        for (x = 0; x < VDP_TPL; x++) {
            for (i = 0; s[i] >= 0 && s[i] < x; i++)
                ;
            next_slot[t][x] = (uint16_t)(s[i] >= 0 ? s[i] : VDP_TPL + s[0]);
        }
    }
    next_built = 1;
}

enum { M_T1 = 0x01, M_T2 = 0x09, M_G4 = 0x0c, M_G5 = 0x10, M_G6 = 0x14, M_G7 = 0x1c };

static int bitmap(int mode)
{
    return mode == M_G4 || mode == M_G5 || mode == M_G6 || mode == M_G7;
}

static int table_at(const vdp *v, uint32_t tick)
{
    int mode = vdp_mode(v), line = (int)(tick / VDP_TPL);
    uint32_t from, to;
    if (!bitmap(mode))
        return (mode == M_T1 || mode == M_T2) ? T_TEXT : T_CHAR;
    /* The grid switches 162 ticks into the line before the display, and
     * back 162 ticks into the line after it. */
    from = (uint32_t)(v->line_zero - 1) * VDP_TPL + 162;
    to = (uint32_t)(v->line_zero + v->disp_lines) * VDP_TPL + 162;
    if (!(v->reg[1] & 0x40) || tick < from || tick >= to)
        return T_OFF;
    (void)line;
    return (v->reg[8] & 2) ? T_SPR_OFF : T_SPR_ON;
}

/* The first access slot at least delta ticks after tick. */
static uint32_t slot_after(const vdp *v, uint32_t tick, int delta)
{
    int t = table_at(v, tick);
    uint32_t line = tick / VDP_TPL, x = tick % VDP_TPL + (uint32_t)delta;
    if (t == T_SPR_ON)
        x++;                    /* sprite fetches cost the engine a cycle */
    while (x >= VDP_TPL) {
        x -= VDP_TPL;
        line++;
    }
    return line * VDP_TPL + next_slot[t][x];
}

/* ---- pixels ------------------------------------------------------------- */

typedef struct pmode {
    int width, shift, mask;    /* pixels per line, log2 pixels per byte, colour mask */
} pmode;

static pmode pmode_of(const vdp *v)
{
    pmode p;
    switch (vdp_mode(v)) {
    case M_G4: p.width = 256; p.shift = 1; p.mask = 15; break;
    case M_G5: p.width = 512; p.shift = 2; p.mask = 3; break;
    case M_G6: p.width = 512; p.shift = 1; p.mask = 15; break;
    default:   p.width = 256; p.shift = 0; p.mask = 255; break;
    }
    return p;
}

static uint32_t pix_addr(const vdp *v, int x, int y)
{
    switch (vdp_mode(v)) {
    case M_G4: return (uint32_t)(y & 1023) << 7 | (uint32_t)(x & 255) >> 1;
    case M_G5: return (uint32_t)(y & 1023) << 7 | (uint32_t)(x & 511) >> 2;
    case M_G6: return (uint32_t)(y & 511) << 8 | (uint32_t)(x & 511) >> 1;
    default:   return (uint32_t)(y & 511) << 8 | (uint32_t)(x & 255);
    }
}

static int pix_shift(const vdp *v, int x)
{
    switch (vdp_mode(v)) {
    case M_G4:
    case M_G6: return (~x & 1) << 2;
    case M_G5: return (3 - (x & 3)) << 1;
    default:   return 0;
    }
}

static uint8_t vread(const vdp *v, uint32_t a)
{
    return v->vram[vdp_phys(v, a)];
}

static void vwrite(vdp *v, uint32_t a, uint8_t b)
{
    v->vram[vdp_phys(v, a)] = b;
}

static int point(const vdp *v, int x, int y)
{
    pmode p = pmode_of(v);
    return (vread(v, pix_addr(v, x, y)) >> pix_shift(v, x)) & p.mask;
}

static uint8_t logop(int op, int src, int dst, int mask)
{
    if ((op & 8) && !src)
        return (uint8_t)dst;
    switch (op & 7) {
    case 0: return (uint8_t)src;
    case 1: return (uint8_t)(src & dst);
    case 2: return (uint8_t)(src | dst);
    case 3: return (uint8_t)(src ^ dst);
    case 4: return (uint8_t)(~src & mask);
    default: return (uint8_t)dst;
    }
}

static void pset(vdp *v, int x, int y, int c, int op)
{
    pmode p = pmode_of(v);
    uint32_t a = pix_addr(v, x, y);
    int sh = pix_shift(v, x);
    uint8_t b = vread(v, a);
    int d = (b >> sh) & p.mask;
    int n = logop(op, c & p.mask, d, p.mask);
    vwrite(v, a, (uint8_t)((b & ~(p.mask << sh)) | (n << sh)));
}

/* ---- clipping (as the V9938 does it) ------------------------------------ */

#define DIX(c) ((c)->arg & 0x04)
#define DIY(c) ((c)->arg & 0x08)

static int clip_nx_pixel(int dx, int nx, int arg, int width)
{
    if (dx >= width)
        return 1;
    nx = nx ? nx : width;
    return (arg & 0x04) ? (nx < dx + 1 ? nx : dx + 1) : (nx < width - dx ? nx : width - dx);
}

static int clip_nx2_pixel(int sx, int dx, int nx, int arg, int width)
{
    int lo = sx < dx ? sx : dx, hi = sx > dx ? sx : dx;
    if (sx >= width || dx >= width)
        return 1;
    nx = nx ? nx : width;
    return (arg & 0x04) ? (nx < lo + 1 ? nx : lo + 1) : (nx < width - hi ? nx : width - hi);
}

static int clip_nx_byte(int dx, int nx, int arg, pmode p)
{
    int bpl = p.width >> p.shift;
    dx >>= p.shift;
    if (dx >= bpl)
        return 1;
    nx >>= p.shift;
    nx = nx ? nx : bpl;
    return (arg & 0x04) ? (nx < dx + 1 ? nx : dx + 1) : (nx < bpl - dx ? nx : bpl - dx);
}

static int clip_nx2_byte(int sx, int dx, int nx, int arg, pmode p)
{
    int bpl = p.width >> p.shift, lo, hi;
    sx >>= p.shift;
    dx >>= p.shift;
    if (sx >= bpl || dx >= bpl)
        return 1;
    lo = sx < dx ? sx : dx;
    hi = sx > dx ? sx : dx;
    nx >>= p.shift;
    nx = nx ? nx : bpl;
    return (arg & 0x04) ? (nx < lo + 1 ? nx : lo + 1) : (nx < bpl - hi ? nx : bpl - hi);
}

static int clip_ny(int dy, int ny, int arg)
{
    ny = ny ? ny : 1024;
    return (arg & 0x08) ? (ny < dy + 1 ? ny : dy + 1) : ny;
}

static int clip_ny2(int sy, int dy, int ny, int arg)
{
    int lo = sy < dy ? sy : dy;
    ny = ny ? ny : 1024;
    return (arg & 0x08) ? (ny < lo + 1 ? ny : lo + 1) : ny;
}

/* ---- commands ----------------------------------------------------------- */

enum {
    C_STOP = 0, C_POINT = 4, C_PSET, C_SRCH, C_LINE, C_LMMV, C_LMMM, C_LMCM,
    C_LMMC, C_HMMV, C_HMMM, C_YMMM, C_HMMC
};

static void done(vdp *v)
{
    v->cmd.busy = 0;
    v->cmd.cmd &= 0x0f;
    v->stat[2] &= ~0x01;
}

/* Count off one unit along the row; at its end, next row. Returns 1 when
 * the whole command is done. */
static int row_step(vdp *v, int *ax, int *ax2, int step, int start, int start2, int tmpnx,
                    int *tmpny, int two_y)
{
    vdp_cmd *c = &v->cmd;
    int ty = DIY(c) ? -1 : 1;
    if (--c->anx == 0) {
        c->dy += ty;
        if (two_y)
            c->sy += ty;
        c->ny = (c->ny - 1) & 1023;
        *ax = start;
        if (ax2)
            *ax2 = start2;
        c->anx = tmpnx;
        if (--*tmpny == 0)
            return 1;
        return 2;               /* end of row */
    }
    *ax += step;
    if (ax2)
        *ax2 += step;
    return 0;
}

static void start(vdp *v, uint32_t now)
{
    vdp_cmd *c = &v->cmd;
    pmode p = pmode_of(v);
    int startd;
    c->ny &= 1023;
    c->busy = 1;
    v->stat[2] |= 0x01;
    switch (c->cmd >> 4) {
    case C_POINT: startd = 63; break;
    case C_LMMM: startd = 64; break;
    case C_LMCM: startd = 76; break;
    case C_HMMM: case C_YMMM: startd = 100; break;
    case C_HMMV: case C_LINE: case C_HMMC: startd = 112; break;
    default: startd = 88; break;
    }
    c->time = slot_after(v, now, startd);
    switch (c->cmd >> 4) {
    case C_SRCH:
        c->asx = c->sx;
        break;
    case C_LINE:
        c->asx = (c->nx - 1) >> 1;
        c->adx = c->dx;
        c->anx = 0;
        break;
    case C_LMMV:
    case C_LMMC:
        c->adx = c->dx;
        c->anx = clip_nx_pixel(c->dx, c->nx, c->arg, p.width);
        if ((c->cmd >> 4) == C_LMMC)
            v->stat[2] |= 0x80;
        break;
    case C_LMMM:
        c->asx = c->sx;
        c->adx = c->dx;
        c->anx = clip_nx2_pixel(c->sx, c->dx, c->nx, c->arg, p.width);
        break;
    case C_LMCM:
        c->asx = c->sx;
        c->anx = clip_nx_pixel(c->sx, c->nx, c->arg, p.width);
        c->transfer = 1;
        v->stat[2] |= 0x80;
        break;
    case C_HMMV:
    case C_HMMC:
        c->adx = c->dx;
        c->anx = clip_nx_byte(c->dx, c->nx, c->arg, p);
        if ((c->cmd >> 4) == C_HMMC)
            v->stat[2] |= 0x80;
        break;
    case C_HMMM:
        c->asx = c->sx;
        c->adx = c->dx;
        c->anx = clip_nx2_byte(c->sx, c->dx, c->nx, c->arg, p);
        break;
    case C_YMMM:
        c->adx = c->dx;
        c->anx = clip_nx_byte(c->dx, 512, c->arg, p);
        break;
    case C_STOP:
    default:
        done(v);
        break;
    }
}

void vdp_cmd_run(vdp *v, uint32_t upto)
{
    vdp_cmd *c = &v->cmd;
    pmode p;
    int tx, ty, op, n;

    if (!next_built)
        build_slots();
    if (!c->busy)
        return;
    p = pmode_of(v);
    tx = DIX(c) ? -1 : 1;
    ty = DIY(c) ? -1 : 1;
    op = c->cmd & 15;

    while (c->busy && c->time < upto) {
        uint32_t t1, t2;
        switch (c->cmd >> 4) {
        case C_POINT:
            v->stat[7] = (uint8_t)point(v, c->sx, c->sy);
            done(v);
            break;

        case C_PSET:
            t1 = slot_after(v, c->time, 24);
            if (t1 > upto)
                return;
            pset(v, c->dx, c->dy, c->col, op);
            c->time = t1;
            done(v);
            break;

        case C_SRCH: {
            int col = c->col & p.mask;
            int eq = (c->arg & 0x02) != 0;
            if ((point(v, c->asx, c->sy) == col) ^ eq) {
                v->stat[2] |= 0x10;
                v->stat[8] = (uint8_t)c->asx;
                v->stat[9] = (uint8_t)(c->asx >> 8);
                done(v);
                break;
            }
            c->asx += tx;
            if (c->asx & p.width) {
                done(v);
                break;
            }
            c->time = slot_after(v, c->time, 88);
            break;
        }

        case C_LINE: {
            int delta = 84;
            t1 = slot_after(v, c->time, 24);
            if (t1 > upto)
                return;
            pset(v, c->adx, c->dy, c->col, op);
            if (!(c->arg & 0x01)) {
                c->adx += tx;
                if (c->anx++ == c->nx || (c->adx & p.width)) {
                    done(v);
                    break;
                }
                if (c->asx < c->ny) {
                    c->asx += c->nx;
                    c->dy += ty;
                    delta = 120;
                    if (ty < 0 && c->dy < 0) {
                        done(v);
                        break;
                    }
                }
                c->asx = (c->asx - c->ny) & 1023;
            } else {
                c->dy += ty;
                if (ty < 0 && c->dy < 0) {
                    done(v);
                    break;
                }
                if (c->asx < c->ny) {
                    c->asx += c->nx;
                    c->adx += tx;
                    delta = 120;
                }
                c->asx = (c->asx - c->ny) & 1023;
                if (c->anx++ == c->nx || (c->adx & p.width)) {
                    done(v);
                    break;
                }
            }
            c->dy &= 1023;
            c->time = slot_after(v, t1, delta);
            break;
        }

        case C_LMMV: {
            int tmpnx = clip_nx_pixel(c->dx, c->nx, c->arg, p.width);
            int tmpny = clip_ny(c->dy, c->ny, c->arg);
            t1 = slot_after(v, c->time, 24);
            if (t1 > upto)
                return;
            pset(v, c->adx, c->dy, c->col, op);
            n = row_step(v, &c->adx, NULL, tx, c->dx, 0, tmpnx, &tmpny, 0);
            if (n == 1) {
                done(v);
                break;
            }
            c->time = slot_after(v, t1, n ? 72 + 58 : 72);
            break;
        }

        case C_LMMM: {
            int tmpnx = clip_nx2_pixel(c->sx, c->dx, c->nx, c->arg, p.width);
            int tmpny = clip_ny2(c->sy, c->dy, c->ny, c->arg);
            int src;
            t1 = slot_after(v, c->time, 32);
            t2 = slot_after(v, t1, 24);
            if (t2 > upto)
                return;
            src = point(v, c->asx, c->sy);
            pset(v, c->adx, c->dy, src, op);
            n = row_step(v, &c->asx, &c->adx, tx, c->sx, c->dx, tmpnx, &tmpny, 1);
            if (n == 1) {
                done(v);
                break;
            }
            c->time = slot_after(v, t2, n ? 60 + 68 : 60);
            break;
        }

        case C_LMCM: {
            int tmpnx = clip_nx_pixel(c->sx, c->nx, c->arg, p.width);
            int tmpny = clip_ny(c->sy, c->ny, c->arg);
            if (!c->transfer) {
                c->time = upto;
                return;
            }
            v->stat[7] = (uint8_t)point(v, c->asx, c->sy);
            c->transfer = 0;
            v->stat[2] |= 0x80;
            if (--c->anx == 0) {
                c->sy += ty;
                c->ny = (c->ny - 1) & 1023;
                c->asx = c->sx;
                c->anx = tmpnx;
                if (--tmpny == 0) {
                    done(v);
                    break;
                }
            } else {
                c->asx += tx;
            }
            c->time = slot_after(v, c->time, 24);
            break;
        }

        case C_LMMC:
        case C_HMMC: {
            int byte = (c->cmd >> 4) == C_HMMC;
            int tmpnx = byte ? clip_nx_byte(c->dx, c->nx, c->arg, p)
                             : clip_nx_pixel(c->dx, c->nx, c->arg, p.width);
            int tmpny = clip_ny(c->dy, c->ny, c->arg);
            if (!c->transfer) {
                c->time = upto;
                return;
            }
            if (byte)
                vwrite(v, pix_addr(v, c->adx, c->dy), c->col);
            else
                pset(v, c->adx, c->dy, c->col, op);
            c->transfer = 0;
            n = row_step(v, &c->adx, NULL, byte ? tx << p.shift : tx, c->dx, 0, tmpnx, &tmpny, 0);
            if (n == 1) {
                done(v);
                break;
            }
            c->time = slot_after(v, c->time, 24);
            break;
        }

        case C_HMMV: {
            int tmpnx = clip_nx_byte(c->dx, c->nx, c->arg, p);
            int tmpny = clip_ny(c->dy, c->ny, c->arg);
            vwrite(v, pix_addr(v, c->adx, c->dy), c->col);
            n = row_step(v, &c->adx, NULL, tx << p.shift, c->dx, 0, tmpnx, &tmpny, 0);
            if (n == 1) {
                done(v);
                break;
            }
            c->time = slot_after(v, c->time, n ? 46 + 58 : 46);
            break;
        }

        case C_HMMM: {
            int tmpnx = clip_nx2_byte(c->sx, c->dx, c->nx, c->arg, p);
            int tmpny = clip_ny2(c->sy, c->dy, c->ny, c->arg);
            uint8_t b;
            t1 = slot_after(v, c->time, 24);
            if (t1 > upto)
                return;
            b = vread(v, pix_addr(v, c->asx, c->sy));
            vwrite(v, pix_addr(v, c->adx, c->dy), b);
            n = row_step(v, &c->asx, &c->adx, tx << p.shift, c->sx, c->dx, tmpnx, &tmpny, 1);
            if (n == 1) {
                done(v);
                break;
            }
            c->time = slot_after(v, t1, n ? 60 + 68 : 60);
            break;
        }

        case C_YMMM: {
            int tmpnx = clip_nx_byte(c->dx, 512, c->arg, p);
            int tmpny = clip_ny2(c->sy, c->dy, c->ny, c->arg);
            uint8_t b;
            t1 = slot_after(v, c->time, 24);
            if (t1 > upto)
                return;
            b = vread(v, pix_addr(v, c->adx, c->sy));
            vwrite(v, pix_addr(v, c->adx, c->dy), b);
            n = row_step(v, &c->adx, NULL, tx << p.shift, c->dx, 0, tmpnx, &tmpny, 1);
            if (n == 1) {
                done(v);
                break;
            }
            c->time = slot_after(v, t1, n ? 36 + 68 : 36);
            break;
        }

        default:
            done(v);
            break;
        }
    }
}

void vdp_cmd_write(vdp *v, int reg, uint8_t val)
{
    vdp_cmd *c = &v->cmd;
    switch (reg) {
    case 32: c->sx = (c->sx & 0x100) | val; break;
    case 33: c->sx = (c->sx & 0x0ff) | (val & 1) << 8; break;
    case 34: c->sy = (c->sy & 0x300) | val; break;
    case 35: c->sy = (c->sy & 0x0ff) | (val & 3) << 8; break;
    case 36: c->dx = (c->dx & 0x100) | val; break;
    case 37: c->dx = (c->dx & 0x0ff) | (val & 1) << 8; break;
    case 38: c->dy = (c->dy & 0x300) | val; break;
    case 39: c->dy = (c->dy & 0x0ff) | (val & 3) << 8; break;
    case 40: c->nx = (c->nx & 0x300) | val; break;
    case 41: c->nx = (c->nx & 0x0ff) | (val & 3) << 8; break;
    case 42: c->ny = (c->ny & 0x300) | val; break;
    case 43: c->ny = (c->ny & 0x0ff) | (val & 3) << 8; break;
    case 44:
        c->col = val;
        if (!c->busy)
            v->stat[2] &= ~0x80;
        c->transfer = 1;
        break;
    case 45:
        c->arg = val;
        break;
    case 46:
        c->cmd = val;
        if (!next_built)
            build_slots();
        start(v, v->synced);
        break;
    }
}

void vdp_cmd_read_s7(vdp *v)
{
    v->cmd.transfer = 1;
    if (!v->cmd.busy)
        v->stat[2] &= ~0x80;
}
