#include "sam.h"
#include "../zx/tape.h"
#include "../devices/floppy.h"

#include <string.h>

/* ---- the frame ------------------------------------------------------------
 *
 * The frame interrupt starts the frame. A raster line is 48 cells of 8
 * T-states (16 high-res pixels): 8 cells of left border, 32 of screen, 8 of
 * right border, and raster line L starts 64 T-states (the right border) into
 * frame line L. The screen is raster lines 68..259.
 */
#define TOP_LINES     68
#define SCREEN_LINES  192
#define SIDE_CELLS    8
#define CELL_T        8
#define INT_T         128          /* an interrupt is active this long */
#define ASIC_WAKE_T   291675       /* ~49 ms after reset the ASIC answers */

/* The visible part of the raster: 256 lines from 36, 40 cells from 4. */
#define VIS_TOP       (TOP_LINES - 32)
#define VIS_LEFT      4
#define VIS_CELLS     40
#define VIS_CHUNKS    (SAM_FB_H * VIS_CELLS)

/* Ports */
#define PORT_LEPR     0x80
#define PORT_HEPR     0x81
#define PORT_CLUT     0xf8         /* write; reads: light pen */
#define PORT_STATUS   0xf9         /* read; writes: line interrupt */
#define PORT_LMPR     0xfa
#define PORT_HMPR     0xfb
#define PORT_VMPR     0xfc
#define PORT_MIDI     0xfd
#define PORT_KEYBOARD 0xfe         /* writes: border */
#define PORT_ATTR     0xff         /* writes: SAA1099 */

#define LMPR_ROM0_OFF 0x20
#define LMPR_ROM1     0x40
#define LMPR_WPROT    0x80
#define HMPR_EXTERNAL 0x80
#define BORDER_SOFF   0x80

#define MIDI_T        1920         /* 320 us a byte at 31.25 kbit/s */
#define MIDI_INT_T    96
#define MIDI_TXFMST_T 192

static uint64_t now(const sam *s) { return s->frames * SAM_FRAME_T + s->t; }
static int mode(const sam *s) { return (s->vmpr >> 5) & 3; }      /* 0..3 = modes 1..4 */
static int screen_off(const sam *s) { return (s->border & BORDER_SOFF) && (s->vmpr & 0x40); }
static int screen_page(const sam *s) { return mode(s) >= 2 ? (s->vmpr & 0x1e) : (s->vmpr & 0x1f); }
static int asleep(const sam *s) { return now(s) < s->asic_ready; }

/* ---- contention ------------------------------------------------------------
 *
 * Internal RAM is shared with the ASIC. The CPU gets one access slot every
 * 8 T-states while the screen is being fetched (and in mode 1, in alternate
 * 64 T-state bands everywhere, for the attribute prefetch), and one every 4
 * T-states otherwise. The wait is sampled at T2 of a memory cycle and T3 of
 * an I/O cycle; the ASIC's own ports always use the 8 T-state slots.
 */
static uint32_t mem_wait(const sam *s, uint32_t t)
{
    uint32_t line = t / SAM_LINE_T, lc = (t + 4) % SAM_LINE_T, mask;
    int main = line >= TOP_LINES && line < TOP_LINES + SCREEN_LINES && lc >= 2 * SIDE_CELLS * CELL_T;
    if (mode(s) == 0)
        mask = (main || !(lc & 0x40)) ? 7 : 3;
    else if (screen_off(s))
        mask = 3;
    else
        mask = main ? 7 : 3;
    return mask - ((t + 2) & mask);
}

static uint32_t io_wait(uint16_t port, uint32_t t)
{
    return (port & 0xff) >= 0xf8 ? 7 - ((t + 2) & 7) : 0;
}

/* ---- interrupts ------------------------------------------------------------ */

static uint8_t int_status(const sam *s)
{
    uint8_t st = 0x1f;
    uint32_t t = s->t;
    if (t < INT_T)
        st &= (uint8_t)~0x08;
    if (s->line < SCREEN_LINES) {
        uint32_t at = (uint32_t)(s->line + TOP_LINES) * SAM_LINE_T;
        if (t >= at && t < at + INT_T)
            st &= (uint8_t)~0x01;
    }
    if (s->midi_at) {
        uint64_t n = now(s);
        if (n >= s->midi_at && n < s->midi_at + MIDI_INT_T)
            st &= (uint8_t)~0x10;
    }
    return st;
}

