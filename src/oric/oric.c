#include "oric.h"

#include <string.h>

/* Frame layouts: lines per frame and the first of the 224 shown. */
#define LINES_50 312
#define LINES_60 264
#define FIRST_50 44
#define FIRST_60 20

/* Colour number: bit 0 red, 1 green, 2 blue. As RGB333. */
static const uint16_t palette[8] = {
    0x000, 0x1c0, 0x038, 0x1f8, 0x007, 0x1c7, 0x03f, 0x1ff
};

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    int k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* ---- ULA ------------------------------------------------------------------- */

static void line_start(oric *m)
{
    m->ink = 7;
    m->paper = 0;
    m->text = 0;
}

static void attribute(oric *m, uint8_t c)
{
    switch (c & 0x18) {
    case 0x00: m->ink = c & 7; break;
    case 0x08: m->text = c & 7; break;       /* bit 0 alternate set, 1 double height, 2 blink */
    case 0x10: m->paper = c & 7; break;
    case 0x18: m->mode = c & 7; break;       /* bit 2 HIRES, bit 1 50 Hz */
    }
}

/* The cell this cycle shows, if any: 40 cells from the start of each of
 * the 224 picture lines. */
static void ula_cycle(oric *m)
{
    uint32_t line = m->t / ORIC_LINE, col = m->t % ORIC_LINE, first, y;
    uint16_t addr, *px, fg, bg;
    uint8_t c, data;
    int i;

    if (col == 0)
        line_start(m);
    first = m->frame_t == LINES_50 * ORIC_LINE ? FIRST_50 : FIRST_60;
    if (col >= 40 || line < first || line >= first + ORIC_H)
        return;
    y = line - first;
    /* HIRES: 200 lines of bitmap at A000, then three text rows from BF68,
     * which is where row 25 of the text screen at BB80 is anyway. */
    if ((m->mode & 4) && y < 200)
        addr = (uint16_t)(0xa000 + y * 40 + col);
    else
        addr = (uint16_t)(0xbb80 + (y >> 3) * 40 + col);
    c = m->ram[addr];
    if (!(c & 0x60)) {
        attribute(m, c);
        data = 0;
    } else if ((m->mode & 4) && y < 200) {
        data = c & 0x3f;
    } else {
        uint16_t set = (uint16_t)(((m->mode & 4) ? 0x9800 : 0xb400) + ((m->text & 1) ? 0x400 : 0));
        uint32_t row = (m->text & 2) ? (y >> 1) & 7 : y & 7;
        data = m->ram[(uint16_t)(set + (c & 0x7f) * 8 + row)] & 0x3f;
    }
    if ((m->text & 4) && !(m->frames & 0x10))
        data = 0;
    fg = m->ink;
    bg = m->paper;
    if (c & 0x80) {
        fg ^= 7;
        bg ^= 7;
    }
    px = &m->screen[y][col * 6];
    for (i = 0; i < 6; i++)
        px[i] = palette[(data << i & 0x20) ? fg : bg];
}

/* ---- AY, keyboard and tape motor, all hung off the VIA's pins --------------- */

static void keyboard_sense(oric *m)
{
    int row = via_port_b(&m->via) & 7;
    int on = (m->ay.reg[7] & 0x40) && (m->keys[row] & ~m->ay.reg[14]);
    m->via.pb_in = (uint8_t)((m->via.pb_in & ~0x08) | (on ? 0x08 : 0));
}

/* BDIR (CB2) and BC1 (CA2) select what the AY does with the VIA's port
 * A: 11 latch the register number, 10 write it, 01 read it. */
static void pins_changed(void *ctx)
{
    oric *m = ctx;
    uint8_t mode = (uint8_t)(m->via.cb2 << 1 | m->via.ca2);
    uint8_t data = (uint8_t)((m->via.ora & m->via.ddra) | ~m->via.ddra);
    if (mode != m->ay_bus || data != m->ay_data) {
        m->ay_bus = mode;
        m->ay_data = data;
        if (mode == 3)
            m->ay.sel = data;
        else if (mode == 2)
            ay_write(&m->ay, data);
    }
    m->via.pa_in = mode == 1 ? ay_read(&m->ay) : 0xff;
    keyboard_sense(m);
}

