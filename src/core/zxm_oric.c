/*
 * zxm over the Orics (src/oric).
 */
#include "zxm_impl.h"
#include "../oric/oric.h"

#include <string.h>

/* Text typed one key at a time: held 4 frames, released 4, so the ROM's
 * 100 Hz scan sees each press and each release. */
typedef struct oric_typer {
    char buf[1024];
    uint32_t pos, len;
    int32_t phase, wait;
} oric_typer;

typedef struct zxm_oric {
    struct zxm base;
    oric m;
    oric_typer typer;
} zxm_oric;

#define OR(p) ((zxm_oric *)(p))
#define COR(p) ((const zxm_oric *)(p))

static const char *const roms_oric1[] = { "basic10.rom", NULL };
static const char *const roms_atmos[] = { "basic11b.rom", NULL };

static const zxm_slot slots[] = {
    { "tape", "Tape", ZXM_SLOT_TAPE, ".tap" },
    { NULL, NULL, 0, NULL },
};

static const zxm_input_desc inputs[] = {
    { "keyboard", ZXM_IN_ORIC_KEYBOARD },
    { NULL, 0 },
};

/* machines[i] is model i. */
static zxm_info machines[] = {
    { "oric1", "Oric-1", ORIC_HZ, 312 * ORIC_LINE, ORIC_W, ORIC_H, ZXM_RGB333, ORIC_AUDIO_RATE,
      roms_oric1, slots, inputs, "6502", 0 },
    { "atmos", "Oric Atmos", ORIC_HZ, 312 * ORIC_LINE, ORIC_W, ORIC_H, ZXM_RGB333, ORIC_AUDIO_RATE,
      roms_atmos, slots, inputs, "6502", 0 },
};

#define MACHINES ((int)(sizeof machines / sizeof machines[0]))

static size_t size(const zxm_info *info)
{
    (void)info;
    return sizeof(zxm_oric);
}

static int create(zxm *b, const zxm_rom_set *roms)
{
    zxm_oric *m = OR(b);
    memset(&m->typer, 0, sizeof m->typer);
    return oric_create(&m->m, (oric_model)(b->info - machines), roms->rom[0], roms->len[0]);
}

static void destroy(zxm *b) { oric_free(&OR(b)->m); }

static void reset(zxm *b, zxm_reset_kind kind)
{
    zxm_oric *m = OR(b);
    switch (kind) {
    case ZXM_POWER_ON:
        oric_power_on(&m->m);
        memset(&m->typer, 0, sizeof m->typer);
        break;
    case ZXM_RESET:
        oric_reset(&m->m);
        break;
    case ZXM_NMI:
        oric_nmi(&m->m);
        break;
    }
}

static void type_frame(zxm_oric *z)
{
    oric_typer *ty = &z->typer;
    int row, col, shift;
    if (ty->pos >= ty->len)
        return;
    if (ty->wait > 0) {
        ty->wait--;
        return;
    }
    if (ty->phase == 0 || ty->phase == 4)
        memset(z->m.keys, 0, sizeof z->m.keys);
    if (ty->phase == 0 && oric_char_key((unsigned char)ty->buf[ty->pos], &row, &col, &shift)) {
        if (shift)
            oric_key(&z->m, 4, 4, 1);
        oric_key(&z->m, row, col, 1);
    }
    if (ty->phase == 4)
        oric_key(&z->m, 0, 0, 0);       /* refresh the column sense */
    if (++ty->phase == 8) {
        ty->phase = 0;
        ty->pos++;
    }
}

static void run_frame(zxm *b)
{
    type_frame(OR(b));
    oric_run_frame(&OR(b)->m);
}

static const void *framebuffer(const zxm *b, int *pitch)
{
    if (pitch)
        *pitch = ORIC_W;
    return COR(b)->m.screen;
}

static const int16_t *audio(const zxm *b, int *samples)
{
    if (samples)
        *samples = COR(b)->m.audio_samples;
    return COR(b)->m.audio;
}

