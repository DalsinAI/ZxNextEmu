/*
 * Spec256: eight shadow Z80s on bit-plane memory. See spec256.h.
 */
#include "spec256.h"
#include "../zx/zx.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 0x4000
#define GFX_PAGE (PAGE * 8)        /* a page's eight planes, interleaved */
#define CORES 8
#define MAX_ACC 16                 /* accesses of one kind per instruction */
#define MAX_BKG 100
#define MAX_RULES 32

/* Register alignment, ZX-Poly's letters: AFBCDEHL, IX/IY halves,
 * F and F' without carry, PC, SP halves, then the alternate set. */
static const char align_letters[] = "AFBCDEHLXxYy10PSsafbcdehl";

typedef struct bk_rule {
    uint8_t bkg, val;
    uint16_t addr;
} bk_rule;

struct s256 {
    /* ---- saved in machine state ---- */
    uint8_t ram[8][GFX_PAGE];      /* byte (offset * 8 + k) is plane k */
    z80 core[CORES];
    int32_t bkg;                   /* background shown, -1 for none */

    /* ---- settings (from the .CFG) ---- */
    uint32_t align;
    int bk_over_ff, hide_same, paper00inkff, up_mixed, down_mixed;
    bk_rule rules[MAX_RULES];
    int nrules;

    /* ---- media ---- */
    uint8_t rom[4][GFX_PAGE];
    uint8_t rom48[PAGE], rom128[2][PAGE];
    int have_rom48, have_rom128;
    uint8_t *gfx;                  /* the .GFX file, 3 pages */
    uint8_t *gfn[8];               /* .GF0-.GF7 */
    uint8_t *gfr[2];               /* .GFA/.GFB (ROM 0/1 of a 128K) */
    uint8_t *bkgs[MAX_BKG];        /* 320x200 */
    uint8_t pal[768];
    char cfg_align[32];
    char snap_name[32];
    int cfg_set;                   /* the .CFG named zxpAlignRegs */
    uint16_t rgb[256];             /* the palette as RGB333 */
    uint16_t bkg_rgb[192][256];

    /* ---- the real CPU's accesses during the current instruction ---- */
    struct zx *m;
    z80_bus inner, shadow;
    uint16_t r_addr[MAX_ACC], w_addr[MAX_ACC];
    uint8_t f_val[MAX_ACC], io_val[MAX_ACC];
    int nr, nw, nf, nio;
    /* the shadow CPU's position in that record */
    int k, sr, sw, sf, sio;
    int recording;
    s256_trace_fn trace;
    void *trace_user;
};

/* ---- planes ----------------------------------------------------------- */

/* Planes behind a 16K memory slot, or NULL (unmapped). */
static uint8_t *planes_of(s256 *s, const uint8_t *page)
{
    zx *m = s->m;
    uintptr_t p = (uintptr_t)page, r = (uintptr_t)&m->ram[0][0], o = (uintptr_t)&m->rom[0][0];
    if (p - r < (uintptr_t)m->ram_pages * PAGE)
        return s->ram[(p - r) / PAGE];
    if (p - o < sizeof m->rom)
        return s->rom[(p - o) / PAGE];
    return NULL;
}

static uint8_t plane_read(s256 *s, uint16_t addr)
{
    uint8_t *pl = planes_of(s, s->m->rd[addr >> 14]);
    return pl ? pl[(addr & 0x3fff) * 8 + s->k] : 0xff;
}

static void plane_write(s256 *s, uint16_t addr, uint8_t v)
{
    uint8_t *pl;
    if (!s->m->wr[addr >> 14])
        return;
    pl = planes_of(s, s->m->wr[addr >> 14]);
    if (pl)
        pl[(addr & 0x3fff) * 8 + s->k] = v;
}

/* A page from a file: there, byte i of each group of eight is the colour of
 * pixel bit i (an 8x8 bit transpose of the planes). */