/* ---- the bus ---------------------------------------------------------------- */

static void end_cycle(oric *m)
{
    via_tick(&m->via);
    ay_run(&m->ay, 1);
    m->audio_frac += ORIC_AUDIO_RATE;
    if (m->audio_frac >= ORIC_HZ) {
        int32_t out[3], s;
        m->audio_frac -= ORIC_HZ;
        ay_take(&m->ay, out);
        s = (out[0] + out[1] + out[2]) / 4;
        if (m->audio_samples < (int)(sizeof m->audio / sizeof m->audio[0] / 2)) {
            m->audio[m->audio_samples * 2] = (int16_t)s;
            m->audio[m->audio_samples * 2 + 1] = (int16_t)s;
            m->audio_samples++;
        }
    }
    if (m->tape.data)
        oric_tape_tick(m);
    m->cpu.irq_line = (uint8_t)via_irq(&m->via);
    m->t++;
}

static uint8_t bus_read(void *ctx, uint16_t a)
{
    oric *m = ctx;
    uint8_t v;
    ula_cycle(m);
    if (a >= 0xc000)
        v = m->rom[a - 0xc000];
    else if ((a & 0xff00) == 0x0300)
        v = via_read(&m->via, a & 15);
    else
        v = m->ram[a];
    end_cycle(m);
    return v;
}

static void bus_write(void *ctx, uint16_t a, uint8_t v)
{
    oric *m = ctx;
    ula_cycle(m);
    if ((a & 0xff00) == 0x0300)
        via_write(&m->via, a & 15, v);
    else if (a < 0xc000)
        m->ram[a] = v;
    end_cycle(m);
}

/* ---- the machine -------------------------------------------------------------- */

static void wire(oric *m)
{
    m->bus.ctx = m;
    m->bus.read = bus_read;
    m->bus.write = bus_write;
    m->via.changed = pins_changed;
    m->via.ctx = m;
}

int oric_create(oric *m, oric_model model, const uint8_t *rom, size_t rom_len)
{
    if (model >= ORIC_MODEL_COUNT || !rom || rom_len != sizeof m->rom)
        return -1;
    memset(m, 0, sizeof *m);
    m->model = model;
    memcpy(m->rom, rom, sizeof m->rom);
    m->rom_crc = crc32(m->rom, sizeof m->rom);
    m->tape_fast = 1;
    wire(m);
    oric_power_on(m);
    return 0;
}

void oric_free(oric *m)
{
    oric_tape_eject(m);
}

void oric_reset(oric *m)
{
    m6502_reset(&m->cpu);
    via_reset(&m->via);
    ay_reset(&m->ay);
    m->ay_bus = 0xff;
    pins_changed(m);
}

void oric_power_on(oric *m)
{
    memset(m->ram, 0, sizeof m->ram);
    memset(m->keys, 0, sizeof m->keys);
    m6502_init(&m->cpu);
    m->via.pa_in = m->via.pb_in = 0xff;
    m->via.ca1 = m->via.cb1 = 1;
    m->mode = 0x02;                       /* text, 50 Hz */
    m->t = 0;
    m->frame_t = LINES_50 * ORIC_LINE;
    m->frames = 0;
    oric_reset(m);
}

void oric_nmi(oric *m)
{
    m->cpu.nmi_line = 1;
}

void oric_key(oric *m, int row, int col, int down)
{
    if (row < 0 || row > 7 || col < 0 || col > 7)
        return;
    if (down)
        m->keys[row] |= (uint8_t)(1 << col);
    else
        m->keys[row] &= (uint8_t)~(1 << col);
    keyboard_sense(m);
}

uint8_t oric_peek(const oric *m, uint16_t a)
{
    return a >= 0xc000 ? m->rom[a - 0xc000] : m->ram[a];
}