/* ---- video ----------------------------------------------------------------- */

static uint16_t rgb(uint8_t c)
{
    /* Palette index bits: 0 B1, 1 R1, 2 G1, 3 bright, 4 B2, 5 R2, 6 G2. */
    unsigned b = (unsigned)(((c >> 4) & 1) << 2 | (c & 1) << 1 | ((c >> 3) & 1));
    unsigned r = (unsigned)(((c >> 5) & 1) << 2 | ((c >> 1) & 1) << 1 | ((c >> 3) & 1));
    unsigned g = (unsigned)(((c >> 6) & 1) << 2 | ((c >> 2) & 1) << 1 | ((c >> 3) & 1));
    return (uint16_t)(r << 6 | g << 3 | b);
}

static uint16_t border_rgb(const sam *s)
{
    return rgb(s->clut[((s->border & 0x20) >> 2) | (s->border & 7)]);
}

static uint32_t mode1_offset(int line)
{
    return (uint32_t)(((line & 0xc0) << 5) | ((line & 7) << 8) | ((line & 0x38) << 2));
}

static int32_t chunk_t(uint32_t c)
{
    uint32_t y = c / VIS_CELLS, x = c % VIS_CELLS;
    return (int32_t)(SIDE_CELLS * CELL_T + (y + VIS_TOP) * SAM_LINE_T + (x + VIS_LEFT) * CELL_T);
}

static void draw_chunk(sam *s, uint32_t c)
{
    int y = (int)(c / VIS_CELLS), x = (int)(c % VIS_CELLS), i;
    int line = y + VIS_TOP - TOP_LINES, col = x + VIS_LEFT - SIDE_CELLS;
    uint16_t *px = &s->screen[y][x * 16];
    const uint8_t *mem = s->ram[screen_page(s)];

    if (screen_off(s)) {
        for (i = 0; i < 16; i++)
            px[i] = 0;
        return;
    }
    if (line < 0 || line >= SCREEN_LINES || col < 0 || col >= 32) {
        uint16_t b = border_rgb(s);
        for (i = 0; i < 16; i++)
            px[i] = b;
        return;
    }
    switch (mode(s)) {
    case 0:
    case 1: {
        uint8_t data, attr, ink, paper;
        if (mode(s) == 0) {
            data = mem[mode1_offset(line) + (uint32_t)col];
            attr = mem[6144 + (uint32_t)(line >> 3) * 32 + (uint32_t)col];
        } else {
            data = mem[(uint32_t)line * 32 + (uint32_t)col];
            attr = mem[0x2000 + (uint32_t)line * 32 + (uint32_t)col];
        }
        ink = (uint8_t)(((attr >> 3) & 8) | (attr & 7));
        paper = (attr >> 3) & 15;
        if ((attr & 0x80) && s->flash) {
            uint8_t k = ink;
            ink = paper;
            paper = k;
        }
        {
            uint16_t fg = rgb(s->clut[ink]), bg = rgb(s->clut[paper]);
            for (i = 0; i < 8; i++)
                px[i * 2] = px[i * 2 + 1] = (data & (0x80 >> i)) ? fg : bg;
        }
        break;
    }
    case 2: {
        /* Mode 3: 512 wide, 2 bits a pixel from CLUT 0..3 (or the set HMPR
         * bits 5..6 choose), with entries 1 and 2 swapped. */
        uint8_t base = (uint8_t)((s->hmpr & 0x60) >> 3);
        uint16_t c4[4];
        const uint8_t *p = mem + (uint32_t)line * 128 + (uint32_t)col * 4;
        c4[0] = rgb(s->clut[base]);
        c4[1] = rgb(s->clut[base | 2]);
        c4[2] = rgb(s->clut[base | 1]);
        c4[3] = rgb(s->clut[base | 3]);
        for (i = 0; i < 16; i++)
            px[i] = c4[(p[i >> 2] >> (6 - 2 * (i & 3))) & 3];
        break;
    }
    default: {
        const uint8_t *p = mem + (uint32_t)line * 128 + (uint32_t)col * 4;
        for (i = 0; i < 8; i++) {
            uint8_t n = (uint8_t)((i & 1) ? p[i >> 1] & 15 : p[i >> 1] >> 4);
            px[i * 2] = px[i * 2 + 1] = rgb(s->clut[n]);
        }
        break;
    }
    }
}