static void from_file(uint8_t *planes, const uint8_t *file)
{
    int i, k, b;
    for (i = 0; i < GFX_PAGE; i += 8)
        for (k = 0; k < 8; k++) {
            uint8_t v = 0;
            for (b = 0; b < 8; b++)
                v = (uint8_t)(v | ((file[i + b] >> k) & 1) << b);
            planes[i + k] = v;
        }
}

/* Every plane a copy of the original byte: colour 0xFF where a pixel is set. */
static void expand(uint8_t *planes, const uint8_t *orig)
{
    int i;
    for (i = 0; i < PAGE; i++)
        memset(planes + i * 8, orig[i], 8);
}

/* ---- the real CPU's bus: pass through, remembering each access ------- */

static uint8_t m_fetch(void *ctx, uint16_t a)
{
    s256 *s = ctx;
    uint8_t v = s->inner.fetch(s->inner.ctx, a);
    if (s->recording && s->nf < MAX_ACC)
        s->f_val[s->nf++] = v;
    return v;
}

static uint8_t m_read(void *ctx, uint16_t a)
{
    s256 *s = ctx;
    if (s->recording && s->nr < MAX_ACC)
        s->r_addr[s->nr++] = a;
    return s->inner.read(s->inner.ctx, a);
}

static void m_write(void *ctx, uint16_t a, uint8_t v)
{
    s256 *s = ctx;
    if (s->recording && s->nw < MAX_ACC)
        s->w_addr[s->nw++] = a;
    s->inner.write(s->inner.ctx, a, v);
}

static uint8_t m_io_read(void *ctx, uint16_t port)
{
    s256 *s = ctx;
    uint8_t v = s->inner.io_read(s->inner.ctx, port);
    if (s->recording && s->nio < MAX_ACC)
        s->io_val[s->nio++] = v;
    return v;
}

static void m_io_write(void *ctx, uint16_t port, uint8_t v)
{
    s256 *s = ctx;
    s->inner.io_write(s->inner.ctx, port, v);
}

static void m_internal(void *ctx, uint16_t a, unsigned n)
{
    s256 *s = ctx;
    s->inner.internal(s->inner.ctx, a, n);
}

static void m_tick(void *ctx, unsigned n)
{
    s256 *s = ctx;
    s->inner.tick(s->inner.ctx, n);
}

static uint8_t m_int_ack(void *ctx)
{
    s256 *s = ctx;
    return s->inner.int_ack ? s->inner.int_ack(s->inner.ctx) : 0xff;
}

/* ---- the shadow CPUs' bus: opcodes from real memory, data from planes - */

static uint8_t g_fetch(void *ctx, uint16_t a)
{
    s256 *s = ctx;
    if (s->sf < s->nf)
        return s->f_val[s->sf++];
    return zx_peek(s->m, a);
}

static uint8_t g_read(void *ctx, uint16_t a)
{
    s256 *s = ctx;
    if (s->sr < s->nr)
        a = s->r_addr[s->sr++];
    return plane_read(s, a);
}

static void g_write(void *ctx, uint16_t a, uint8_t v)
{
    s256 *s = ctx;
    if (s->sw < s->nw)
        a = s->w_addr[s->sw++];
    plane_write(s, a, v);
}

static uint8_t g_io_read(void *ctx, uint16_t port)
{
    s256 *s = ctx;
    (void)port;
    return s->sio < s->nio ? s->io_val[s->sio++] : 0xff;
}

static void g_io_write(void *ctx, uint16_t port, uint8_t v) { (void)ctx; (void)port; (void)v; }
static void g_internal(void *ctx, uint16_t a, unsigned n) { (void)ctx; (void)a; (void)n; }
static void g_tick(void *ctx, unsigned n) { (void)ctx; (void)n; }
static uint8_t g_int_ack(void *ctx) { (void)ctx; return 0xff; }

/* ---- stepping --------------------------------------------------------- */

