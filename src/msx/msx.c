/*
 * MSX1 and MSX2 machines (msx.h): memory and slots, I/O, the frame loop,
 * keyboard, joysticks and mouse, sound and saved state.
 */
#include "msx.h"
#include "../storage/blockdev.h"

#include <stdlib.h>
#include <string.h>

#define NONE 0xffffffffu

const msx_model msx_models[MSX_MODELS] = {
    { "msx1", "MSX1", VDP_TMS9918, 0, 64 },
    { "msx1eu", "MSX1 (50 Hz)", VDP_TMS9929, 0, 64 },
    { "msx2", "MSX2", VDP_V9938, 1, 1024 },
    { "msx2eu", "MSX2 (50 Hz)", VDP_V9938, 1, 1024 },
};

/* What an empty slot reads as. */
static uint8_t ff_bank[0x2000];

static void audio_update(msx *m, uint32_t upto);

/* ---- slots -------------------------------------------------------------- */

static void map_bank(msx *m, int b)
{
    int page = b >> 1, ps = (m->slot_reg >> (page * 2)) & 3;
    int ss = m->expanded[ps] ? (m->sub_reg[ps] >> (page * 2)) & 3 : 0;
    int dev = m->slot_dev[ps][ss];
    uint32_t off = (uint32_t)(b & 1) * 0x2000;
    const uint8_t *rd = ff_bank;
    uint8_t *wr = NULL;

    switch (dev) {
    case DEV_BIOS:
        if (page < 2)
            rd = m->rom_main + page * 0x4000 + off;
        else if (page == 2 && m->rom_logo)
            rd = m->rom_logo + off;
        break;
    case DEV_SUB:
        if (page == 0)
            rd = m->rom_sub + off;
        break;
    case DEV_RAM:
        rd = wr = m->ram + (uint32_t)b * 0x2000;
        break;
    case DEV_MAPPER: {
        uint32_t seg = m->mapper[page] & ((m->ram_size >> 14) - 1);
        rd = wr = m->ram + seg * 0x4000 + off;
        break;
    }
    case DEV_CART0:
    case DEV_CART1:
        rd = cart_bank(&m->cart[dev - DEV_CART0], b);
        break;
    case DEV_EXT:
        if (m->ext_type == EXT_DISK) {
            /* The FDC's registers are at xFF8-xFFF of every page. */
            rd = (b & 1) ? NULL : page == 1 ? m->rom_ext + off : ff_bank;
        } else {
            uint32_t bank = (uint32_t)ide_bank(&m->ide, m->ext_len >> 14);
            if ((b & 1) && ide_regs_on(&m->ide))
                rd = NULL;      /* 7C00-7EFF and its mirrors */
            else
                rd = page == 1 ? m->rom_ext + bank * 0x4000 + off : ff_bank;
        }
        break;
    }
    m->rd[b] = rd;
    m->wr[b] = wr;
    m->dev[b] = (uint8_t)dev;
}

static void update_map(msx *m)
{
    int b;
    for (b = 0; b < 8; b++)
        map_bank(m, b);
    m->page3_exp = m->expanded[m->slot_reg >> 6];
}

/* T-states since the machine was created. */
static uint64_t now(const msx *m)
{
    return m->time_base + m->t;
}

static uint8_t slow_read(msx *m, uint16_t a)
{
    uint8_t v;
    switch (m->dev[a >> 13]) {
    case DEV_CART0:
    case DEV_CART1:
        return cart_read(&m->cart[m->dev[a >> 13] - DEV_CART0], a);
    case DEV_EXT:
        if (m->ext_type == EXT_DISK) {
            m->fdc.now = now(m);
            if (fdc_read(&m->fdc, a, &v))
                return v;
            return (a >> 14) == 1 ? m->rom_ext[a & 0x3fff] : 0xff;
        }
        if (ide_read(&m->ide, a, &v))
            return v;
        if ((a >> 14) == 1)
            return m->rom_ext[(uint32_t)ide_bank(&m->ide, m->ext_len >> 14) * 0x4000 + (a & 0x3fff)];
        return 0xff;
    }
    return 0xff;
}

