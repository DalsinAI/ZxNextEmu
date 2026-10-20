#include "zx.h"
#include "tape.h"

#include <stdio.h>
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
};

static const struct { const char *name; zx_model model; } model_names[] = {
    { "48", ZX_48K }, { "48k", ZX_48K }, { "128", ZX_128K }, { "128k", ZX_128K },
    { "plus2", ZX_PLUS2 }, { "+2", ZX_PLUS2 }, { "plus2a", ZX_PLUS2A }, { "+2a", ZX_PLUS2A },
    { "plus3", ZX_PLUS3 }, { "+3", ZX_PLUS3 }, { "pentagon", ZX_PENTAGON },
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
        uint8_t bits = scr[((row & 0xc0) << 5) | ((row & 7) << 8) | ((row & 0x38) << 2) | col];
        uint8_t attr = scr[6144 + (row >> 3) * 32 + col];
        uint16_t ink = zx_colour[(attr & 7) | ((attr & 0x40) >> 3)];
        uint16_t paper = zx_colour[((attr >> 3) & 7) | ((attr & 0x40) >> 3)];
        if ((attr & 0x80) && (m->frames & 16))
            bits = (uint8_t)~bits;
        for (i = 0; i < 8; i++)
            px[i] = (bits & (0x80 >> i)) ? ink : paper;
    } else {
        uint16_t b = zx_colour[m->border];
        for (i = 0; i < 8; i++)
            px[i] = b;
    }
}

static void video_update(zx *m, uint32_t upto)
{
    while (m->video_chunk < CHUNKS && chunk_t(m, m->video_chunk) <= (int32_t)upto) {
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
    default: /* 128K, +2, Pentagon */
        rom = (m->port_7ffd >> 4) & 1;
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

/* ---- bus --------------------------------------------------------------- */

static inline void mem_write(zx *m, uint16_t addr, uint8_t v)
{
    uint8_t *p = m->wr[addr >> 14];
    if (!p)
        return;
    if (p == m->ram[m->screen_page] && (addr & 0x3fff) < 6912)
        video_update(m, m->t);
    p[addr & 0x3fff] = v;
}

static uint8_t b_fetch(void *ctx, uint16_t addr)
{
    zx *m = ctx;
    if (m->contended[addr >> 14])
        m->t += contention(m, m->t);
    m->t += 4;
    return m->rd[addr >> 14][addr & 0x3fff];
}

static uint8_t b_read(void *ctx, uint16_t addr)
{
    zx *m = ctx;
    if (m->contended[addr >> 14])
        m->t += contention(m, m->t);
    m->t += 3;
    return m->rd[addr >> 14][addr & 0x3fff];
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
    if ((port & 0xe0) == 0) { /* Kempston on port 0x1F */
        v = m->kempston;
        have = 1;
    }
    if (has_ay(m) && (port & 0xc002) == 0xc000) {
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

static void b_io_write(void *ctx, uint16_t port, uint8_t v)
{
    zx *m = ctx;
    io_timing(m, port);

    if (!(port & 1)) {
        video_update(m, m->t);
        audio_update(m, m->t);
        m->border = v & 7;
        m->speaker = (v >> 4) & 1;
        m->mic = (v >> 3) & 1;
    }
    if (m->model == ZX_48K)
        return;

    if (is_plus3(m)) {
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

    if ((port & 0xc002) == 0xc000) {
        m->ay.sel = v;
    } else if ((port & 0xc002) == 0x8000) {
        audio_update(m, m->t);
        ay_write(&m->ay, v);
    }
}

/* ---- setup ------------------------------------------------------------- */

static int load_rom(uint8_t *dst, const char *dir, const char *name)
{
    char path[1024];
    FILE *fp;
    size_t n;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "zx: cannot open ROM %s\n", path);
        return -1;
    }
    n = fread(dst, 1, 0x4000, fp);
    fclose(fp);
    if (n != 0x4000) {
        fprintf(stderr, "zx: ROM %s is not 16K\n", path);
        return -1;
    }
    return 0;
}

int zx_init(zx *m, zx_model model, const char *roms_dir)
{
    static const char *const roms[ZX_MODEL_COUNT][4] = {
        { "48.rom" },
        { "128-0.rom", "128-1.rom" },
        { "plus2-0.rom", "plus2-1.rom" },
        { "plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom" },
        { "plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom" },
        { "128-0.rom", "128-1.rom" },
    };
    int i;

    memset(m, 0, sizeof *m);
    m->model = model;
    m->tm = &zx_timings[model];
    for (i = 0; i < 4; i++) {
        memset(m->rom[i], 0xff, sizeof m->rom[i]);
        if (roms[model][i] && load_rom(m->rom[i], roms_dir, roms[model][i]))
            return -1;
    }
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

void zx_reset(zx *m)
{
    int i;
    z80_reset(&m->cpu);
    m->port_7ffd = 0;
    m->port_1ffd = 0;
    m->paging_locked = m->model == ZX_48K;
    update_paging(m);
    m->border = 7;
    for (i = 0; i < 8; i++)
        m->keys[i] = 0x1f;
    ay_reset(&m->ay);
    m->t = 0;
    m->video_chunk = 0;
    m->audio_t = 0;
    m->audio_frac = 0;
}

void zx_poke(zx *m, uint16_t addr, uint8_t v)
{
    uint8_t *p = m->wr[addr >> 14];
    if (p)
        p[addr & 0x3fff] = v;
}

uint8_t zx_peek(const zx *m, uint16_t addr)
{
    return m->rd[addr >> 14][addr & 0x3fff];
}

void zx_key(zx *m, int row, int bit, int down)
{
    if (down)
        m->keys[row] &= (uint8_t)~(1 << bit);
    else
        m->keys[row] |= (uint8_t)(1 << bit);
}

void zx_run_frame(zx *m)
{
    const uint32_t frame_t = m->tm->frame_t;
    m->audio_samples = 0;

    while (m->t < frame_t) {
        if (m->t < m->tm->int_len && z80_int_enabled(&m->cpu)) {
            z80_interrupt(&m->cpu, &m->bus);
            continue;
        }
        if (m->tape && m->cpu.pc == 0x0556 && m->rd[0] == m->rom[m->model == ZX_48K ? 0 : (is_plus3(m) ? 3 : 1)])
            if (zx_tape_trap(m->tape, m))
                continue;
        z80_step(&m->cpu, &m->bus);
    }

    video_update(m, 0xffffffffu >> 1);
    audio_update(m, frame_t);
    if (m->tape && zx_tape_playing(m->tape))
        zx_tape_advance(m->tape, m, (m->frames + 1) * frame_t);

    m->t -= frame_t;
    m->audio_t = 0;
    m->audio_frac -= (uint64_t)frame_t << 32;
    m->video_chunk = 0;
    m->frames++;
}