/* Copy the aligned registers of the real CPU (as it was before the
 * instruction) into a shadow CPU. */
static void align(const s256 *s, z80 *g, const z80 *c)
{
    uint32_t a = s->align;
    g->iff1 = c->iff1; g->iff2 = c->iff2; g->im = c->im;
    g->i = c->i; g->r = c->r;
    g->halted = c->halted; g->ei_delay = c->ei_delay;
    if (!a) {
        g->pc = c->pc;
        g->sp = c->sp;
        return;
    }
    if (a & 1 << 0) g->a = c->a;
    if (a & 1 << 1) g->f = c->f;
    if (a & 1 << 2) g->b = c->b;
    if (a & 1 << 3) g->c = c->c;
    if (a & 1 << 4) g->d = c->d;
    if (a & 1 << 5) g->e = c->e;
    if (a & 1 << 6) g->h = c->h;
    if (a & 1 << 7) g->l = c->l;
    if (a & 1 << 8) g->ix = (uint16_t)((g->ix & 0x00ff) | (c->ix & 0xff00));
    if (a & 1 << 9) g->ix = (uint16_t)((g->ix & 0xff00) | (c->ix & 0x00ff));
    if (a & 1 << 10) g->iy = (uint16_t)((g->iy & 0x00ff) | (c->iy & 0xff00));
    if (a & 1 << 11) g->iy = (uint16_t)((g->iy & 0xff00) | (c->iy & 0x00ff));
    if (a & 1 << 12) g->f = (uint8_t)((g->f & Z80_FC) | (c->f & ~Z80_FC));
    if (a & 1 << 13) g->f_ = (uint8_t)((g->f_ & Z80_FC) | (c->f_ & ~Z80_FC));
    if (a & 1 << 14) g->pc = c->pc;
    if (a & 1 << 15) g->sp = (uint16_t)((g->sp & 0x00ff) | (c->sp & 0xff00));
    if (a & 1 << 16) g->sp = (uint16_t)((g->sp & 0xff00) | (c->sp & 0x00ff));
    if (a & 1 << 17) g->a_ = c->a_;
    if (a & 1 << 18) g->f_ = c->f_;
    if (a & 1 << 19) g->b_ = c->b_;
    if (a & 1 << 20) g->c_ = c->c_;
    if (a & 1 << 21) g->d_ = c->d_;
    if (a & 1 << 22) g->e_ = c->e_;
    if (a & 1 << 23) g->h_ = c->h_;
    if (a & 1 << 24) g->l_ = c->l_;
}

static void begin(s256 *s)
{
    s->nr = s->nw = s->nf = s->nio = 0;
    s->recording = 1;
}

/* Report this instruction's writes to the bitmap and blank their tags. */
static void traced(s256 *s)
{
    int i;
    for (i = 0; i < s->nw; i++) {
        uint16_t a = s->w_addr[i];
        uint8_t *pl;
        if (a < 0x4000 || a >= 0x5800 || !(pl = planes_of(s, s->m->wr[a >> 14])))
            continue;
        uint8_t v;
        pl += (a & 0x3fff) * 8;
        v = zx_peek(s->m, a);
        /* Planes 2 and 3 carry the tag's complement. A byte moved from its
           source keeps that pairing; a constant (every plane alike) or an ALU
           mix of two tags almost never does, so those are no tag. */
        s->trace(s->trace_user, a, v, pl[2] == (uint8_t)~pl[0] && pl[3] == (uint8_t)~pl[1]
                 ? (uint16_t)(pl[0] | pl[1] << 8) : 0);
        pl[0] = pl[1] = pl[2] = pl[3] = 0;
    }
}

static void shadows(s256 *s, const z80 *before, int interrupt)
{
    int k;
    s->recording = 0;
    for (k = 0; k < CORES; k++) {
        z80 *g = &s->core[k];
        s->k = k;
        s->sr = s->sw = s->sf = s->sio = 0;
        align(s, g, before);
        if (interrupt)
            z80_interrupt(g, &s->shadow);
        else
            z80_step(g, &s->shadow);
    }
    if (s->trace)
        traced(s);
}