static void slow_write(msx *m, uint16_t a, uint8_t v)
{
    switch (m->dev[a >> 13]) {
    case DEV_CART0:
    case DEV_CART1: {
        msx_cart *c = &m->cart[m->dev[a >> 13] - DEV_CART0];
        if (c->type == CART_KONAMI_SCC)
            audio_update(m, m->t);
        if (cart_write(c, a, v))
            update_map(m);
        break;
    }
    case DEV_EXT:
        if (m->ext_type == EXT_DISK) {
            m->fdc.now = now(m);
            fdc_write(&m->fdc, a, v);
        } else if (ide_write(&m->ide, a, v)) {
            update_map(m);
        }
        break;
    }
}

static inline uint8_t mem_read(msx *m, uint16_t a)
{
    const uint8_t *p;
    if (a == 0xffff && m->page3_exp)
        return (uint8_t)~m->sub_reg[m->slot_reg >> 6];
    p = m->rd[a >> 13];
    return p ? p[a & 0x1fff] : slow_read(m, a);
}

static inline void mem_write(msx *m, uint16_t a, uint8_t v)
{
    uint8_t *p;
    if (a == 0xffff && m->page3_exp) {
        m->sub_reg[m->slot_reg >> 6] = v;
        update_map(m);
        return;
    }
    p = m->wr[a >> 13];
    if (p)
        p[a & 0x1fff] = v;
    else
        slow_write(m, a, v);
}

/* ---- sound -------------------------------------------------------------- */

#define CLICK_LEVEL 6000

static void audio_accumulate(msx *m, uint32_t dt)
{
    int i;
    m->click_acc += ((m->ppi_c & 0x80) ? CLICK_LEVEL : 0) * (int32_t)dt;
    m->psg_half += dt;
    ay_run(&m->psg, m->psg_half >> 1);   /* the PSG runs at CPU / 2 */
    m->psg_half &= 1;
    for (i = 0; i < 2; i++)
        if (m->cart[i].type == CART_KONAMI_SCC)
            scc_run(&m->cart[i].scc, dt);
}

static void audio_update(msx *m, uint32_t upto)
{
    const uint64_t step = ((uint64_t)MSX_CPU_HZ << 32) / MSX_AUDIO_RATE;
    for (;;) {
        uint32_t next = (uint32_t)(m->audio_frac >> 32);
        uint32_t span = (uint32_t)(step >> 32);
        int32_t ay[3], v;
        int i;
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

        ay_take(&m->psg, ay);
        v = (ay[0] + ay[1] + ay[2]) / 4 + m->click_acc / (int32_t)span;
        m->click_acc = 0;
        for (i = 0; i < 2; i++)
            if (m->cart[i].type == CART_KONAMI_SCC)
                v += scc_take(&m->cart[i].scc) / 2;
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        if (m->audio_samples < (int)(sizeof m->audio / sizeof m->audio[0] / 2)) {
            m->audio[m->audio_samples * 2] = (int16_t)v;
            m->audio[m->audio_samples * 2 + 1] = (int16_t)v;
            m->audio_samples++;
        }
    }
}

/* ---- joysticks and the mouse ------------------------------------------- */

#define MOUSE_TIMEOUT (MSX_CPU_HZ / 666)   /* 1.5 ms without a strobe: start over */

static uint8_t joy_read(msx *m, int port)
{
    uint8_t v = 0x3f, s;
    if (port == 0 && m->mouse.present) {
        msx_mouse *ms = &m->mouse;
        static const uint8_t shift[4] = { 4, 0, 4, 0 };
        uint8_t b = ms->phase < 2 ? ms->lx : ms->ly;
        v = (uint8_t)((b >> shift[ms->phase & 3]) & 15);
        if (!(ms->buttons & 1))
            v |= 0x10;
        if (!(ms->buttons & 2))
            v |= 0x20;
        return v;
    }
    s = m->joy[port];
    if (s & 0x08) v &= ~0x01;  /* up */
    if (s & 0x04) v &= ~0x02;  /* down */
    if (s & 0x02) v &= ~0x04;  /* left */
    if (s & 0x01) v &= ~0x08;  /* right */
    if (s & 0x10) v &= ~0x10;  /* A */
    if (s & 0x20) v &= ~0x20;  /* B */
    return v;
}