/* Draw every cell that has finished by `upto`. */
static void video_update(sam *s, uint32_t upto)
{
    while (s->video_cell < VIS_CHUNKS && chunk_t(s->video_cell) + CELL_T <= (int32_t)upto)
        draw_chunk(s, s->video_cell++);
}

/* The four display bytes the ASIC has fetched at time t. */
static void asic_data(const sam *s, uint32_t t, uint8_t b[4])
{
    const uint8_t *mem = s->ram[screen_page(s)];
    int line = (int)(t / SAM_LINE_T) - TOP_LINES, cell = (int)((t % SAM_LINE_T) >> 3) - 2 * SIDE_CELLS;
    if (cell < 0) {
        line--;
        cell = 31;
    }
    if (line < 0 || line >= SCREEN_LINES) {
        line = SCREEN_LINES - 1;
        cell = 31;
    }
    if (mode(s) >= 2) {
        memcpy(b, mem + line * 128 + cell * 4, 4);
    } else if (mode(s) == 0) {
        b[0] = b[1] = mem[mode1_offset(line) + (uint32_t)cell];
        b[2] = b[3] = mem[6144 + (line & 0xf8) * 4 + cell];
    } else {
        b[0] = b[1] = mem[line * 32 + cell];
        b[2] = b[3] = mem[0x2000 + line * 32 + cell];
    }
}

static int on_screen(uint32_t t)
{
    uint32_t line = t / SAM_LINE_T;
    return line >= TOP_LINES && line < TOP_LINES + SCREEN_LINES && t % SAM_LINE_T >= 2 * SIDE_CELLS * CELL_T;
}

static uint8_t txfmst(const sam *s)
{
    return s->midi_at && now(s) < s->midi_at + MIDI_TXFMST_T ? 0x02 : 0;
}

static uint8_t update_lpen(sam *s)
{
    uint32_t t = s->t, lc = t % SAM_LINE_T;
    if (screen_off(s)) {
        s->lpen = (uint8_t)((s->lpen & ~1) | (s->border & 1));
    } else if (on_screen(t)) {
        uint8_t b[4], bcd1 = 0;
        asic_data(s, t, b);
        switch (mode(s)) {
        case 0:
        case 1: {
            int ink = (b[0] & 0x40) != 0, rev = (b[2] & 0x80) && s->flash;
            bcd1 = (uint8_t)((b[2] >> ((ink ^ rev) ? 0 : 3)) & 1);
            break;
        }
        case 2: bcd1 = ((b[0] >> 1) | (b[0] >> 3)) & 1; break;
        default: bcd1 = b[0] & 1; break;
        }
        s->lpen = (uint8_t)(((lc - 2 * SIDE_CELLS * CELL_T) & 0xfc) | bcd1);
    } else {
        s->lpen = s->border & 1;
    }
    return (uint8_t)(s->lpen | txfmst(s));
}

static uint8_t update_hpen(sam *s)
{
    uint32_t t = s->t, line = t / SAM_LINE_T;
    if (!screen_off(s)) {
        if (line >= TOP_LINES && line < TOP_LINES + SCREEN_LINES
            && (line != TOP_LINES || t % SAM_LINE_T >= 2 * SIDE_CELLS * CELL_T))
            s->hpen = (uint8_t)(line - TOP_LINES);
        else
            s->hpen = SCREEN_LINES;
    }
    return s->hpen;
}

/* ---- sound ----------------------------------------------------------------- */

#define BEEP_LEVEL 8000

static void audio_accumulate(sam *s, uint32_t dt)
{
    uint64_t clocks;
    s->beeper_acc += (s->speaker ? BEEP_LEVEL : 0) * (int32_t)dt;
    /* The SAA1099 runs from 8 MHz: 4 clocks for every 3 T-states. */
    clocks = (uint64_t)dt * 4 + s->saa_frac;
    saa_run(&s->saa, (uint32_t)(clocks / 3));
    s->saa_frac = (uint32_t)(clocks % 3);
}

