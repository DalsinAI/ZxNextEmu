#include "zx.h"
#include "tape.h"
#include "../next/next.h"
#include "../spec256/spec256.h"

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
};

static const struct { const char *name; zx_model model; } model_names[] = {
    { "48", ZX_48K }, { "48k", ZX_48K }, { "128", ZX_128K }, { "128k", ZX_128K },
    { "plus2", ZX_PLUS2 }, { "+2", ZX_PLUS2 }, { "plus2a", ZX_PLUS2A }, { "+2a", ZX_PLUS2A },
    { "plus3", ZX_PLUS3 }, { "+3", ZX_PLUS3 }, { "pentagon", ZX_PENTAGON },
    { "next", ZX_NEXT }, { "tbblue", ZX_NEXT },
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

/* ---- state --------------------------------------------------------------- */

#define STATE_MAGIC   0x534d585au  /* "ZXMS" */
#define STATE_VERSION 1
#define ST_SD0  1
#define ST_SD1  2
#define ST_TAPE 4
#define ST_S256 8

/* Header, the zx struct, the Next's part, the tape position. The structs
 * are stored as they are in memory, so a state belongs to one build of the
 * core (native or WebAssembly); the sizes in the header catch a mismatch. */
typedef struct state_hdr {
    uint32_t magic, version, model, flags;
    uint32_t zx_size, nx_size, tape_size, reserved;
} state_hdr;

size_t zx_state_size(const zx *m)
{
    return sizeof(state_hdr) + sizeof(zx)
        + (m->model == ZX_NEXT ? nx_state_size(m) : 0)
        + zx_tape_state_size()
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
    if (m->next && m->next->sd[0])
        h.flags |= ST_SD0;
    if (m->next && m->next->sd[1])
        h.flags |= ST_SD1;
    if (m->tape)
        h.flags |= ST_TAPE;
    if (m->s256)
        h.flags |= ST_S256;
    memcpy(p, &h, sizeof h);
    p += sizeof h;
    memcpy(p, m, sizeof *m);
    p += sizeof *m;
    if (m->model == ZX_NEXT) {
        nx_state_save(m, p);
        p += h.nx_size;
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
    int has_sd[2];
    if (len < sizeof h)
        return -1;
    memcpy(&h, p, sizeof h);
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION
        || h.model != (uint32_t)m->model || h.zx_size != sizeof(zx)
        || h.nx_size != (m->model == ZX_NEXT ? nx_state_size(m) : 0)
        || h.tape_size != zx_tape_state_size() || len < zx_state_size(m)
        || !(h.flags & ST_S256) != !s256)
        return -1;
    p += sizeof h;
    /* The live machine's pointers: the timing table, the bus callbacks,
     * the picture, the Next's own state and the tape stay as they are. */
    memcpy(m, p, sizeof *m);
    p += sizeof *m;
    m->tm = tm;
    m->bus = bus;
    m->pixels = pixels;
    m->next = next;
    m->tape = tape;
    m->s256 = s256;
    if (m->model == ZX_NEXT) {
        has_sd[0] = (h.flags & ST_SD0) != 0;
        has_sd[1] = (h.flags & ST_SD1) != 0;
        nx_state_load(m, p, has_sd);
        p += h.nx_size;
    } else {
        update_paging(m);
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
    { "128-0.rom", "128-1.rom" },
    { NULL },
};

const char *zx_rom_file(zx_model model, int i)
{
    return model < ZX_MODEL_COUNT && i >= 0 && i < 4 ? rom_files[model][i] : NULL;
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
    for (i = 0; i < 4; i++) {
        memset(m->rom[i], 0xff, sizeof m->rom[i]);
        if (rom_files[model][i]) {
            if (!roms || !roms[i])
                return -1;
            memcpy(m->rom[i], roms[i], sizeof m->rom[i]);
        }
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

void zx_free(zx *m)
{
    if (m->model == ZX_NEXT)
        nx_free(m);
}

int zx_attach_sd_dev(zx *m, int slot, struct zx_blockdev *bd)
{
    if (m->model != ZX_NEXT)
        return -1;
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
    uint8_t *p;
    if (m->model == ZX_NEXT) {
        m->next->slot[addr >> 13].wr[addr & 0x1fff] = v;
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
    return m->rd[addr >> 14][addr & 0x3fff];
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
        if (m->t < m->tm->int_len && z80_int_enabled(&m->cpu)) {
            if (m->s256)
                s256_interrupt(m->s256);
            else
                z80_interrupt(&m->cpu, &m->bus);
            continue;
        }
        if (m->tape && m->cpu.pc == 0x0556 && m->rd[0] == m->rom[m->model == ZX_48K ? 0 : (is_plus3(m) ? 3 : 1)])
            if (zx_tape_trap(m->tape, m))
                continue;
        if (m->s256)
            s256_step(m->s256);
        else
            z80_step(&m->cpu, &m->bus);
    }

    video_update(m, 0xffffffffu >> 1);
    if (m->s256)
        s256_render(m->s256);
    audio_update(m, frame_t);
    if (m->tape && zx_tape_playing(m->tape))
        zx_tape_advance(m->tape, m, (m->frames + 1) * frame_t);

    m->t -= frame_t;
    m->audio_t = 0;
    m->audio_frac -= (uint64_t)frame_t << 32;
    m->video_chunk = 0;
    m->frames++;
}