static int clamp127(int32_t v)
{
    return v > 127 ? 127 : v < -127 ? -127 : (int)v;
}

/* PSG R15 written: pin 8 of port 1 clocks the mouse through its nibbles. */
static void mouse_strobe(msx *m, uint8_t r15)
{
    msx_mouse *ms = &m->mouse;
    uint8_t pin8 = (r15 >> 4) & 1;
    uint64_t t = now(m);
    if (pin8 == ms->pin8)
        return;
    ms->pin8 = pin8;
    if (t - ms->last > MOUSE_TIMEOUT)
        ms->phase = 0;
    else
        ms->phase = (uint8_t)((ms->phase + 1) & 3);
    ms->last = t;
    if (ms->phase == 0) {
        /* An MSX mouse counts left and up as positive. */
        int dx = clamp127(ms->acc_x), dy = clamp127(ms->acc_y);
        ms->acc_x -= dx;
        ms->acc_y -= dy;
        ms->lx = (uint8_t)(-dx);
        ms->ly = (uint8_t)(-dy);
    }
}

void msx_joystick(msx *m, int port, uint8_t state)
{
    if (port < 0 || port > 1)
        return;
    m->joy[port] = state;
    if (port == 0)
        m->mouse.present = 0;
}

void msx_mouse_move(msx *m, int dx, int dy, int buttons)
{
    m->mouse.present = 1;
    m->mouse.acc_x += dx;
    m->mouse.acc_y += dy;
    m->mouse.buttons = (uint8_t)(buttons & 3);
}

void msx_key(msx *m, int row, int bit, int down)
{
    if (row < 0 || row > 10 || bit < 0 || bit > 7)
        return;
    if (down)
        m->keys[row] &= (uint8_t)~(1 << bit);
    else
        m->keys[row] |= (uint8_t)(1 << bit);
}

/* ---- I/O ---------------------------------------------------------------- */

static void rtc_catch_up(msx *m)
{
    uint64_t t = now(m);
    if (t > m->rtc_last) {
        rtc_run(&m->rtc, (uint32_t)(t - m->rtc_last));
        m->rtc_last = t;
    }
}

static uint8_t psg_read(msx *m)
{
    switch (m->psg.sel) {
    case 14:
        /* Joystick port (R15 bit 6 picks it), bit 6 keyboard layout,
         * bit 7 the cassette input. */
        return (uint8_t)(0x40 | joy_read(m, (m->psg.reg[15] >> 6) & 1));
    case 15:
        return m->psg.reg[15];
    default:
        return ay_read(&m->psg);
    }
}

static uint8_t b_io_read(void *ctx, uint16_t port)
{
    msx *m = ctx;
    uint8_t p = (uint8_t)port;
    m->t += 4;
    if ((p & 0xf8) == 0x98) {
        uint8_t v = vdp_read(&m->vdp, p & 3, m->t * 6);
        m->next_event = 0;
        return v;
    }
    if ((p & 0xfc) == 0xa0)
        return (p & 3) == 2 ? psg_read(m) : 0xff;
    if ((p & 0xfc) == 0xa8) {
        switch (p & 3) {
        case 0: return m->slot_reg;
        case 1: return (m->ppi_c & 15) < 11 ? m->keys[m->ppi_c & 15] : 0xff;
        case 2: return m->ppi_c;
        default: return 0xff;
        }
    }
    if (m->model->msx2) {
        if (p == 0xb5) {
            rtc_catch_up(m);
            return rtc_read(&m->rtc);
        }
        if (p >= 0xfc)
            return (uint8_t)(m->mapper[p & 3] | ~((m->ram_size >> 14) - 1));
    }
    return 0xff;
}