static void input(zxm *b, const zxm_input_event *ev)
{
    oric *m = &OR(b)->m;
    switch (ev->kind) {
    case ZXM_EV_KEY:
        oric_key(m, ev->a, ev->b, ev->down);
        break;
    case ZXM_EV_RELEASE_ALL:
        memset(m->keys, 0, sizeof m->keys);
        oric_key(m, 0, 0, 0);
        break;
    default:
        break;
    }
}

static void type(zxm *b, const char *text, int delay_frames)
{
    oric_typer *ty = &OR(b)->typer;
    size_t n = strlen(text);
    if (n > sizeof ty->buf)
        n = sizeof ty->buf;
    memcpy(ty->buf, text, n);
    ty->len = (uint32_t)n;
    ty->pos = 0;
    ty->phase = 0;
    ty->wait = delay_frames;
}

static int typing(const zxm *b)
{
    return COR(b)->typer.pos < COR(b)->typer.len;
}

static int media_insert(zxm *b, const zxm_slot *s, const zxm_media *media)
{
    if (s->kind != ZXM_SLOT_TAPE || media->kind != ZXM_MEDIA_BYTES)
        return -1;
    return oric_tape_insert(&OR(b)->m, media->data, media->len);
}

static int media_eject(zxm *b, const zxm_slot *s)
{
    if (s->kind != ZXM_SLOT_TAPE)
        return -1;
    oric_tape_eject(&OR(b)->m);
    return 0;
}

static size_t state_size(const zxm *b)
{
    return sizeof(oric_typer) + oric_state_size(&COR(b)->m);
}

static int state_save(const zxm *b, void *buf, size_t len)
{
    if (len < state_size(b))
        return -1;
    memcpy(buf, &COR(b)->typer, sizeof(oric_typer));
    return oric_state_save(&COR(b)->m, (uint8_t *)buf + sizeof(oric_typer), len - sizeof(oric_typer));
}

static int state_load(zxm *b, const void *buf, size_t len)
{
    if (len < sizeof(oric_typer)
        || oric_state_load(&OR(b)->m, (const uint8_t *)buf + sizeof(oric_typer), len - sizeof(oric_typer)))
        return -1;
    memcpy(&OR(b)->typer, buf, sizeof(oric_typer));
    return 0;
}

/* ---- debugger: the 6502 in zxm_regs (see zxm.h) ----------------------------- */

static void get_regs(const zxm *b, zxm_regs *r)
{
    const m6502 *c = &COR(b)->m.cpu;
    memset(r, 0, sizeof *r);
    r->af = (uint16_t)(c->a << 8 | c->p);
    r->bc = c->x;
    r->de = c->y;
    r->sp = (uint16_t)(0x100 | c->s);
    r->pc = c->pc;
}

static void set_regs(zxm *b, const zxm_regs *r)
{
    m6502 *c = &OR(b)->m.cpu;
    c->a = (uint8_t)(r->af >> 8);
    c->p = (uint8_t)(r->af | M6502_FU);
    c->x = (uint8_t)r->bc;
    c->y = (uint8_t)r->de;
    c->s = (uint8_t)r->sp;
    c->pc = r->pc;
}

static uint8_t peek(const zxm *b, uint16_t addr) { return oric_peek(&COR(b)->m, addr); }
static void poke(zxm *b, uint16_t addr, uint8_t v) { oric_poke(&OR(b)->m, addr, v); }
static uint64_t frames(const zxm *b) { return COR(b)->m.frames; }

static const zxm_debug debug = { get_regs, set_regs, peek, poke, frames };

static const zxm_debug *debugger(zxm *m)
{
    (void)m;
    return &debug;
}

const zxm_family zxm_family_oric = {
    machines, MACHINES, NULL, size, create, destroy, reset, run_frame,
    framebuffer, audio, input, type, typing, media_insert, media_eject,
    state_size, state_save, state_load, debugger, NULL, NULL, NULL, NULL, NULL
};
