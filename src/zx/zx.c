#include "zx.h"
#include "tape.h"
#include "expansion.h"
#include "../next/next.h"
#include "../spec256/spec256.h"
#include "../storage/blockdev.h"

#include <stdlib.h>
#include <string.h>

#define NOCONT 0xffffffffu

const zx_timing zx_timings[ZX_MODEL_COUNT] = {
    /* name        frame  line int paper  contend  pattern                    io int cpu      ay */
    { "48K",       69888, 224, 32, 14336, 14335, { 6, 5, 4, 3, 2, 1, 0, 0 }, 1, 1, 3500000, 0 },
    { "128K",      70908, 228, 36, 14364, 14361, { 6, 5, 4, 3, 2, 1, 0, 0 }, 1, 1, 3546900, 1773450 },
    { "+2",        70908, 228, 36, 14364, 14361, { 6, 5, 4, 3, 2, 1, 0, 0 }, 1, 1, 3546900, 1773450 },
    { "+2A",       70908, 228, 32, 14364, 14365, { 1, 0, 7, 6, 5, 4, 3, 2 }, 0, 0, 3546900, 1773450 },
    { "+3",        70908, 228, 32, 14364, 14365, { 1, 0, 7, 6, 5, 4, 3, 2 }, 0, 0, 3546900, 1773450 },
    { "Pentagon",  71680, 224, 32, 17988, NOCONT, { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0, 3500000, 1750000 },
    /* The Next keeps its own timing (src/next); this row names it and gives
     * the 3.5 MHz frame for pacing. */
    { "Next",      69888, 224, 32, 14336, NOCONT, { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0, 3500000, 1750000 },
    /* Scorpion: libspectrum's table (312 lines, INT 36 T long). */
    { "Scorpion",  69888, 224, 36, 14336, NOCONT, { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0, 3500000, 1750000 },
    /* ATM Turbo 2+: Unreal Speccy's ATM preset; 7 MHz turbo runs two CPU
     * T-states to each of these. */
    { "ATM Turbo 2+", 69888, 224, 32, 17989, NOCONT, { 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0, 3500000, 1750000 },
};

static const struct { const char *name; zx_model model; } model_names[] = {
    { "48", ZX_48K }, { "48k", ZX_48K }, { "128", ZX_128K }, { "128k", ZX_128K },
    { "plus2", ZX_PLUS2 }, { "+2", ZX_PLUS2 }, { "plus2a", ZX_PLUS2A }, { "+2a", ZX_PLUS2A },
    { "plus3", ZX_PLUS3 }, { "+3", ZX_PLUS3 }, { "pentagon", ZX_PENTAGON },
    { "next", ZX_NEXT }, { "tbblue", ZX_NEXT },
    { "scorpion", ZX_SCORPION }, { "zs256", ZX_SCORPION },
    { "atm2", ZX_ATM2 }, { "atm", ZX_ATM2 },
};

int zx_model_from_name(const char *name, zx_model *out)
{
    size_t i;
    for (i = 0; i < sizeof model_names / sizeof model_names[0]; i++) {
        if (!strcmp(name, model_names[i].name)) {
            *out = model_names[i].model;
            return 0;
        }
    }
    return -1;
}

static int is_plus3(const zx *m) { return m->model == ZX_PLUS2A || m->model == ZX_PLUS3; }
static int has_ay(const zx *m) { return m->model != ZX_48K; }
/* An interface's TurboSound takes over the AY ports. */
static int own_ay(const zx *m) { return has_ay(m) && !(m->exp && (m->exp->features & ZX_EXP_TURBOSOUND)); }
static int is_atm(const zx *m) { return m->model == ZX_ATM2; }

/* Absolute time in T-states, for the disk controller. */
static inline uint64_t now_t(const zx *m)
{
    return m->frames * m->tm->frame_t + m->t;
}

/* The TR-DOS ROM and the Beta ports are on: paged in by the 3Dxx trap, or
 * forced by the ATM's CP/M bit. */
static inline int dos_on(const zx *m)
{
    return m->dos || (is_atm(m) && !(m->atm.p77_hi & 2));
}

/* ---- contention -------------------------------------------------------- */

static inline uint32_t contention(const zx *m, uint32_t t)
{
    const zx_timing *tm = m->tm;
    uint32_t d, x;
    if (t < tm->contend_t)
        return 0;
    d = t - tm->contend_t;
    if (d >= 192 * tm->line_t)
        return 0;
    x = d % tm->line_t;
    if (x >= 128)
        return 0;
    return tm->pattern[x & 7];
}

/* ---- video ------------------------------------------------------------- */

#define CHUNKS_PER_LINE (ZX_SCREEN_W / 8)
#define CHUNKS (CHUNKS_PER_LINE * ZX_SCREEN_H)

static const uint16_t zx_colour[16] = {
    /* RGB333: normal intensity 5, bright 7. Index = BRIGHT<<3 | GRB */
    0x000, 0x005, 0x140, 0x145, 0x028, 0x02d, 0x168, 0x16d,
    0x000, 0x007, 0x1c0, 0x1c7, 0x038, 0x03f, 0x1f8, 0x1ff,
};

/* ULA+ palette entry (GGGRRRBB) to RGB333; the missing low blue bit is the
 * OR of the other two, as the ULAplus specification says. */
static inline uint16_t ulaplus_colour(uint8_t v)
{
    uint16_t g = v >> 5, r = (v >> 2) & 7, b = v & 3;
    return (uint16_t)(r << 6 | g << 3 | b << 1 | ((b >> 1) | (b & 1)));
}

static inline int32_t chunk_t(const zx *m, uint32_t c)
{
    int32_t y = (int32_t)(c / CHUNKS_PER_LINE), cx = (int32_t)(c % CHUNKS_PER_LINE);
    return (int32_t)m->tm->paper_t + (y - ZX_BORDER_Y) * (int32_t)m->tm->line_t + cx * 4 - ZX_BORDER_X / 2;
}

static void draw_chunk(zx *m, uint32_t c)
{
    int y = (int)(c / CHUNKS_PER_LINE), cx = (int)(c % CHUNKS_PER_LINE), i;
    uint16_t *px = &m->screen[y][cx * 8];
    int row = y - ZX_BORDER_Y, col = cx - ZX_BORDER_X / 8;

    if (row >= 0 && row < 192 && col >= 0 && col < 32) {
        const uint8_t *scr = m->ram[m->screen_page];
        uint32_t po = ((row & 0xc0) << 5) | ((row & 7) << 8) | ((row & 0x38) << 2) | col;
        uint32_t ao = 6144 + (row >> 3) * 32 + col;
        uint8_t bits, attr;
        uint16_t ink, paper;
        if (m->exp)
            zx_exp_screen(m->exp, &po, &ao);
        bits = scr[po];
        attr = scr[ao];
        if (m->ulaplus_mode & 1) {
            /* ULA+: FLASH and BRIGHT pick one of four 16-colour CLUTs. */
            const uint8_t *clut = &m->ulaplus_pal[(attr >> 6) * 16];
            ink = ulaplus_colour(clut[attr & 7]);
            paper = ulaplus_colour(clut[8 + ((attr >> 3) & 7)]);
        } else {
            ink = zx_colour[(attr & 7) | ((attr & 0x40) >> 3)];
            paper = zx_colour[((attr >> 3) & 7) | ((attr & 0x40) >> 3)];
            if ((attr & 0x80) && (m->frames & 16))
                bits = (uint8_t)~bits;
        }
        for (i = 0; i < 8; i++)
            px[i] = (bits & (0x80 >> i)) ? ink : paper;
    } else {
        uint16_t b = (m->ulaplus_mode & 1) ? ulaplus_colour(m->ulaplus_pal[8 + m->border])
                                            : zx_colour[m->border];
        for (i = 0; i < 8; i++)
            px[i] = b;
    }
}

/* ---- ATM Turbo 2+ video ------------------------------------------------------
 *
 * The picture is 640 wide (each classic pixel is two). Mode 3 is the
 * Spectrum screen; modes 0 (320x200, 16 colours), 2 (640x200 with an
 * attribute per 8 pixels) and 6 (80x25 text) show a 320x200 raster that
 * starts 4 lines above and 32 pixels left of the Spectrum paper, filling
 * the width. They read the screen page (5 or 7) and the page 4 below it.
 * Every colour goes through the 16-entry palette. */

#define ATM_W (ZX_SCREEN_W * 2)

static inline uint16_t atm_ink(const zx *m, uint8_t c)
{
    return m->atm.pal[((c & 0x40) >> 3) | (c & 7)];
}

static inline uint16_t atm_paper(const zx *m, uint8_t c)
{
    return m->atm.pal[((c & 0x80) >> 4) | ((c >> 3) & 7)];
}

/* Text mode characters: the 48K ROM's character set (the ATM's own
 * character generator is not emulated); codes outside 32..127 are blank. */
static inline uint8_t atm_font(const zx *m, uint8_t ch, int line)
{
    return ch >= 32 && ch < 128 ? m->rom[0][0x3d00 + (ch - 32) * 8 + line] : 0;
}

static void draw_chunk_atm(zx *m, uint32_t c)
{
    int y = (int)(c / CHUNKS_PER_LINE), cx = (int)(c % CHUNKS_PER_LINE), i;
    uint16_t *px = m->wide + (size_t)y * ATM_W + (size_t)cx * 16;
    int mode = m->atm.p77 & 7, yy = y - (ZX_BORDER_Y - 4);
    const uint8_t *hi = m->ram[m->screen_page], *lo = m->ram[m->screen_page - 4];

    if (mode == 3) {
        int row = y - ZX_BORDER_Y, col = cx - ZX_BORDER_X / 8;
        if (row >= 0 && row < 192 && col >= 0 && col < 32) {
            uint8_t bits = hi[((row & 0xc0) << 5) | ((row & 7) << 8) | ((row & 0x38) << 2) | col];
            uint8_t attr = hi[6144 + (row >> 3) * 32 + col];
            uint16_t ink = m->atm.pal[(attr & 7) | ((attr & 0x40) >> 3)];
            uint16_t paper = m->atm.pal[((attr >> 3) & 7) | ((attr & 0x40) >> 3)];
            if ((attr & 0x80) && (m->frames & 16))
                bits = (uint8_t)~bits;
            for (i = 0; i < 8; i++)
                px[2 * i] = px[2 * i + 1] = (bits & (0x80 >> i)) ? ink : paper;
            return;
        }
    } else if (yy >= 0 && yy < 200 && (mode == 0 || mode == 2 || mode == 6)) {
        uint32_t g = (uint32_t)yy * 40 + (uint32_t)cx;
        if (mode == 0) {
            const uint8_t b[4] = { lo[g], hi[g], lo[g + 0x2000], hi[g + 0x2000] };
            for (i = 0; i < 4; i++) {
                px[4 * i] = px[4 * i + 1] = atm_ink(m, b[i]);
                px[4 * i + 2] = px[4 * i + 3] = atm_paper(m, b[i]);
            }
        } else {
            uint8_t bits[2], attr[2];
            int h;
            if (mode == 2) {
                bits[0] = hi[g]; attr[0] = lo[g];
                bits[1] = hi[g + 0x2000]; attr[1] = lo[g + 0x2000];
            } else {
                uint32_t a = 0x1c0 + (uint32_t)(yy >> 3) * 64 + (uint32_t)cx;
                bits[0] = atm_font(m, hi[a], yy & 7); attr[0] = lo[a + 0x2000];
                bits[1] = atm_font(m, hi[a + 0x2000], yy & 7); attr[1] = lo[a + 1];
            }
            for (h = 0; h < 2; h++) {
                uint16_t ink = atm_ink(m, attr[h]), paper = atm_paper(m, attr[h]);
                for (i = 0; i < 8; i++)
                    px[8 * h + i] = (bits[h] & (0x80 >> i)) ? ink : paper;
            }
        }
        return;
    }
    for (i = 0; i < 16; i++)
        px[i] = m->atm.pal[m->border & 15];
}

/* The ATM palette register: inverted, two bits a gun, grbG--RB. */
static uint16_t atm_colour(uint8_t raw)
{
    static const uint8_t level[4] = { 0, 2, 5, 7 };
    uint8_t v = (uint8_t)~raw;
    int r = ((v >> 1) & 1) << 1 | ((v >> 6) & 1);
    int g = ((v >> 4) & 1) << 1 | ((v >> 7) & 1);
    int b = (v & 1) << 1 | ((v >> 5) & 1);
    return (uint16_t)(level[r] << 6 | level[g] << 3 | level[b]);
}

static void video_update(zx *m, uint32_t upto)
{
    while (m->video_chunk < CHUNKS && chunk_t(m, m->video_chunk) <= (int32_t)upto) {
        if (m->wide)
            draw_chunk_atm(m, m->video_chunk);
        else
            draw_chunk(m, m->video_chunk);
        m->video_chunk++;
    }
}

/* Byte the ULA is fetching at time t, or 0xff when it is idle. */
static uint8_t floating_bus(const zx *m, uint32_t t)
{
    const uint8_t *scr = m->ram[m->screen_page];
    uint32_t d, line, x, col, row;
    if (t < m->tm->paper_t)
        return 0xff;
    d = t - m->tm->paper_t;
    line = d / m->tm->line_t;
    x = d % m->tm->line_t;
    if (line >= 192 || x >= 128)
        return 0xff;
    col = (x / 8) * 2;
    row = line;
    switch (x & 7) {
    case 2: col++; /* fall through */
    case 0: return scr[((row & 0xc0) << 5) | ((row & 7) << 8) | ((row & 0x38) << 2) | col];
    case 3: col++; /* fall through */
    case 1: return scr[6144 + (row >> 3) * 32 + col];
    default: return 0xff;
    }
}

/* ---- audio ------------------------------------------------------------- */

#define BEEP_EAR 12000
#define BEEP_MIC 1500

static void audio_accumulate(zx *m, uint32_t dt)
{
    int32_t level = (m->speaker ? BEEP_EAR : 0) + (m->mic ? BEEP_MIC : 0) + (m->ear_in ? BEEP_MIC : 0);
    m->beeper_acc += level * (int32_t)dt;
    if (has_ay(m)) {
        /* The AY is clocked at CPU/2. */
        m->ay_half += dt;
        ay_run(&m->ay, m->ay_half >> 1);
        m->ay_half &= 1;
    }
    if (m->exp)
        zx_exp_audio_run(m, dt);
}

static void audio_update(zx *m, uint32_t upto)
{
    const uint64_t step = ((uint64_t)m->tm->cpu_hz << 32) / ZX_AUDIO_RATE;
    for (;;) {
        uint32_t next = (uint32_t)(m->audio_frac >> 32);
        uint32_t span = (uint32_t)(step >> 32);
        int32_t ay[3] = { 0, 0, 0 }, beep, l, r;
        if (next > upto) {
            if (upto > m->audio_t) {
                audio_accumulate(m, upto - m->audio_t);
                m->audio_t = upto;
            }
            return;
        }
        if (next > m->audio_t)
            audio_accumulate(m, next - m->audio_t);
        m->audio_t = next;
        m->audio_frac += step;

        beep = m->beeper_acc / (int32_t)span;
        m->beeper_acc = 0;
        if (has_ay(m))
            ay_take(&m->ay, ay);
        /* ACB stereo, as on the Next and most 128K add-ons. */
        l = beep / 2 + (ay[0] + ay[1] / 2) / 4;
        r = beep / 2 + (ay[2] + ay[1] / 2) / 4;
        if (m->exp)
            zx_exp_audio_take(m, span, &l, &r);
        if (m->audio_samples < (int)(sizeof m->audio / sizeof m->audio[0] / 2)) {
            m->audio[m->audio_samples * 2] = (int16_t)(l > 32767 ? 32767 : l);
            m->audio[m->audio_samples * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r);
            m->audio_samples++;
        }
    }
}

/* ---- paging ------------------------------------------------------------ */

static void update_paging(zx *m)
{
    int i, rom, page;
    static const int special[4][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 4, 5, 6, 3 }, { 4, 7, 6, 3 } };

    switch (m->model) {
    case ZX_48K:
        m->rd[0] = m->rom[0]; m->wr[0] = NULL;
        m->rd[1] = m->wr[1] = m->ram[5];
        m->rd[2] = m->wr[2] = m->ram[2];
        m->rd[3] = m->wr[3] = m->ram[0];
        m->screen_page = 5;
        m->contended[0] = 0; m->contended[1] = 1; m->contended[2] = 0; m->contended[3] = 0;
        return;
    case ZX_PLUS2A:
    case ZX_PLUS3:
        if (m->port_1ffd & 1) {
            const int *s = special[(m->port_1ffd >> 1) & 3];
            for (i = 0; i < 4; i++) {
                m->rd[i] = m->wr[i] = m->ram[s[i]];
                m->contended[i] = s[i] >= 4;
            }
        } else {
            rom = ((m->port_1ffd >> 1) & 2) | ((m->port_7ffd >> 4) & 1);
            page = m->port_7ffd & 7;
            m->rd[0] = m->rom[rom]; m->wr[0] = NULL;
            m->rd[1] = m->wr[1] = m->ram[5];
            m->rd[2] = m->wr[2] = m->ram[2];
            m->rd[3] = m->wr[3] = m->ram[page];
            m->contended[0] = 0; m->contended[1] = 1; m->contended[2] = 0;
            m->contended[3] = page >= 4;
        }
        break;
    case ZX_SCORPION:
        /* 1FFD: bit 0 RAM 0 at 0000, bit 1 the service monitor ROM, bit 4
         * the upper 128K. ROMs: 0 128K, 1 48K, 2 monitor, 3 TR-DOS. */
        if (m->port_1ffd & 1) {
            m->rd[0] = m->wr[0] = m->ram[0];
        } else {
            rom = (m->port_1ffd & 2) ? 2 : m->dos ? 3 : (m->port_7ffd >> 4) & 1;
            m->rd[0] = m->rom[rom]; m->wr[0] = NULL;
        }
        page = (m->port_7ffd & 7) | ((m->port_1ffd & 0x10) >> 1);
        m->rd[1] = m->wr[1] = m->ram[5];
        m->rd[2] = m->wr[2] = m->ram[2];
        m->rd[3] = m->wr[3] = m->ram[page];
        for (i = 0; i < 4; i++)
            m->contended[i] = 0;
        break;
    case ZX_ATM2:
        /* Each window takes a page from #xxF7 (two sets of four, chosen by
         * 7FFD bit 4), written inverted: bit 6 RAM, bit 7 mixes in 7FFD's
         * bank (RAM) or the TR-DOS line (ROM). Until #xx77 sets pen
         * (A8), every window shows the last ROM page. ROMs: 0 48K, 1
         * TR-DOS, 2 128K, 3 the ATM's own. */
        for (i = 0; i < 4; i++) {
            uint8_t v = m->atm.page[((m->port_7ffd & 0x10) >> 2) | i];
            int pg = ~v & 0x3f;
            m->contended[i] = 0;
            if (!(m->atm.p77_hi & 1)) {
                m->rd[i] = m->rom[3]; m->wr[i] = NULL;
            } else if (v & 0x40) {
                if (v & 0x80)
                    pg = (pg & 0x38) | (m->port_7ffd & 7);
                m->rd[i] = m->wr[i] = m->ram[pg & (m->ram_pages - 1)];
            } else {
                if (v & 0x80)
                    pg = (pg & 0x3e) | dos_on(m);
                m->rd[i] = m->rom[pg & 3]; m->wr[i] = NULL;
            }
        }
        break;
    default: /* 128K, +2, Pentagon */
        rom = (m->model == ZX_PENTAGON && m->dos) ? 2 : (m->port_7ffd >> 4) & 1;
        page = m->port_7ffd & 7;
        m->rd[0] = m->rom[rom]; m->wr[0] = NULL;
        m->rd[1] = m->wr[1] = m->ram[5];
        m->rd[2] = m->wr[2] = m->ram[2];
        m->rd[3] = m->wr[3] = m->ram[page];
        m->contended[0] = 0;
        m->contended[1] = m->model != ZX_PENTAGON;
        m->contended[2] = 0;
        m->contended[3] = m->model != ZX_PENTAGON && (page & 1);
        break;
    }
    m->screen_page = (m->port_7ffd & 8) ? 7 : 5;
}

/* The ROM holding the 48K BASIC tape routines. */
static int tape_rom(const zx *m)
{
    switch (m->model) {
    case ZX_48K: case ZX_ATM2: return 0;
    case ZX_PLUS2A: case ZX_PLUS3: return 3;
    default: return 1;
    }
}

/* ---- bus --------------------------------------------------------------- */

static inline uint8_t mem_read(const zx *m, uint16_t addr)
{
    if (addr < 0x4000 && m->div_map)
        return zx_exp_read(m, addr);
    return m->rd[addr >> 14][addr & 0x3fff];
}

static inline void mem_write(zx *m, uint16_t addr, uint8_t v)
{
    uint8_t *p = m->wr[addr >> 14];
    if (addr < 0x4000 && m->div_map) {
        zx_exp_write(m, addr, v);
        return;
    }
    if (!p)
        return;
    if (p == m->ram[m->screen_page] && (addr & 0x3fff) < 6912)
        video_update(m, m->t);
    p[addr & 0x3fff] = v;
}

/* TR-DOS comes in when the CPU fetches from 3Dxx in the 48K BASIC ROM, and
 * goes when it fetches from RAM. */
static void dos_trap(zx *m, uint16_t addr)
{
    if (m->dos) {
        if (m->wr[addr >> 14]) {
            m->dos = 0;
            update_paging(m);
        }
    } else if ((addr & 0xff00) == 0x3d00
               && (is_atm(m) ? !m->wr[0] && (m->port_7ffd & 0x10) : m->rd[0] == m->rom[1])) {
        m->dos = 1;
        update_paging(m);
    }
}

static uint8_t b_fetch(void *ctx, uint16_t addr)
{
    zx *m = ctx;
    if (m->has_beta)
        dos_trap(m, addr);
    if (m->model == ZX_SCORPION)
        m->t = (m->t + 1) & ~1u;         /* M1 starts on an even T-state */
    if (m->contended[addr >> 14])
        m->t += contention(m, m->t);
    m->t += 4;
    if (m->exp)
        return zx_exp_fetch(m, addr);
    return m->rd[addr >> 14][addr & 0x3fff];
}

static uint8_t b_read(void *ctx, uint16_t addr)
{
    zx *m = ctx;
    if (m->contended[addr >> 14])
        m->t += contention(m, m->t);
    m->t += 3;
    return mem_read(m, addr);
}

static void b_write(void *ctx, uint16_t addr, uint8_t v)
{
    zx *m = ctx;
    if (m->contended[addr >> 14])
        m->t += contention(m, m->t);
    m->t += 3;
    mem_write(m, addr, v);
}

static void b_internal(void *ctx, uint16_t addr, unsigned n)
{
    zx *m = ctx;
    if (m->tm->contend_internal && m->contended[addr >> 14]) {
        while (n--)
            m->t += contention(m, m->t) + 1;
    } else {
        m->t += n;
    }
}

static void b_tick(void *ctx, unsigned n)
{
    zx *m = ctx;
    m->t += n;
}

/* I/O cycle timing, including ULA contention where the model has it. */
static void io_timing(zx *m, uint16_t port)
{
    if (!m->tm->contend_io) {
        m->t += 4;
        return;
    }
    if (m->contended[port >> 14]) {
        if (port & 1) {
            int i;
            for (i = 0; i < 4; i++)
                m->t += contention(m, m->t) + 1;
        } else {
            m->t += contention(m, m->t) + 1;
            m->t += contention(m, m->t) + 3;
        }
    } else if (port & 1) {
        m->t += 4;
    } else {
        m->t += 1;
        m->t += contention(m, m->t) + 3;
    }
}

static uint8_t b_io_read(void *ctx, uint16_t port)
{
    zx *m = ctx;
    uint8_t v = 0xff;
    int i, have = 0;

    io_timing(m, port);

    if (dos_on(m) && m->has_beta && beta_port((uint8_t)port))
        return beta_read(&m->beta, (uint8_t)port, now_t(m));
    if (!(port & 1)) {
        uint8_t k = 0x1f;
        for (i = 0; i < 8; i++)
            if (!(port & (0x100 << i)))
                k &= m->keys[i];
        if (m->tape) {
            if (m->cpu.pc >= 0x4000)
                zx_tape_custom_loader(m->tape);
            if (zx_tape_playing(m->tape))
                zx_tape_advance(m->tape, m, m->frames * m->tm->frame_t + m->t);
        }
        v = (uint8_t)(0xa0 | k);
        if ((m->tape && zx_tape_playing(m->tape)) ? m->ear_in : (m->speaker && !is_plus3(m)))
            v |= 0x40;
        return v;
    }
    if (port == 0xff3b) {           /* ULA+ data */
        return (m->ulaplus_reg & 0xc0) == 0x40 ? m->ulaplus_mode
                                                : m->ulaplus_pal[m->ulaplus_reg & 63];
    }
    if (m->exp && zx_exp_io_read(m, port, &v))
        return v;
    if ((port & 0xe0) == 0) { /* Kempston on port 0x1F */
        v = m->kempston;
        /* The Scorpion's joystick port also shows the Beta's INTRQ and
         * DRQ (bits 7, 6); its service ROM waits on them there. */
        if (m->model == ZX_SCORPION)
            v = (uint8_t)((v & 0x1f) | (beta_read(&m->beta, 0xff, now_t(m)) & 0xe0));
        have = 1;
    }
    if (own_ay(m) && (port & (is_atm(m) ? 0xc202 : 0xc002)) == (is_atm(m) ? 0xc200 : 0xc000)) {
        v = ay_read(&m->ay);
        have = 1;
    }
    if (!have) {
        if (is_plus3(m))
            v = 0xff;
        else
            v = floating_bus(m, m->t - 1);
    }
    return v;
}

static void atm_io_write(zx *m, uint16_t port, uint8_t v);

static void b_io_write(void *ctx, uint16_t port, uint8_t v)
{
    zx *m = ctx;
    io_timing(m, port);

    if (!(port & 1)) {
        video_update(m, m->t);
        audio_update(m, m->t);
        m->border = v & 7;
        if (is_atm(m) && !(port & 8))    /* A3 low: bright border */
            m->border |= 8;
        m->speaker = (v >> 4) & 1;
        m->mic = (v >> 3) & 1;
    }
    if (m->exp) {
        video_update(m, m->t);
        audio_update(m, m->t);
        zx_exp_io_write(m, port, v);
    }
    if (is_atm(m)) {
        atm_io_write(m, port, v);
        return;
    }
    if (dos_on(m) && m->has_beta && beta_port((uint8_t)port)) {
        beta_write(&m->beta, (uint8_t)port, v, now_t(m));
        return;
    }
    if (port == 0xbf3b) {           /* ULA+ register select */
        m->ulaplus_reg = v;
        return;
    }
    if (port == 0xff3b) {           /* ULA+ data */
        video_update(m, m->t);
        if ((m->ulaplus_reg & 0xc0) == 0)
            m->ulaplus_pal[m->ulaplus_reg & 63] = v;
        else if ((m->ulaplus_reg & 0xc0) == 0x40)
            m->ulaplus_mode = v;
        return;
    }
    if (m->model == ZX_48K)
        return;

    if (m->model == ZX_SCORPION) {
        if ((port & 0xc023) == 0x4021 && !m->paging_locked) {
            video_update(m, m->t);
            m->port_7ffd = v;
            m->paging_locked = (v >> 5) & 1;
            update_paging(m);
        } else if ((port & 0xc023) == 0x0021) {
            m->port_1ffd = v;
            update_paging(m);
        }
    } else if (is_plus3(m)) {
        if ((port & 0xc002) == 0x4000 && !m->paging_locked) {
            video_update(m, m->t);
            m->port_7ffd = v;
            m->paging_locked = (v >> 5) & 1;
            update_paging(m);
        } else if ((port & 0xf002) == 0x1000 && !m->paging_locked) {
            m->port_1ffd = v;
            update_paging(m);
        }
    } else if ((port & 0x8002) == 0 && !m->paging_locked) {
        video_update(m, m->t);
        m->port_7ffd = v;
        m->paging_locked = (v >> 5) & 1;
        update_paging(m);
    }

    if (!own_ay(m)) {
        return;
    } else if ((port & 0xc002) == 0xc000) {
        m->ay.sel = v;
    } else if ((port & 0xc002) == 0x8000) {
        audio_update(m, m->t);
        ay_write(&m->ay, v);
    }
}

/* ATM Turbo 2+ ports. 7FFD and the AY decode A15, A9 and A1. With TR-DOS
 * on: #xx77 the system port (its address lines too), #xxF7 the window
 * pages, #FF (and its A5/A6 aliases) the palette as well as the Beta. */
static void atm_io_write(zx *m, uint16_t port, uint8_t v)
{
    uint8_t lo = (uint8_t)port;
    if ((port & 0x8202) == 0x0200 && !m->paging_locked) {
        video_update(m, m->t);
        m->port_7ffd = v;
        m->paging_locked = (v >> 5) & 1;
        update_paging(m);
    }
    if ((port & 0xc202) == 0xc200) {
        m->ay.sel = v;
    } else if ((port & 0xc202) == 0x8200) {
        audio_update(m, m->t);
        ay_write(&m->ay, v);
    }
    if (!dos_on(m))
        return;
    if (lo == 0x77) {
        video_update(m, m->t);
        m->atm.p77 = v;
        m->atm.p77_hi = (uint8_t)(port >> 8);
        update_paging(m);
        return;
    }
    if (lo == 0xf7) {
        m->atm.page[((m->port_7ffd & 0x10) >> 2) | (port >> 14)] = v;
        update_paging(m);
        return;
    }
    if ((lo & 0x9f) == 0x9f && !(m->atm.p77_hi & 0x40)) {
        video_update(m, m->t);
        m->atm.pal_raw[m->border & 15] = v;
        m->atm.pal[m->border & 15] = atm_colour(v);
    }
    if (beta_port(lo))
        beta_write(&m->beta, lo, v, now_t(m));
}

/* ---- state --------------------------------------------------------------- */

#define STATE_MAGIC   0x534d585au  /* "ZXMS" */
#define STATE_VERSION 1
#define ST_SD0  1
#define ST_SD1  2
#define ST_TAPE 4
#define ST_S256 32
#define ST_SD_EXP0 8
#define ST_SD_EXP1 16
#define ST_EXP_SHIFT 8   /* the interface's feature bits, above the flags */

/* Header, the zx struct, the Next's part or the interface's, the tape
 * position. The structs
 * are stored as they are in memory, so a state belongs to one build of the
 * core (native or WebAssembly); the sizes in the header catch a mismatch. */
typedef struct state_hdr {
    uint32_t magic, version, model, flags;
    uint32_t zx_size, nx_size, tape_size, exp_size;
} state_hdr;

/* RAM beyond the eight pages inside the struct. */
static size_t ext_ram(const zx *m)
{
    return (const void *)m->ram == (const void *)m->ram8 ? 0 : (size_t)m->ram_pages * 0x4000;
}

size_t zx_state_size(const zx *m)
{
    return sizeof(state_hdr) + sizeof(zx)
        + (m->model == ZX_NEXT ? nx_state_size(m) : 0)
        + zx_exp_state_size(m) + ext_ram(m) + zx_tape_state_size()
        + (m->s256 ? s256_state_size(m->s256) : 0);
}

int zx_state_save(const zx *m, void *buf, size_t len)
{
    uint8_t *p = buf;
    state_hdr h;
    if (len < zx_state_size(m))
        return -1;
    memset(&h, 0, sizeof h);
    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.model = (uint32_t)m->model;
    h.zx_size = sizeof(zx);
    h.nx_size = m->model == ZX_NEXT ? (uint32_t)nx_state_size(m) : 0;
    h.tape_size = (uint32_t)zx_tape_state_size();
    h.exp_size = (uint32_t)zx_exp_state_size(m);
    if (m->next && m->next->sd[0])
        h.flags |= ST_SD0;
    if (m->next && m->next->sd[1])
        h.flags |= ST_SD1;
    if (m->tape)
        h.flags |= ST_TAPE;
    if (m->s256)
        h.flags |= ST_S256;
    if (m->exp) {
        h.flags |= m->exp->features << ST_EXP_SHIFT;
        if (m->exp->sd[0])
            h.flags |= ST_SD_EXP0;
        if (m->exp->sd[1])
            h.flags |= ST_SD_EXP1;
    }
    memcpy(p, &h, sizeof h);
    p += sizeof h;
    memcpy(p, m, sizeof *m);
    p += sizeof *m;
    if (m->model == ZX_NEXT) {
        nx_state_save(m, p);
        p += h.nx_size;
    }
    memcpy(p, m->ram, ext_ram(m));
    p += ext_ram(m);
    if (m->exp) {
        zx_exp_state_save(m, p);
        p += h.exp_size;
    }
    if (m->tape)
        zx_tape_state_save(m->tape, p);
    else
        memset(p, 0, h.tape_size);
    p += h.tape_size;
    if (m->s256)
        s256_state_save(m->s256, p);
    return 0;
}

int zx_state_load(zx *m, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    state_hdr h;
    const zx_timing *tm = m->tm;
    z80_bus bus = m->bus;
    const uint16_t *pixels = m->pixels;
    struct zxn *next = m->next;
    struct zx_tape *tape = m->tape;
    struct s256 *s256 = m->s256;
    struct zx_exp *exp = m->exp;
    uint8_t (*ram)[0x4000] = m->ram;
    uint16_t *wide = m->wide;
    struct zx_blockdev *disk[BETA_DRIVES];
    uint8_t *raw[BETA_DRIVES];
    int has_sd[2], i;
    if (len < sizeof h)
        return -1;
    memcpy(&h, p, sizeof h);
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION
        || h.model != (uint32_t)m->model || h.zx_size != sizeof(zx)
        || h.nx_size != (m->model == ZX_NEXT ? nx_state_size(m) : 0)
        || h.tape_size != zx_tape_state_size() || h.exp_size != zx_exp_state_size(m)
        || h.flags >> ST_EXP_SHIFT != (m->exp ? m->exp->features : 0)
        || len < zx_state_size(m) || !(h.flags & ST_S256) != !s256)
        return -1;
    p += sizeof h;
    /* The live machine's pointers: the timing table, the bus callbacks,
     * the picture, the Next's own state, the interface, the RAM, the tape
     * and the disks stay as they are. */
    memcpy(disk, m->beta.disk, sizeof disk);
    memcpy(raw, m->beta.raw, sizeof raw);
    memcpy(m, p, sizeof *m);
    p += sizeof *m;
    m->tm = tm;
    m->bus = bus;
    m->pixels = pixels;
    m->next = next;
    m->tape = tape;
    m->s256 = s256;
    m->exp = exp;
    m->ram = ram;
    m->wide = wide;
    for (i = 0; i < BETA_DRIVES; i++) {
        m->beta.disk[i] = disk[i];
        m->beta.raw[i] = raw[i];
    }
    if (m->model == ZX_NEXT) {
        has_sd[0] = (h.flags & ST_SD0) != 0;
        has_sd[1] = (h.flags & ST_SD1) != 0;
        nx_state_load(m, p, has_sd);
        p += h.nx_size;
    } else {
        memcpy(m->ram, p, ext_ram(m));
        p += ext_ram(m);
        update_paging(m);
    }
    if (m->exp) {
        has_sd[0] = (h.flags & ST_SD_EXP0) != 0;
        has_sd[1] = (h.flags & ST_SD_EXP1) != 0;
        zx_exp_state_load(m, p, has_sd);
        p += h.exp_size;
    }
    if (m->tape && (h.flags & ST_TAPE))
        zx_tape_state_load(m->tape, p);
    p += h.tape_size;
    if (s256)
        s256_state_load(s256, p);
    return 0;
}

/* ---- setup ------------------------------------------------------------- */

static const char *const rom_files[ZX_MODEL_COUNT][4] = {
    { "48.rom" },
    { "128-0.rom", "128-1.rom" },
    { "plus2-0.rom", "plus2-1.rom" },
    { "plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom" },
    { "plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom" },
    { "128-0.rom", "128-1.rom", "trdos.rom" },
    { NULL },
    { "256s-0.rom", "256s-1.rom", "256s-2.rom", "256s-3.rom" },
    { "atm2-0.rom", "atm2-1.rom", "atm2-2.rom", "atm2-3.rom" },
};

const char *zx_rom_file(zx_model model, int i)
{
    return model < ZX_MODEL_COUNT && i >= 0 && i < 4 ? rom_files[model][i] : NULL;
}

int zx_rom_optional(zx_model model, int i)
{
    return model == ZX_PENTAGON && i == 2;
}

int zx_create(zx *m, zx_model model, const uint8_t *const roms[4])
{
    int i;

    memset(m, 0, sizeof *m);
    m->model = model;
    m->tm = &zx_timings[model];
    m->pixels = &m->screen[0][0];
    m->screen_w = ZX_SCREEN_W;
    m->screen_h = ZX_SCREEN_H;
    m->pitch = ZX_SCREEN_W;
    if (model == ZX_NEXT) {
        /* Everything the Next needs comes from its SD card. */
        for (i = 0; i < 8; i++)
            m->keys[i] = 0x1f;
        return nx_init(m);
    }
    m->ram = m->ram8;
    m->ram_pages = 8;
    for (i = 0; i < 4; i++) {
        memset(m->rom[i], 0xff, sizeof m->rom[i]);
        if (rom_files[model][i]) {
            if (!roms || !roms[i]) {
                if (zx_rom_optional(model, i))
                    continue;
                return -1;
            }
            memcpy(m->rom[i], roms[i], sizeof m->rom[i]);
        }
    }
    if (model == ZX_SCORPION || model == ZX_ATM2) {
        m->ram_pages = model == ZX_SCORPION ? 16 : 64;
        m->ram = calloc((size_t)m->ram_pages, 0x4000);
        if (!m->ram) {
            m->ram = m->ram8;
            return -1;
        }
    }
    if (model == ZX_ATM2) {
        m->wide = calloc((size_t)ATM_W * ZX_SCREEN_H, sizeof *m->wide);
        if (!m->wide) {
            zx_free(m);
            return -1;
        }
        m->pixels = m->wide;
        m->screen_w = m->pitch = ATM_W;
        for (i = 0; i < 16; i++) {
            m->atm.pal[i] = zx_colour[i];
            m->atm.pal_raw[i] = 0xff;
        }
    }
    m->has_beta = model == ZX_SCORPION || model == ZX_ATM2 || (model == ZX_PENTAGON && roms && roms[2]);
    beta_init(&m->beta, m->tm->cpu_hz);
    z80_init(&m->cpu, 0);
    m->bus.ctx = m;
    m->bus.fetch = b_fetch;
    m->bus.read = b_read;
    m->bus.write = b_write;
    m->bus.io_read = b_io_read;
    m->bus.io_write = b_io_write;
    m->bus.internal = b_internal;
    m->bus.tick = b_tick;
    zx_reset(m);
    return 0;
}

void zx_free(zx *m)
{
    if (m->model == ZX_NEXT) {
        nx_free(m);
        return;
    }
    zx_exp_free(m);
    beta_free(&m->beta);
    if (m->ram && m->ram != m->ram8)
        free(m->ram);
    m->ram = m->ram8;
    m->ram_pages = 8;
    free(m->wide);
    m->wide = NULL;
    m->pixels = &m->screen[0][0];
}

int zx_attach_disk_dev(zx *m, int drive, struct zx_blockdev *bd)
{
    if (!m->has_beta)
        return -1;
    return beta_insert(&m->beta, drive, bd);
}

int zx_attach_disk_image(zx *m, int drive, const uint8_t *data, size_t len, int scl)
{
    if (!m->has_beta || len > 0x1000000)
        return -1;
    return beta_insert_image(&m->beta, drive, data, (uint32_t)len, scl);
}

void zx_eject_disk(zx *m, int drive)
{
    beta_eject(&m->beta, drive);
}

int zx_attach_sd_dev(zx *m, int slot, struct zx_blockdev *bd)
{
    if (m->model != ZX_NEXT)
        return zx_exp_attach_sd(m, slot, bd);
    return nx_attach_sd_dev(m, slot, bd);
}

void zx_reset(zx *m)
{
    int i;
    if (m->model == ZX_NEXT) {
        nx_soft_reset(m);
        return;
    }
    z80_reset(&m->cpu);
    m->port_7ffd = 0;
    m->port_1ffd = 0;
    m->paging_locked = m->model == ZX_48K;
    m->dos = 0;
    if (is_atm(m)) {
        /* Reset clears pen and cpm: the ATM ROM in every window, TR-DOS
         * ports on. Spectrum screen, INT on, no turbo. */
        m->atm.p77 = 0x23;
        m->atm.p77_hi = 0;
        m->atm.carry = 0;
    }
    beta_reset(&m->beta);
    update_paging(m);
    m->border = 7;
    m->ulaplus_reg = m->ulaplus_mode = 0;
    for (i = 0; i < 8; i++)
        m->keys[i] = 0x1f;
    ay_reset(&m->ay);
    zx_exp_reset(m);
    m->t = 0;
    m->video_chunk = 0;
    m->audio_t = 0;
    m->audio_frac = 0;
}

void zx_poke(zx *m, uint16_t addr, uint8_t v)
{
    uint8_t *p;
    if (m->model == ZX_NEXT) {
        m->next->slot[addr >> 13].wr[addr & 0x1fff] = v;
        return;
    }
    if (addr < 0x4000 && m->div_map) {
        zx_exp_write(m, addr, v);
        return;
    }
    p = m->wr[addr >> 14];
    if (p)
        p[addr & 0x3fff] = v;
}

uint8_t zx_peek(const zx *m, uint16_t addr)
{
    if (m->model == ZX_NEXT)
        return m->next->slot[addr >> 13].rd[addr & 0x1fff];
    return mem_read(m, addr);
}

void zx_key(zx *m, int row, int bit, int down)
{
    if (down)
        m->keys[row] &= (uint8_t)~(1 << bit);
    else
        m->keys[row] |= (uint8_t)(1 << bit);
}

void zx_mouse(zx *m, int dx, int dy, int dz, int buttons)
{
    if (m->next)
        nx_mouse(m->next, dx, dy, dz, buttons);
    else if (m->exp && (m->exp->features & ZX_EXP_MOUSE))
        zx_kmouse_move(&m->exp->mouse, dx, dy, dz, buttons, 4);
}

void zx_run_frame(zx *m)
{
    const uint32_t frame_t = m->tm->frame_t;
    if (m->model == ZX_NEXT) {
        nx_run_frame(m);
        return;
    }
    m->audio_samples = 0;

    while (m->t < frame_t) {
        uint32_t t0 = m->t;
        if (m->t < m->tm->int_len && z80_int_enabled(&m->cpu)
            && !(m->exp && (m->exp->port_ff & 0x40))
            && !(is_atm(m) && !(m->atm.p77 & 0x20))) {   /* the ATM's INT gate */
            if (m->s256)
                s256_interrupt(m->s256);
            else
                z80_interrupt(&m->cpu, &m->bus);
        } else if (m->tape && m->cpu.pc == 0x0556 && !m->div_map && m->rd[0] == m->rom[tape_rom(m)]
                   && zx_tape_trap(m->tape, m)) {
            continue;
        } else if (m->s256) {
            s256_step(m->s256);
        } else {
            z80_step(&m->cpu, &m->bus);
        }
        if (is_atm(m) && (m->atm.p77 & 8)) {
            /* 7 MHz: two CPU T-states to each 3.5 MHz one. Ports inside an
             * instruction see the unhalved count. */
            uint32_t d = m->t - t0 + m->atm.carry;
            m->t = t0 + d / 2;
            m->atm.carry = (uint8_t)(d & 1);
        }
    }

    video_update(m, 0xffffffffu >> 1);
    if (m->s256)
        s256_render(m->s256);
    audio_update(m, frame_t);
    if (m->tape && zx_tape_playing(m->tape))
        zx_tape_advance(m->tape, m, (m->frames + 1) * frame_t);
    if (m->exp)
        zx_exp_frame_end(m);

    m->t -= frame_t;
    m->audio_t = 0;
    m->audio_frac -= (uint64_t)frame_t << 32;
    m->video_chunk = 0;
    m->frames++;
}