static void b_io_write(void *ctx, uint16_t port, uint8_t v)
{
    msx *m = ctx;
    uint8_t p = (uint8_t)port;
    m->t += 4;
    if ((p & 0xf8) == 0x98) {
        vdp_write(&m->vdp, p & 3, v, m->t * 6);
        m->next_event = 0;
        return;
    }
    if ((p & 0xfc) == 0xa0) {
        switch (p & 3) {
        case 0:
            m->psg.sel = v;
            break;
        case 1:
            audio_update(m, m->t);
            if (m->psg.sel == 15)
                mouse_strobe(m, v);
            ay_write(&m->psg, v);
            break;
        }
        return;
    }
    if ((p & 0xfc) == 0xa8) {
        switch (p & 3) {
        case 0:
            m->slot_reg = v;
            update_map(m);
            break;
        case 2:
            audio_update(m, m->t);
            m->ppi_c = v;
            break;
        case 3:
            if (!(v & 0x80)) {
                audio_update(m, m->t);
                if (v & 1)
                    m->ppi_c |= (uint8_t)(1 << ((v >> 1) & 7));
                else
                    m->ppi_c &= (uint8_t)~(1 << ((v >> 1) & 7));
            }
            break;
        }
        return;
    }
    if (m->model->msx2) {
        if (p == 0xb4 || p == 0xb5) {
            rtc_catch_up(m);
            rtc_write(&m->rtc, p & 1, v);
        } else if (p >= 0xfc) {
            m->mapper[p & 3] = v;
            map_bank(m, (p & 3) * 2);
            map_bank(m, (p & 3) * 2 + 1);
        }
    }
}

/* ---- the CPU's bus: an extra wait state on every M1 ----------------------- */

static uint8_t b_fetch(void *ctx, uint16_t a)
{
    msx *m = ctx;
    m->t += 5;
    return mem_read(m, a);
}

static uint8_t b_read(void *ctx, uint16_t a)
{
    msx *m = ctx;
    m->t += 3;
    return mem_read(m, a);
}

static void b_write(void *ctx, uint16_t a, uint8_t v)
{
    msx *m = ctx;
    m->t += 3;
    mem_write(m, a, v);
}

static void b_internal(void *ctx, uint16_t a, unsigned n)
{
    msx *m = ctx;
    (void)a;
    m->t += n;
}

static void b_tick(void *ctx, unsigned n)
{
    msx *m = ctx;
    m->t += n;
}

static uint8_t b_int_ack(void *ctx)
{
    msx *m = ctx;
    m->t += 1;
    return 0xff;
}

/* ---- typing ------------------------------------------------------------- */

/* MSX international keyboard: the row and bit of each character, and
 * whether SHIFT is held. */
static int char_key(int ch, int *row, int *bit, int *shift)
{
    static const char *const rows[9] = {
        "01234567", "89-=\\[];", "'`,./\001ab", "cdefghij", "klmnopqr", "stuvwxyz",
        "\x01\x01\x01\x01\x01\x01\x01\x01", "\x01\x01\x1b\t\x01\b\x01\n", " \x01\x01\x7f\x01\x01\x01\x01",
    };
    static const char *const shifted = ")!@#$%^&*(_+|{}:\"~<>?";
    static const char *const base = "0123456789-=\\[];'`,./";
    const char *s;
    int r, b;
    *shift = 0;
    if (ch == '\r')
        ch = '\n';
    if (ch >= 'A' && ch <= 'Z') {
        *shift = 1;
        ch += 'a' - 'A';
    } else if (ch && (s = strchr(shifted, ch)) != NULL) {
        *shift = 1;
        ch = base[s - shifted];
    }
    for (r = 0; r < 9; r++)
        for (b = 0; b < 8; b++)
            if (rows[r][b] == ch && ch != 1) {
                *row = r;
                *bit = b;
                return 1;
            }
    return 0;
}

void msx_type(msx *m, const char *text, int delay_frames)
{
    size_t n = strlen(text);
    if (n > sizeof m->typer.buf)
        n = sizeof m->typer.buf;
    memcpy(m->typer.buf, text, n);
    m->typer.len = (uint32_t)n;
    m->typer.pos = 0;
    m->typer.phase = 0;
    m->typer.wait = delay_frames;
}