void s256_step(s256 *s)
{
    zx *m = s->m;
    z80 before = m->cpu;
    begin(s);
    z80_step(&m->cpu, &m->bus);
    shadows(s, &before, 0);
}

int s256_interrupt(s256 *s)
{
    zx *m = s->m;
    z80 before = m->cpu;
    int taken;
    begin(s);
    taken = z80_interrupt(&m->cpu, &m->bus);
    if (taken)
        shadows(s, &before, 1);
    s->recording = 0;
    return taken;
}

/* ---- picture ---------------------------------------------------------- */

static const uint8_t zx_rgb[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0xb6 }, { 0xb6, 0x00, 0x00 }, { 0xb6, 0x00, 0xb6 },
    { 0x00, 0xb6, 0x00 }, { 0x00, 0xb6, 0xb6 }, { 0xb6, 0xb6, 0x00 }, { 0xb6, 0xb6, 0xb6 },
    { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0xff }, { 0xff, 0x00, 0x00 }, { 0xff, 0x00, 0xff },
    { 0x00, 0xff, 0x00 }, { 0x00, 0xff, 0xff }, { 0xff, 0xff, 0x00 }, { 0xff, 0xff, 0xff },
};

static uint16_t to333(int r, int g, int b)
{
    return (uint16_t)(((r * 7 + 127) / 255) << 6 | ((g * 7 + 127) / 255) << 3 | ((b * 7 + 127) / 255));
}

static void make_bkg_rgb(s256 *s)
{
    const uint8_t *b = s->bkg >= 0 ? s->bkgs[s->bkg] : NULL;
    int x, y;
    if (!b)
        return;
    /* 320x200, centred on the 256x192 paper. */
    for (y = 0; y < 192; y++)
        for (x = 0; x < 256; x++)
            s->bkg_rgb[y][x] = s->rgb[b[(y + 4) * 320 + x + 32]];
}

static void pick_background(s256 *s)
{
    int i;
    for (i = 0; i < s->nrules; i++) {
        const bk_rule *r = &s->rules[i];
        if (s->bkgs[r->bkg] && zx_peek(s->m, r->addr) == r->val) {
            if (s->bkg != r->bkg) {
                s->bkg = r->bkg;
                make_bkg_rgb(s);
            }
            return;
        }
    }
}