static void audio_update(sam *s, uint32_t upto)
{
    const uint64_t step = ((uint64_t)SAM_CPU_HZ << 32) / SAM_AUDIO_RATE;
    for (;;) {
        uint32_t next = (uint32_t)(s->audio_frac >> 32), span = (uint32_t)(step >> 32);
        int32_t l, r, beep;
        if (next > upto) {
            if (upto > s->audio_t) {
                audio_accumulate(s, upto - s->audio_t);
                s->audio_t = upto;
            }
            return;
        }
        if (next > s->audio_t)
            audio_accumulate(s, next - s->audio_t);
        s->audio_t = next;
        s->audio_frac += step;
        beep = s->beeper_acc / (int32_t)span;
        s->beeper_acc = 0;
        saa_take(&s->saa, &l, &r);
        /* SAA full scale (all six channels at maximum) to about 3/4. */
        l = beep / 2 + l * 8;
        r = beep / 2 + r * 8;
        if (s->audio_samples < (int)(sizeof s->audio / sizeof s->audio[0] / 2)) {
            s->audio[s->audio_samples * 2] = (int16_t)(l > 32767 ? 32767 : l);
            s->audio[s->audio_samples * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r);
            s->audio_samples++;
        }
    }
}

/* ---- paging ---------------------------------------------------------------- */

/* Reads from where nothing is fitted (external memory) float high. */
static uint8_t no_memory[0x4000];

static void map(sam *s, int sec, int page)
{
    if (page < 0) {
        s->rd[sec] = no_memory;
        s->wr[sec] = NULL;
        s->page[sec] = -1;
    } else {
        s->rd[sec] = s->wr[sec] = s->ram[page];
        s->page[sec] = (int8_t)page;
    }
}

static void update_paging(sam *s)
{
    if (!(s->lmpr & LMPR_ROM0_OFF)) {
        s->rd[0] = s->rom[0];
        s->wr[0] = NULL;
        s->page[0] = -1;
    } else {
        map(s, 0, s->lmpr & 0x1f);
        if (s->lmpr & LMPR_WPROT)
            s->wr[0] = NULL;
    }
    map(s, 1, (s->lmpr + 1) & 0x1f);
    /* External memory: none is fitted. */
    if (s->hmpr & HMPR_EXTERNAL) {
        map(s, 2, -1);
        map(s, 3, -1);
    } else {
        map(s, 2, s->hmpr & 0x1f);
        if (s->lmpr & LMPR_ROM1) {
            s->rd[3] = s->rom[1];
            s->wr[3] = NULL;
            s->page[3] = -1;
        } else {
            map(s, 3, (s->hmpr + 1) & 0x1f);
        }
    }
}

/* ---- bus ------------------------------------------------------------------- */

static inline uint32_t wait_for(const sam *s, uint16_t addr)
{
    return s->page[addr >> 14] >= 0 ? mem_wait(s, s->t) : 0;
}

static uint8_t b_fetch(void *ctx, uint16_t addr)
{
    sam *s = ctx;
    uint8_t v;
    s->t += 2;
    s->t += wait_for(s, addr);
    v = s->rd[addr >> 14][addr & 0x3fff];
    s->t += 2;
    return v;
}

static uint8_t b_read(void *ctx, uint16_t addr)
{
    sam *s = ctx;
    uint8_t v;
    s->t += 2;
    s->t += wait_for(s, addr);
    v = s->rd[addr >> 14][addr & 0x3fff];
    s->t += 1;
    return v;
}

static void b_write(void *ctx, uint16_t addr, uint8_t v)
{
    sam *s = ctx;
    uint8_t *p;
    int pg;
    s->t += 2;
    s->t += wait_for(s, addr);
    p = s->wr[addr >> 14];
    if (p) {
        pg = s->page[addr >> 14];
        if (pg == screen_page(s) || (mode(s) >= 2 && pg == screen_page(s) + 1))
            video_update(s, s->t);
        p[addr & 0x3fff] = v;
    }
    s->t += 1;
}

static void b_internal(void *ctx, uint16_t addr, unsigned n)
{
    sam *s = ctx;
    (void)addr;
    s->t += n;
}

static void b_tick(void *ctx, unsigned n)
{
    sam *s = ctx;
    s->t += n;
}