int msx_typing(const msx *m)
{
    return m->typer.pos < m->typer.len;
}

/* Each key is held for 3 frames and released for 3: the BIOS scans the
 * keyboard on every interrupt. */
static void type_frame(msx *m)
{
    msx_typer *ty = &m->typer;
    int row, bit, shift;
    if (ty->pos >= ty->len)
        return;
    if (ty->wait > 0) {
        ty->wait--;
        return;
    }
    if (ty->phase == 0 || ty->phase == 3)
        memset(m->keys, 0xff, sizeof m->keys);
    if (ty->phase == 0 && char_key((unsigned char)ty->buf[ty->pos], &row, &bit, &shift)) {
        msx_key(m, row, bit, 1);
        if (shift)
            msx_key(m, 6, 0, 1);
    }
    if (++ty->phase == 6) {
        ty->phase = 0;
        ty->pos++;
    }
}

/* ---- the frame ---------------------------------------------------------- */

void msx_run_frame(msx *m)
{
    vdp *v = &m->vdp;
    vdp_frame_start(v);
    m->frame_t = v->frame_ticks / 6;
    m->audio_samples = 0;
    m->next_event = 0;
    type_frame(m);

    while (m->t < m->frame_t) {
        if (m->t >= m->next_event) {
            uint32_t e;
            vdp_sync(v, m->t * 6);
            e = vdp_next_event(v);
            m->next_event = e == NONE ? NONE : (e + 5) / 6;
        }
        if (vdp_irq(v) && z80_int_enabled(&m->cpu)) {
            z80_interrupt(&m->cpu, &m->bus);
            continue;
        }
        z80_step(&m->cpu, &m->bus);
    }

    vdp_frame_end(v);
    audio_update(m, m->frame_t);
    rtc_catch_up(m);
    m->t -= m->frame_t;
    m->audio_t = 0;
    m->audio_frac -= (uint64_t)m->frame_t << 32;
    m->time_base += m->frame_t;
    m->frames++;
    if (m->ext_type == EXT_DISK)
        fdc_frame(&m->fdc);
    else if (m->ext_type == EXT_IDE)
        ide_frame(&m->ide);
}

/* ---- setup -------------------------------------------------------------- */

static uint8_t *copy_rom(const uint8_t *src, uint32_t len, uint32_t size)
{
    uint8_t *p = malloc(size);
    if (!p)
        return NULL;
    memset(p, 0xff, size);
    memcpy(p, src, len < size ? len : size);
    return p;
}

/* Memory as it comes up on a Philips NMS 8250: 00 FF repeated, then FF 00,
 * in 256-byte halves. */
static void ram_pattern(uint8_t *ram, uint32_t size)
{
    uint32_t i;
    for (i = 0; i < size; i++)
        ram[i] = (uint8_t)(((i & 1) ^ ((i >> 8) & 1)) ? 0xff : 0x00);
}