void s256_render(s256 *s)
{
    zx *m = s->m;
    const uint8_t *scr = m->ram[m->screen_page];
    const uint8_t *pl = s->ram[m->screen_page];
    int flash_on = (m->frames & 16) != 0;
    int has_bkg = s->bkg >= 0 && s->bkgs[s->bkg];
    int p00 = s->paper00inkff >= 0 ? s->paper00inkff : !has_bkg;
    int up = 0xff - s->up_mixed, down = s->down_mixed;
    int row, col, x, p;

    pick_background(s);
    has_bkg = s->bkg >= 0 && s->bkgs[s->bkg];
    for (row = 0; row < 192; row++) {
        uint16_t *px = &m->screen[ZX_BORDER_Y + row][ZX_BORDER_X];
        for (col = 0; col < 32; col++) {
            int off = ((row & 0xc0) << 5) | ((row & 7) << 8) | ((row & 0x38) << 2) | col;
            const uint8_t *g = pl + off * 8;
            uint8_t orig = scr[off];
            uint8_t attr = scr[6144 + (row >> 3) * 32 + col];
            int bright = (attr & 0x40) >> 3;
            const uint8_t *ink = zx_rgb[(attr & 7) | bright];
            const uint8_t *paper = zx_rgb[((attr >> 3) & 7) | bright];
            int same = (attr & 7) == ((attr >> 3) & 7);
            int flashing = (attr & 0x80) && flash_on;
            if (flashing) {
                const uint8_t *t = ink;
                ink = paper;
                paper = t;
            }
            for (x = 0; x < 8; x++) {
                int bit = 7 - x, ci = 0, draw = 1;
                const uint8_t *src;
                uint8_t rgb[3];
                /* While tracing, planes 0 to 3 carry address tags, so the
                   picture comes from the plain Spectrum byte instead. */
                if (s->trace)
                    ci = (orig >> bit & 1) ? 0xff : 0;
                else
                    for (p = 0; p < 8; p++)
                        ci |= ((g[p] >> bit) & 1) << p;
                src = &s->pal[ci * 3];
                memcpy(rgb, src, 3);
                if (!has_bkg) {
                    if (s->hide_same && same)
                        memcpy(rgb, ink, 3);
                    else if (p00 && ci == 0)
                        memcpy(rgb, paper, 3);
                    else if (p00 && ci == 0xff)
                        memcpy(rgb, ink, 3);
                } else {
                    int show_back = flashing || (s->hide_same && same);
                    if (p00 && ci == 0)
                        memcpy(rgb, paper, 3);
                    else if (p00 && ci == 0xff)
                        memcpy(rgb, ink, 3);
                    else if (p00)
                        draw = !show_back;
                    else
                        draw = !(show_back || ci == 0 || (s->bk_over_ff && ci == 0xff));
                }
                if (!draw) {
                    px[col * 8 + x] = s->bkg_rgb[row][col * 8 + x];
                    continue;
                }
                if (ci < down || ci > up) {
                    const uint8_t *a = (orig >> bit) & 1 ? ink : paper;
                    rgb[0] = (uint8_t)((rgb[0] + a[0]) >> 1);
                    rgb[1] = (uint8_t)((rgb[1] + a[1]) >> 1);
                    rgb[2] = (uint8_t)((rgb[2] + a[2]) >> 1);
                }
                px[col * 8 + x] = to333(rgb[0], rgb[1], rgb[2]);
            }
        }
    }
}

/* ---- settings --------------------------------------------------------- */