static uint8_t tape_ear(sam *s)
{
    if (!s->tape || !zx_tape_playing(s->tape))
        return s->ear;
    /* Tape timings are in 3.5 MHz T-states. */
    return (uint8_t)zx_tape_ear(s->tape, now(s) * 7 / 12);
}

static uint8_t in(sam *s, uint16_t port)
{
    uint8_t lo = (uint8_t)port, hi = (uint8_t)(port >> 8), k;
    int i;

    if (lo >= 0xf8 && asleep(s))
        return 0x00;

    switch (lo) {
    case PORT_KEYBOARD:
        if (s->tape && s->cpu.pc == s->hook_edge && s->hook_edge && !zx_tape_playing(s->tape))
            zx_tape_play(s->tape, now(s) * 7 / 12);
        k = 0x1f;
        if (hi == 0xff) {
            k &= s->keys[8];
        } else {
            for (i = 0; i < 8; i++)
                if (!(hi & (1 << i)))
                    k &= s->keys[i];
        }
        return (uint8_t)((k & 0x1f) | (s->border & BORDER_SOFF) | (tape_ear(s) ? 0x40 : 0));
    case PORT_STATUS:
        k = 0xe0;
        for (i = 0; i < 8; i++)
            if (!(hi & (1 << i)))
                k &= s->keys[i];
        return (uint8_t)((k & 0xe0) | int_status(s));
    case PORT_LMPR:
        return s->lmpr;
    case PORT_HMPR:
        return s->hmpr;
    case PORT_VMPR:
        return (uint8_t)(0x80 | s->vmpr);
    case PORT_CLUT:
        return (port & 0x1ff) == 0xf8 ? update_lpen(s) : update_hpen(s);
    case PORT_ATTR:
        if (!screen_off(s)) {
            uint8_t b[4];
            asic_data(s, s->t, b);
            s->attr = b[2];
        }
        return s->attr;
    case 0x1f:                         /* Kempston joystick */
        return (uint8_t)~s->kempston;
    default:
        break;
    }
    if ((lo & 0xf8) == 0xe0 || (lo & 0xf8) == 0xf0) {
        int d = (lo & 0x10) != 0;
        return wd_read(&s->fdc[d], s->disk[d], lo & 3, now(s));
    }
    /* Nothing answers: the bus holds what the ASIC is fetching. */
    if (on_screen(s->t)) {
        uint8_t b[4];
        asic_data(s, s->t, b);
        return b[2];
    }
    return 0xff;
}

static void out_vmpr(sam *s, uint8_t v)
{
    uint8_t old = s->vmpr;
    v &= 0x7f;
    if ((old ^ v) & 0x60) {
        /* Changes into or out of modes 3 and 4 show at once, others a
         * cell later. */
        video_update(s, ((old | v) & 0x40) ? s->t : s->t + CELL_T);
        s->vmpr = (uint8_t)((old & 0x1f) | (v & 0x60));
    }
    if ((old ^ v) & 0x1f) {
        video_update(s, s->t + CELL_T);
        s->vmpr = v;
    }
}

static void out_border(sam *s, uint8_t v)
{
    uint8_t old = s->border;
    if (((old ^ v) & BORDER_SOFF) && (s->vmpr & 0x40)) {
        video_update(s, s->t);
        if (!(old & BORDER_SOFF)) {
            /* The screen goes off: the pens and attribute latch hold. */
            uint8_t b[4];
            update_lpen(s);
            update_hpen(s);
            asic_data(s, s->t, b);
            s->attr = b[2];
        }
    } else if ((old ^ v) & 0x27) {
        video_update(s, s->t);
    }
    if ((old ^ v) & 0x10) {
        audio_update(s, s->t);
        s->speaker = (v >> 4) & 1;
    }
    s->mic = (v >> 3) & 1;
    s->border = v;
    if (s->tape && s->hook_load_end && s->cpu.pc == s->hook_load_end)
        zx_tape_stop(s->tape);
}