int msx_create(msx *m, const msx_model *model, const uint8_t *main_rom, uint32_t main_len,
               const uint8_t *sub, uint32_t sub_len, const uint8_t *logo, uint32_t logo_len,
               const uint8_t *ext, uint32_t ext_len)
{
    memset(m, 0, sizeof *m);
    memset(ff_bank, 0xff, sizeof ff_bank);
    m->model = model;
    if (!main_rom || main_len != 0x8000)
        return -1;
    if (model->msx2 && (!sub || sub_len != 0x4000))
        return -1;
    if (logo && logo_len && logo_len != 0x4000)
        return -1;
    m->rom_main = copy_rom(main_rom, main_len, 0x8000);
    if (model->msx2)
        m->rom_sub = copy_rom(sub, sub_len, 0x4000);
    if (logo && logo_len)
        m->rom_logo = copy_rom(logo, logo_len, 0x4000);
    if (ext && ext_len) {
        uint32_t size = 0x4000;
        if (ext_len <= 0x8000) {
            m->ext_type = EXT_DISK;
            ext_len = 0x4000;
        } else {
            m->ext_type = EXT_IDE;
            while (size < ext_len)
                size <<= 1;
            ext_len = size;
        }
        m->rom_ext = copy_rom(ext, ext_len, ext_len);
        m->ext_len = ext_len;
    }
    m->ram_size = model->ram_kb * 1024u;
    m->ram = malloc(m->ram_size);
    m->vram = malloc(model->msx2 ? 0x20000 : 0x4000);
    if (!m->rom_main || (model->msx2 && !m->rom_sub) || !m->ram || !m->vram
        || (ext && ext_len && !m->rom_ext)) {
        msx_free(m);
        return -1;
    }

    m->slot_dev[0][0] = DEV_BIOS;
    m->slot_dev[1][0] = DEV_CART0;
    m->slot_dev[2][0] = DEV_CART1;
    m->expanded[3] = 1;
    m->slot_dev[3][0] = model->msx2 ? DEV_SUB : DEV_RAM;
    m->slot_dev[3][2] = model->msx2 ? DEV_MAPPER : DEV_NONE;
    m->slot_dev[3][3] = m->ext_type ? DEV_EXT : DEV_NONE;

    vdp_init(&m->vdp, model->vdp, m->vram);
    rtc_init(&m->rtc, MSX_CPU_HZ);
    fdc_init(&m->fdc, MSX_CPU_HZ);
    ide_init(&m->ide);

    z80_init(&m->cpu, 0);
    m->bus.ctx = m;
    m->bus.fetch = b_fetch;
    m->bus.read = b_read;
    m->bus.write = b_write;
    m->bus.io_read = b_io_read;
    m->bus.io_write = b_io_write;
    m->bus.internal = b_internal;
    m->bus.tick = b_tick;
    m->bus.int_ack = b_int_ack;
    msx_reset(m, 1);
    return 0;
}

void msx_free(msx *m)
{
    cart_free(&m->cart[0]);
    cart_free(&m->cart[1]);
    fdc_free(&m->fdc);
    ide_free(&m->ide);
    free(m->rom_main);
    free(m->rom_logo);
    free(m->rom_sub);
    free(m->rom_ext);
    free(m->ram);
    free(m->vram);
    m->rom_main = m->rom_logo = m->rom_sub = m->rom_ext = m->ram = m->vram = NULL;
}

void msx_reset(msx *m, int power)
{
    int i;
    if (power) {
        ram_pattern(m->ram, m->ram_size);
        memset(m->vram, 0, m->model->msx2 ? 0x20000 : 0x4000);
        memset(&m->typer, 0, sizeof m->typer);
        memset(&m->mouse, 0, sizeof m->mouse);
        m->t = 0;
        m->audio_t = 0;
        m->audio_frac = 0;
    }
    z80_reset(&m->cpu);
    m->slot_reg = 0;
    memset(m->sub_reg, 0, sizeof m->sub_reg);
    for (i = 0; i < 4; i++)
        m->mapper[i] = (uint8_t)(3 - i);
    m->ppi_c = 0x50;
    memset(m->keys, 0xff, sizeof m->keys);
    ay_reset(&m->psg);
    vdp_reset(&m->vdp);
    rtc_reset(&m->rtc);
    for (i = 0; i < 2; i++)
        if (m->cart[i].rom)
            cart_reset(&m->cart[i]);
    fdc_reset(&m->fdc);
    ide_reset(&m->ide);
    m->ide.control = 0xff;
    update_map(m);
}

/* ---- media -------------------------------------------------------------- */

int msx_cart_insert(msx *m, int slot, const uint8_t *data, uint32_t len, const char *name)
{
    if (slot < 0 || slot > 1 || cart_load(&m->cart[slot], data, len, name))
        return -1;
    update_map(m);
    return 0;
}

void msx_cart_eject(msx *m, int slot)
{
    if (slot < 0 || slot > 1)
        return;
    cart_free(&m->cart[slot]);
    update_map(m);
}

int msx_disk_insert(msx *m, int drive, zx_blockdev *bd)
{
    if (drive < 0 || drive > 1)
        return -1;
    fdc_insert(&m->fdc, drive, bd);
    return 0;
}