static uint32_t parse_align(const char *str)
{
    uint32_t a = 0;
    const char *c, *pos;
    for (c = str; *c; c++) {
        if (*c != ' ' && *c != '\r' && *c != '\n' && (pos = strchr(align_letters, *c)))
            a |= 1u << (pos - align_letters);
    }
    return a;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int name_eq(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return !*a && !*b;
}

static void defaults(s256 *s)
{
    s->bk_over_ff = 0;
    s->hide_same = 1;
    s->paper00inkff = -1;          /* on unless there is a background */
    s->up_mixed = 64;
    s->down_mixed = 0;
    s->nrules = 0;
    s->cfg_set = 0;
}

static int hexval(const char *p, unsigned *out)
{
    unsigned v = 0;
    int n = 0;
    for (;; p++, n++) {
        int c = lower(*p);
        if (c >= '0' && c <= '9') v = v * 16 + (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (unsigned)(c - 'a' + 10);
        else break;
    }
    *out = v;
    return n;
}

static void parse_cfg(s256 *s, const char *d, size_t len)
{
    char line[96];
    size_t i = 0;
    while (i < len) {
        size_t n = 0;
        char *eq;
        while (i < len && d[i] != '\n' && d[i] != '\r') {
            if (n < sizeof line - 1)
                line[n++] = d[i];
            i++;
        }
        while (i < len && (d[i] == '\n' || d[i] == '\r'))
            i++;
        line[n] = 0;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        if (line[0] >= '0' && line[0] <= '9' && line[2] == ':') {
            /* "NN: addr=VVh": background NN while (addr) == VV */
            unsigned v;
            int bk = (line[0] - '0') * 10 + (line[1] - '0');
            if (s->nrules < MAX_RULES && hexval(eq + 1, &v)) {
                s->rules[s->nrules].bkg = (uint8_t)bk;
                s->rules[s->nrules].addr = (uint16_t)atoi(line + 3);
                s->rules[s->nrules].val = (uint8_t)v;
                s->nrules++;
            }
        } else if (!strcmp(line, "zxpAlignRegs")) {
            strncpy(s->cfg_align, eq + 1, sizeof s->cfg_align - 1);
            s->cfg_set = 1;
        } else if (!strcmp(line, "BkOverFF")) {
            s->bk_over_ff = atoi(eq + 1) != 0;
        } else if (!strcmp(line, "HideSameInkPaper")) {
            s->hide_same = atoi(eq + 1) != 0;
        } else if (!strcmp(line, "Paper00InkFF")) {
            s->paper00inkff = atoi(eq + 1) != 0;
        } else if (!strcmp(line, "UpColorsMixed")) {
            s->up_mixed = atoi(eq + 1);
        } else if (!strcmp(line, "DownColorsMixed")) {
            s->down_mixed = atoi(eq + 1);
        }
        /* GFXLeveledXOR/OR/AND (no game in the collection sets them) are not done. */
    }
}

/* ---- files ------------------------------------------------------------ */

s256 *s256_new(const uint8_t *rom48, const uint8_t *rom128_0, const uint8_t *rom128_1)
{
    s256 *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (rom48) {
        memcpy(s->rom48, rom48, PAGE);
        s->have_rom48 = 1;
    }
    if (rom128_0 && rom128_1) {
        memcpy(s->rom128[0], rom128_0, PAGE);
        memcpy(s->rom128[1], rom128_1, PAGE);
        s->have_rom128 = 1;
    }
    memcpy(s->pal, s256_default_palette, sizeof s->pal);
    defaults(s);
    s->bkg = -1;
    return s;
}

void s256_clear_files(s256 *s)
{
    int i;
    free(s->gfx);
    s->gfx = NULL;
    for (i = 0; i < 8; i++) {
        free(s->gfn[i]);
        s->gfn[i] = NULL;
    }
    for (i = 0; i < 2; i++) {
        free(s->gfr[i]);
        s->gfr[i] = NULL;
    }
    for (i = 0; i < MAX_BKG; i++) {
        free(s->bkgs[i]);
        s->bkgs[i] = NULL;
    }
    memcpy(s->pal, s256_default_palette, sizeof s->pal);
    defaults(s);
    s->bkg = -1;
}

void s256_free(s256 *s)
{
    if (!s)
        return;
    s256_clear_files(s);
    free(s);
}

int s256_snapshot_is_128(size_t len)
{
    return len > 49179;
}

void s256_roms(const s256 *s, int is128, const uint8_t *roms[4])
{
    roms[0] = roms[1] = roms[2] = roms[3] = NULL;
    if (is128) {
        roms[0] = s->have_rom128 ? s->rom128[0] : NULL;
        roms[1] = s->have_rom128 ? s->rom128[1] : NULL;
    } else {
        roms[0] = s->have_rom48 ? s->rom48 : NULL;
    }
}

static uint8_t *keep(uint8_t **slot, const uint8_t *data, size_t len, size_t want)
{
    uint8_t *p = malloc(want);
    if (!p)
        return NULL;
    memset(p, 0, want);
    memcpy(p, data, len < want ? len : want);
    free(*slot);
    *slot = p;
    return p;
}

/* The extension after the last dot, lower case; empty if over 4 characters. */
static void extension(const char *name, char ext[5])
{
    const char *dot = strrchr(name, '.');
    int i = 0;
    if (dot)
        for (dot++; *dot && i < 4; dot++)
            ext[i++] = (char)lower(*dot);
    ext[dot && *dot ? 0 : i] = 0;
}

static const char *base_name(const char *name)
{
    const char *b = name, *c;
    for (c = name; *c; c++)
        if (*c == '/' || *c == '\\')
            b = c + 1;
    return b;
}

int s256_add_file(s256 *s, const char *name, const uint8_t *data, size_t len)
{
    char ext[5];
    const char *b = base_name(name);
    extension(b, ext);
    if (!len)
        return 0;                  /* some games ship empty placeholders */
    if (!strcmp(ext, "gfx")) {
        if (name_eq(b, "rom0.gfx"))      /* the 48K BASIC ROM */
            return keep(&s->gfr[1], data, len, GFX_PAGE) ? 0 : -1;
        if (name_eq(b, "rom1.gfx"))
            return keep(&s->gfr[0], data, len, GFX_PAGE) ? 0 : -1;
        return keep(&s->gfx, data, len, GFX_PAGE * 3) ? 0 : -1;
    }
    if (ext[0] == 'g' && ext[1] == 'f' && ext[3] == 0) {
        if (ext[2] >= '0' && ext[2] <= '7')
            return keep(&s->gfn[ext[2] - '0'], data, len, GFX_PAGE) ? 0 : -1;
        if (ext[2] == 'a' || ext[2] == 'b')
            return keep(&s->gfr[ext[2] - 'a'], data, len, GFX_PAGE) ? 0 : -1;
    }
    if (ext[0] == 'b' && ext[1] >= '0' && ext[1] <= '9' && ext[2] >= '0' && ext[2] <= '9' && !ext[3])
        return keep(&s->bkgs[(ext[1] - '0') * 10 + ext[2] - '0'], data, len, 320 * 200) ? 0 : -1;
    if (!strcmp(ext, "cfg")) {
        parse_cfg(s, (const char *)data, len);
        return 0;
    }
    if (!strcmp(ext, "pal") && len >= 768) {
        memcpy(s->pal, data, 768);
        return 0;
    }
    return -1;
}

void s256_attach(s256 *s, zx *m)
{
    s->m = m;
    s->inner = m->bus;
    m->bus.ctx = s;
    m->bus.fetch = m_fetch;
    m->bus.read = m_read;
    m->bus.write = m_write;
    m->bus.io_read = m_io_read;
    m->bus.io_write = m_io_write;
    m->bus.internal = m_internal;
    m->bus.tick = m_tick;
    m->bus.int_ack = m_int_ack;
    memset(&s->shadow, 0, sizeof s->shadow);
    s->shadow.ctx = s;
    s->shadow.fetch = g_fetch;
    s->shadow.read = g_read;
    s->shadow.write = g_write;
    s->shadow.io_read = g_io_read;
    s->shadow.io_write = g_io_write;
    s->shadow.internal = g_internal;
    s->shadow.tick = g_tick;
    s->shadow.int_ack = g_int_ack;
    m->s256 = s;
}

/* The shadow CPUs share the real CPU's address bus (they are the rest of
 * a 72-bit data bus), so with PC, SP and the flags but carry aligned they
 * follow it exactly. A .CFG may align more (ZX-Poly's zxpAlignRegs). */
static void apply_settings(s256 *s)
{
    /* Games that want colour 0 and 0xFF drawn as they are, not as the
     * attribute's paper and ink (ZX-Poly's spec256appbase.txt). */
    static const char *const plain[] = { "booty", "cybernoid_ii256", "eaglesns", "rene256" };
    size_t i;
    s->align = parse_align(s->cfg_set ? s->cfg_align : "1PSs");
    for (i = 0; i < sizeof plain / sizeof plain[0]; i++)
        if (s->paper00inkff < 0 && name_eq(plain[i], s->snap_name))
            s->paper00inkff = 0;
}

void s256_set_name(s256 *s, const char *snapshot_file)
{
    const char *b = base_name(snapshot_file), *dot = strrchr(b, '.');
    size_t n = dot ? (size_t)(dot - b) : strlen(b);
    if (n >= sizeof s->snap_name)
        n = sizeof s->snap_name - 1;
    memcpy(s->snap_name, b, n);
    s->snap_name[n] = 0;
}

void s256_start(s256 *s)
{
    zx *m = s->m;
    int i, top = m->model == ZX_48K ? 0 : (m->port_7ffd & 7);
    int is48 = m->model == ZX_48K;

    for (i = 0; i < 8; i++) {
        if (s->gfn[i])
            from_file(s->ram[i], s->gfn[i]);
        else if (s->gfx && i == 5)
            from_file(s->ram[i], s->gfx);
        else if (s->gfx && i == 2)
            from_file(s->ram[i], s->gfx + GFX_PAGE);
        else if (s->gfx && i == top)
            from_file(s->ram[i], s->gfx + GFX_PAGE * 2);
        else
            expand(s->ram[i], m->ram[i]);
    }
    for (i = 0; i < 4; i++)
        expand(s->rom[i], m->rom[i]);
    /* .GFA is ROM 0 of a 128K, .GFB (or ROM0.GFX) the 48K BASIC ROM. */
    if (is48) {
        if (s->gfr[1])
            from_file(s->rom[0], s->gfr[1]);
    } else {
        if (s->gfr[0])
            from_file(s->rom[0], s->gfr[0]);
        if (s->gfr[1])
            from_file(s->rom[1], s->gfr[1]);
    }

    apply_settings(s);
    for (i = 0; i < 256; i++)
        s->rgb[i] = to333(s->pal[i * 3], s->pal[i * 3 + 1], s->pal[i * 3 + 2]);
    s->bkg = -1;
    for (i = 0; i < MAX_BKG; i++)
        if (s->bkgs[i]) {
            s->bkg = i;
            break;
        }
    make_bkg_rgb(s);
    for (i = 0; i < CORES; i++)
        s->core[i] = m->cpu;
}

void s256_trace(s256 *s, s256_trace_fn fn, void *user)
{
    int slot, i;
    if (!fn && s->trace) {
        /* Tracing off: planes back to plain copies of RAM, tags gone. */
        for (slot = 0; slot < 4; slot++) {
            uint8_t *pl = planes_of(s, s->m->rd[slot]);
            if (pl)
                expand(pl, s->m->rd[slot]);
        }
    }
    s->trace = fn;
    s->trace_user = user;
    if (!fn)
        return;
    for (slot = 0; slot < 4; slot++) {
        uint8_t *pl = planes_of(s, s->m->rd[slot]);
        if (!pl)
            continue;
        for (i = 0; i < PAGE; i++) {
            unsigned a = (unsigned)slot * PAGE + (unsigned)i;
            int blank = a >= 0x4000 && a < 0x5800;
            pl[i * 8] = blank ? 0 : (uint8_t)a;
            pl[i * 8 + 1] = blank ? 0 : (uint8_t)(a >> 8);
            pl[i * 8 + 2] = blank ? 0 : (uint8_t)~a;
            pl[i * 8 + 3] = blank ? 0 : (uint8_t)(~a >> 8);
        }
    }
}

/* ---- state ------------------------------------------------------------ */

size_t s256_state_size(const s256 *s)
{
    (void)s;
    return sizeof s->ram + sizeof s->core + sizeof s->bkg;
}

void s256_state_save(const s256 *s, uint8_t *p)
{
    memcpy(p, s->ram, sizeof s->ram);
    p += sizeof s->ram;
    memcpy(p, s->core, sizeof s->core);
    p += sizeof s->core;
    memcpy(p, &s->bkg, sizeof s->bkg);
}

void s256_state_load(s256 *s, const uint8_t *p)
{
    memcpy(s->ram, p, sizeof s->ram);
    p += sizeof s->ram;
    memcpy(s->core, p, sizeof s->core);
    p += sizeof s->core;
    memcpy(&s->bkg, p, sizeof s->bkg);
    if (s->bkg >= MAX_BKG || (s->bkg >= 0 && !s->bkgs[s->bkg]))
        s->bkg = -1;
    make_bkg_rgb(s);
}