static void out(sam *s, uint16_t port, uint8_t v)
{
    uint8_t lo = (uint8_t)port;

    if (lo >= 0xf8 && asleep(s))
        return;

    switch (lo) {
    case PORT_KEYBOARD:
        if (v != s->border)
            out_border(s, v);
        return;
    case PORT_VMPR:
        out_vmpr(s, v);
        return;
    case PORT_HMPR:
        if (mode(s) == 2 && ((s->hmpr ^ v) & 0x60))
            video_update(s, s->t);
        s->hmpr = v;
        update_paging(s);
        return;
    case PORT_LMPR:
        s->lmpr = v;
        update_paging(s);
        return;
    case PORT_CLUT:
        if (s->clut[(port >> 8) & 15] != (v & 0x7f)) {
            video_update(s, s->t);
            s->clut[(port >> 8) & 15] = v & 0x7f;
        }
        return;
    case PORT_STATUS:
        s->line = v;
        return;
    case PORT_MIDI:
        if (!txfmst(s)) {
            /* The byte goes out over 320 us; the interrupt comes at the end. */
            uint64_t n = now(s), end = (n + MIDI_T + 16 + 31) & ~(uint64_t)31;
            s->midi_at = end - 16 - 32 - MIDI_INT_T + 1;
        }
        return;
    case PORT_ATTR:
        audio_update(s, s->t);
        if (port & 0x100)
            saa_write_address(&s->saa, v);
        else
            saa_write_data(&s->saa, v);
        return;
    case PORT_LEPR:
        s->lepr = v;
        update_paging(s);
        return;
    case PORT_HEPR:
        s->hepr = v;
        update_paging(s);
        return;
    default:
        break;
    }
    if ((lo & 0xf8) == 0xe0 || (lo & 0xf8) == 0xf0) {
        int d = (lo & 0x10) != 0;
        s->fdc[d].side = (lo >> 2) & 1;
        wd_write(&s->fdc[d], s->disk[d], lo & 3, v, now(s));
    }
}

static uint8_t b_io_read(void *ctx, uint16_t port)
{
    sam *s = ctx;
    uint8_t v;
    s->t += 3;
    s->t += io_wait(port, s->t);
    v = in(s, port);
    s->t += 1;
    return v;
}

static void b_io_write(void *ctx, uint16_t port, uint8_t v)
{
    sam *s = ctx;
    s->t += 3;
    s->t += io_wait(port, s->t);
    out(s, port, v);
    s->t += 1;
}

/* ---- keyboard -------------------------------------------------------------- */

void sam_key(sam *s, int row, int bit, int down)
{
    if (row < 0 || row > 8 || bit < 0 || bit > 7)
        return;
    if (down)
        s->keys[row] &= (uint8_t)~(1 << bit);
    else
        s->keys[row] |= (uint8_t)(1 << bit);
}

/* The matrix, by row (A8..A15, then A8-A15 all high) and bit:
 *   0  SHIFT Z X C V F1 F2 F3        5  P O I U Y = " F0
 *   1  A S D F G F4 F5 F6            6  RETURN L K J H ; : EDIT
 *   2  Q W E R T F7 F8 F9            7  SPACE SYMBOL M N B , . INV
 *   3  1 2 3 4 5 ESC TAB CAPS        8  CNTRL UP DOWN LEFT RIGHT
 *   4  0 9 8 7 6 - + DELETE
 */
static const char *const rows[8] = {
    "\001zxcv", "asdfg", "qwert", "12345", "09876", "poiuy", "\nlkjh", " \002mnb",
};

static const struct { char ch; uint8_t row, bit, shift, sym; } punct[] = {
    { '!', 3, 0, 1, 0 }, { '@', 3, 1, 1, 0 }, { '#', 3, 2, 1, 0 }, { '$', 3, 3, 1, 0 },
    { '%', 3, 4, 1, 0 }, { '&', 4, 4, 1, 0 }, { '\'', 4, 3, 1, 0 }, { '(', 4, 2, 1, 0 },
    { ')', 4, 1, 1, 0 }, { '~', 4, 0, 1, 0 }, { '-', 4, 5, 0, 0 }, { '/', 4, 5, 1, 0 },
    { '+', 4, 6, 0, 0 }, { '*', 4, 6, 1, 0 }, { '<', 2, 0, 0, 1 }, { '>', 2, 1, 0, 1 },
    { '[', 2, 3, 0, 1 }, { ']', 2, 4, 0, 1 }, { '=', 5, 5, 0, 0 }, { '_', 5, 5, 1, 0 },
    { '"', 5, 6, 0, 0 }, { '`', 5, 6, 1, 0 }, { '{', 1, 3, 0, 1 }, { '}', 1, 4, 0, 1 },
    { '^', 6, 4, 0, 1 }, { ';', 6, 5, 0, 0 }, { ':', 6, 6, 0, 0 }, { '?', 0, 2, 0, 1 },
    { '.', 7, 6, 0, 0 }, { ',', 7, 5, 0, 0 }, { '\\', 7, 7, 1, 0 }, { '|', 4, 1, 0, 1 },
};