int msx_ide_attach(msx *m, int unit, zx_blockdev *bd)
{
    if (unit < 0 || unit > 1)
        return -1;
    ide_attach(&m->ide, unit, bd);
    return 0;
}

/* ---- debugging ---------------------------------------------------------- */

uint8_t msx_peek(const msx *m, uint16_t addr)
{
    const uint8_t *p;
    if (addr == 0xffff && m->page3_exp)
        return (uint8_t)~m->sub_reg[m->slot_reg >> 6];
    p = m->rd[addr >> 13];
    return p ? p[addr & 0x1fff] : 0xff;
}

void msx_poke(msx *m, uint16_t addr, uint8_t v)
{
    uint8_t *p = m->wr[addr >> 13];
    if (p)
        p[addr & 0x1fff] = v;
}

/* ---- state -------------------------------------------------------------- */

#define STATE_MAGIC   0x5358534du  /* "MSXS" */
#define STATE_VERSION 1

/* The machine struct as it is in memory (so a state belongs to one build
 * of the core), then RAM and VRAM. Media stay attached and are not in it. */
typedef struct state_hdr {
    uint32_t magic, version, model, msx_size;
    uint32_t ram_size, vram_size, ext_len, reserved;
} state_hdr;

static uint32_t vram_size(const msx *m)
{
    return m->model->msx2 ? 0x20000 : 0x4000;
}

size_t msx_state_size(const msx *m)
{
    return sizeof(state_hdr) + sizeof(msx) + m->ram_size + vram_size(m);
}

int msx_state_save(const msx *m, void *buf, size_t len)
{
    uint8_t *p = buf;
    state_hdr h;
    if (len < msx_state_size(m))
        return -1;
    memset(&h, 0, sizeof h);
    h.magic = STATE_MAGIC;
    h.version = STATE_VERSION;
    h.model = (uint32_t)(m->model - msx_models);
    h.msx_size = sizeof(msx);
    h.ram_size = m->ram_size;
    h.vram_size = vram_size(m);
    h.ext_len = m->ext_len;
    memcpy(p, &h, sizeof h);
    p += sizeof h;
    memcpy(p, m, sizeof *m);
    p += sizeof *m;
    memcpy(p, m->ram, m->ram_size);
    p += m->ram_size;
    memcpy(p, m->vram, vram_size(m));
    return 0;
}

int msx_state_load(msx *m, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    state_hdr h;
    msx live;
    int i;
    if (len < sizeof h)
        return -1;
    memcpy(&h, p, sizeof h);
    if (h.magic != STATE_MAGIC || h.version != STATE_VERSION
        || h.model != (uint32_t)(m->model - msx_models) || h.msx_size != sizeof(msx)
        || h.ram_size != m->ram_size || h.vram_size != vram_size(m) || h.ext_len != m->ext_len
        || len < msx_state_size(m))
        return -1;
    p += sizeof h;
    /* Keep what belongs to this machine and its media: ROMs, memory,
     * the bus, cartridges' ROMs and the disks. */
    memcpy(&live, m, sizeof live);
    memcpy(m, p, sizeof *m);
    p += sizeof *m;
    m->model = live.model;
    m->bus = live.bus;
    m->rom_main = live.rom_main;
    m->rom_logo = live.rom_logo;
    m->rom_sub = live.rom_sub;
    m->rom_ext = live.rom_ext;
    m->ram = live.ram;
    m->vram = live.vram;
    m->vdp.vram = live.vram;
    for (i = 0; i < 2; i++) {
        msx_cart *c = &m->cart[i], *l = &live.cart[i];
        c->rom = l->rom;
        c->size = l->size;
        c->pages = l->pages;
        c->type = l->type;
        c->base = l->base;
        m->fdc.disk[i] = live.fdc.disk[i];
        m->ide.bus.dev[i].bd = live.ide.bus.dev[i].bd;
    }
    memcpy(m->ram, p, m->ram_size);
    p += m->ram_size;
    memcpy(m->vram, p, vram_size(m));
    update_map(m);
    return 0;
}