void oric_poke(oric *m, uint16_t a, uint8_t v)
{
    if (a < 0xc000)
        m->ram[a] = v;
}

void oric_run_frame(oric *m)
{
    m->audio_samples = 0;
    while (m->t < m->frame_t) {
        if (m->tape_fast && m->tape.data && oric_tape_trap(m))
            continue;
        m6502_step(&m->cpu, &m->bus);
    }
    /* The NMI button is held for at most a frame. */
    m->cpu.nmi_line = 0;
    m->t -= m->frame_t;
    m->frames++;
    m->frame_t = ((m->mode & 2) ? LINES_50 : LINES_60) * ORIC_LINE;
}

/* ---- keys for typed text -------------------------------------------------------- */

/* keys[row] bit col: the Oric's 8x8 matrix. 0 = no key. */
static const char matrix[8][8] = {
    { '7', 'n', '5', 'v', 0, '1', 'x', '3' },
    { 'j', 't', 'r', 'f', 0, 27, 'q', 'd' },
    { 'm', '6', 'b', '4', 0, 'z', '2', 'c' },
    { 'k', '9', ';', '-', 0, 0, '\\', '\'' },
    { ' ', ',', '.', 0, 0, 0, 0, 0 },
    { 'u', 'i', 'o', 'p', 0, 127, ']', '[' },
    { 'y', 'h', 'g', 'e', 0, 'a', 's', 'w' },
    { '8', 'l', '0', '/', 0, '\n', 0, '=' },
};

static const char *const shifted = "!@#$%^&*()\":<>?+{}|";
static const char *const unshifted = "1234567890';,./=[]\\";

int oric_char_key(int ch, int *row, int *col, int *shift)
{
    const char *s;
    int r, c;
    *shift = 0;
    if (ch == '\r')
        ch = '\n';
    if (ch >= 'A' && ch <= 'Z')
        ch += 'a' - 'A';            /* the ROM starts in CAPS: plain keys give capitals */
    else if (ch == 8)
        ch = 127;
    else if (ch && (s = strchr(shifted, ch)) != NULL) {
        *shift = 1;
        ch = unshifted[s - shifted];
    }
    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++)
            if (ch && matrix[r][c] == ch) {
                *row = r;
                *col = c;
                return 1;
            }
    return 0;
}

/* ---- state ------------------------------------------------------------------------ */

#define STATE_MAGIC 0x4349524fu    /* "ORIC" */
#define STATE_VERSION 1

typedef struct state_hdr {
    uint32_t magic, version, model, size, tape_len, reserved[3];
} state_hdr;

size_t oric_state_size(const oric *m)
{
    (void)m;
    return sizeof(state_hdr) + sizeof(oric);
}

int oric_state_save(const oric *m, void *buf, size_t len)
{
    state_hdr h;
    if (len < oric_state_size(m))
        return -1;
    memset(&h, 0, sizeof h);
    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.model = (uint32_t)m->model;
    h.size = sizeof(oric);
    h.tape_len = m->tape.len;
    memcpy(buf, &h, sizeof h);
    memcpy((uint8_t *)buf + sizeof h, m, sizeof *m);
    return 0;
}

int oric_state_load(oric *m, const void *buf, size_t len)
{
    state_hdr h;
    uint8_t *tape = m->tape.data;
    uint32_t tape_len = m->tape.len;
    if (len < oric_state_size(m))
        return -1;
    memcpy(&h, buf, sizeof h);
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION || h.model != (uint32_t)m->model
        || h.size != sizeof(oric))
        return -1;
    /* Pointers stay those of the live machine; the tape stays the one in
     * the deck, at the saved position if it is the same length. */
    memcpy(m, (const uint8_t *)buf + sizeof h, sizeof *m);
    wire(m);
    m->tape.data = tape;
    m->tape.len = tape_len;
    if (h.tape_len != tape_len) {
        oric_tape tmp = m->tape;
        memset(&m->tape, 0, sizeof m->tape);
        m->tape.data = tmp.data;
        m->tape.len = tmp.len;
        m->tape.level = 1;
    }
    return 0;
}