int sam_char_key(int ch, int *row, int *bit, int *shift, int *sym)
{
    int r, b;
    size_t i;
    *shift = *sym = 0;
    if (ch >= 'A' && ch <= 'Z') {
        *shift = 1;
        ch += 'a' - 'A';
    }
    if (ch == '\r')
        ch = '\n';
    for (r = 0; r < 8; r++) {
        for (b = 0; b < 5; b++) {
            if (rows[r][b] == ch && ch > 2) {
                *row = r;
                *bit = b;
                return 1;
            }
        }
    }
    for (i = 0; i < sizeof punct / sizeof punct[0]; i++) {
        if (punct[i].ch == ch) {
            *row = punct[i].row;
            *bit = punct[i].bit;
            *shift = punct[i].shift;
            *sym = punct[i].sym;
            return 1;
        }
    }
    return 0;
}

/* ---- memory for debuggers and loaders -------------------------------------- */

uint8_t sam_peek(const sam *s, uint16_t addr)
{
    return s->rd[addr >> 14][addr & 0x3fff];
}

void sam_poke(sam *s, uint16_t addr, uint8_t v)
{
    if (s->wr[addr >> 14])
        s->wr[addr >> 14][addr & 0x3fff] = v;
}

/* ---- setup ----------------------------------------------------------------- */

/* Find a byte pattern once in a ROM half (-1 is "any"); 0 if not unique. */
static uint16_t rom_find(const sam *s, int half, const int *pat, int n, int off)
{
    int i, j, found = -1;
    for (i = 0; i + n <= 0x4000; i++) {
        for (j = 0; j < n; j++)
            if (pat[j] >= 0 && s->rom[half][i + j] != pat[j])
                break;
        if (j == n) {
            if (found >= 0)
                return 0;
            found = i;
        }
    }
    return found < 0 ? 0 : (uint16_t)((half ? 0xc000 : 0) + found + off);
}

int sam_create(sam *s, const uint8_t *rom, size_t len)
{
    /* EDGLP: inc c; ret z; xor b (the tape edge loop reads port 0xFE just
     * before it). SVLDCOM: ex af,af'; ld a,(bordcol); out (0xfe),a. */
    static const int edge[] = { 0x0c, 0xc8, 0xa8 };
    static const int load_end[] = { 0x08, 0x3a, 0x4b, 0x5c, 0xd3, 0xfe };
    int d;
    if (!rom || len != 0x8000)
        return -1;
    memset(no_memory, 0xff, sizeof no_memory);
    memset(s, 0, sizeof *s);
    memcpy(s->rom, rom, 0x8000);
    s->hook_edge = rom_find(s, 0, edge, 3, 0);
    s->hook_load_end = rom_find(s, 1, load_end, 6, 6);
    z80_init(&s->cpu, 0);
    s->bus.ctx = s;
    s->bus.fetch = b_fetch;
    s->bus.read = b_read;
    s->bus.write = b_write;
    s->bus.io_read = b_io_read;
    s->bus.io_write = b_io_write;
    s->bus.internal = b_internal;
    s->bus.tick = b_tick;
    for (d = 0; d < 2; d++)
        wd_init(&s->fdc[d], SAM_CPU_HZ);
    sam_power_on(s);
    return 0;
}

void sam_power_on(sam *s)
{
    int p, i;
    /* Power-on RAM: 0xff with stripes of 0x00, as the chips come up. */
    for (p = 0; p < SAM_PAGES; p++)
        for (i = 0; i < 0x4000; i += 0x100) {
            memset(&s->ram[p][i], 0x00, 0x80);
            memset(&s->ram[p][i + 0x80], 0xff, 0x80);
        }
    memset(s->clut, 0, sizeof s->clut);
    s->line = 0xff;
    saa_reset(&s->saa);
    s->speaker = s->mic = 0;
    sam_reset(s);
}

void sam_reset(sam *s)
{
    int i;
    z80_reset(&s->cpu);
    s->lmpr = s->hmpr = s->vmpr = s->lepr = s->hepr = 0;
    s->border = 0;
    s->lpen = 0;
    s->midi_at = 0;
    update_paging(s);
    for (i = 0; i < 9; i++)
        s->keys[i] = 0xff;
    s->ear = 1;
    s->asic_ready = now(s) + ASIC_WAKE_T;
    for (i = 0; i < 2; i++)
        wd_reset(&s->fdc[i]);
    if (s->tape)
        zx_tape_stop(s->tape);
}

void sam_free(sam *s)
{
    int d;
    for (d = 0; d < 2; d++) {
        fd_close(s->disk[d]);
        s->disk[d] = NULL;
    }
    zx_tape_free(s->tape);
    s->tape = NULL;
}

void sam_insert_disk(sam *s, int drive, struct fd_disk *d)
{
    if (drive < 0 || drive > 1)
        return;
    fd_close(s->disk[drive]);
    s->disk[drive] = d;
}

/* ---- running --------------------------------------------------------------- */

void sam_run_frame(sam *s)
{
    int d;
    s->audio_samples = 0;
    while (s->t < SAM_FRAME_T) {
        if (int_status(s) != 0x1f && z80_int_enabled(&s->cpu)) {
            z80_interrupt(&s->cpu, &s->bus);
            continue;
        }
        z80_step(&s->cpu, &s->bus);
    }
    video_update(s, 0xffffffffu >> 1);
    audio_update(s, SAM_FRAME_T);
    for (d = 0; d < 2; d++)
        wd_run(&s->fdc[d], s->disk[d], s->frames * SAM_FRAME_T + SAM_FRAME_T);
    if (s->tape && zx_tape_playing(s->tape))
        s->ear = (uint8_t)zx_tape_ear(s->tape, (s->frames + 1) * SAM_FRAME_T * 7 / 12);

    s->t -= SAM_FRAME_T;
    s->audio_t = 0;
    s->audio_frac -= (uint64_t)SAM_FRAME_T << 32;
    s->video_cell = 0;
    s->frames++;
    if (!(s->frames % 16))
        s->flash ^= 1;
}

/* ---- state ----------------------------------------------------------------- */

#define STATE_MAGIC   0x4d415353u  /* "SSAM" */
#define STATE_VERSION 1

typedef struct sam_state_hdr {
    uint32_t magic, version, size, tape_size;
    uint32_t has_tape, reserved[3];
} sam_state_hdr;

size_t sam_state_size(const sam *s)
{
    (void)s;
    return sizeof(sam_state_hdr) + sizeof(sam) + zx_tape_state_size();
}

int sam_state_save(const sam *s, void *buf, size_t len)
{
    uint8_t *p = buf;
    sam_state_hdr h;
    if (len < sam_state_size(s))
        return -1;
    memset(&h, 0, sizeof h);
    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.size = sizeof(sam);
    h.tape_size = (uint32_t)zx_tape_state_size();
    h.has_tape = s->tape != NULL;
    memcpy(p, &h, sizeof h);
    p += sizeof h;
    memcpy(p, s, sizeof *s);
    p += sizeof *s;
    if (s->tape)
        zx_tape_state_save(s->tape, p);
    else
        memset(p, 0, h.tape_size);
    return 0;
}

int sam_state_load(sam *s, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    sam_state_hdr h;
    z80_bus bus = s->bus;
    struct fd_disk *disk[2];
    struct zx_tape *tape = s->tape;
    if (len < sizeof h)
        return -1;
    memcpy(&h, p, sizeof h);
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION || h.size != sizeof(sam)
        || h.tape_size != zx_tape_state_size() || len < sam_state_size(s))
        return -1;
    p += sizeof h;
    /* The live machine keeps its bus callbacks, disks and tape. */
    disk[0] = s->disk[0];
    disk[1] = s->disk[1];
    memcpy(s, p, sizeof *s);
    p += sizeof *s;
    s->bus = bus;
    s->disk[0] = disk[0];
    s->disk[1] = disk[1];
    s->tape = tape;
    update_paging(s);
    if (s->tape && h.has_tape)
        zx_tape_state_load(s->tape, p);
    return 0;
}
