/*
 * zxm over the SAM Coupé (src/sam).
 */
#include "zxm_impl.h"
#include "../sam/sam.h"
#include "../zx/keys.h"
#include "../zx/tape.h"
#include "../devices/floppy.h"

#include <string.h>

typedef struct zxm_sam {
    struct zxm base;
    sam s;
    zx_typer typer;
} zxm_sam;

#define SAM(p) ((zxm_sam *)(p))
#define CSAM(p) ((const zxm_sam *)(p))

static const char *const roms_sam[] = { "samcoupe.rom", NULL };

static const zxm_slot slots_sam[] = {
    { "disk.1", "Disk drive 1", ZXM_SLOT_DISK, ".mgt .sad .dsk" },
    { "disk.2", "Disk drive 2", ZXM_SLOT_DISK, ".mgt .sad .dsk" },
    { "tape", "Tape", ZXM_SLOT_TAPE, ".tap .tzx" },
    { NULL, NULL, 0, NULL },
};

static const zxm_input_desc inputs_sam[] = {
    { "keyboard", ZXM_IN_SAM_KEYBOARD },
    { "kempston", ZXM_IN_KEMPSTON },
    { NULL, 0 },
};

static zxm_info machines[] = {
    { "sam", "SAM Coup\xc3\xa9", SAM_CPU_HZ, SAM_FRAME_T, SAM_FB_W, SAM_FB_H, ZXM_RGB333,
      SAM_AUDIO_RATE, roms_sam, slots_sam, inputs_sam, "z80", 0 },
};

static size_t size(const zxm_info *info)
{
    (void)info;
    return sizeof(zxm_sam);
}

static int create(zxm *b, const zxm_rom_set *roms)
{
    zxm_sam *m = SAM(b);
    memset(&m->typer, 0, sizeof m->typer);
    return sam_create(&m->s, roms->rom[0], roms->len[0]);
}

static void destroy(zxm *b)
{
    sam_free(&SAM(b)->s);
}

static void reset(zxm *b, zxm_reset_kind kind)
{
    zxm_sam *m = SAM(b);
    switch (kind) {
    case ZXM_POWER_ON:
        sam_power_on(&m->s);
        memset(&m->typer, 0, sizeof m->typer);
        break;
    case ZXM_RESET:
        sam_reset(&m->s);
        break;
    case ZXM_NMI:
        z80_nmi(&m->s.cpu, &m->s.bus);
        break;
    }
}

/* The typing queue of keys.h, pressing SAM keys. */
static void type_frame(zx_typer *ty, sam *s)
{
    int row, bit, shift, sym, i;
    if (ty->pos >= ty->len)
        return;
    if (ty->wait > 0) {
        ty->wait--;
        return;
    }
    if (ty->phase == 0 || ty->phase == 3)
        for (i = 0; i < 9; i++)
            s->keys[i] = 0xff;
    if (ty->phase == 0 && sam_char_key((unsigned char)ty->buf[ty->pos], &row, &bit, &shift, &sym)) {
        sam_key(s, row, bit, 1);
        if (shift)
            sam_key(s, 0, 0, 1);
        if (sym)
            sam_key(s, 7, 1, 1);
    }
    if (++ty->phase == 10) {
        ty->phase = 0;
        ty->pos++;
    }
}

static void run_frame(zxm *b)
{
    zxm_sam *m = SAM(b);
    type_frame(&m->typer, &m->s);
    sam_run_frame(&m->s);
}

static const void *framebuffer(const zxm *b, int *pitch)
{
    if (pitch)
        *pitch = SAM_FB_W;
    return &CSAM(b)->s.screen[0][0];
}

static const int16_t *audio(const zxm *b, int *samples)
{
    if (samples)
        *samples = CSAM(b)->s.audio_samples;
    return CSAM(b)->s.audio;
}

static void input(zxm *b, const zxm_input_event *ev)
{
    zxm_sam *m = SAM(b);
    int i;
    switch (ev->kind) {
    case ZXM_EV_KEY:
        sam_key(&m->s, ev->a, ev->b, ev->down);
        break;
    case ZXM_EV_KEMPSTON:
        m->s.kempston = ev->a;
        break;
    case ZXM_EV_RELEASE_ALL:
        for (i = 0; i < 9; i++)
            m->s.keys[i] = 0xff;
        break;
    default:
        break;
    }
}

static void type(zxm *b, const char *text, int delay_frames)
{
    zxm_sam *m = SAM(b);
    zx_type(&m->typer, text);
    zx_type_delay(&m->typer, delay_frames);
}

static int typing(const zxm *b)
{
    return zx_typing(&CSAM(b)->typer);
}

static int media_insert(zxm *b, const zxm_slot *slot, const zxm_media *media)
{
    sam *s = &SAM(b)->s;
    switch (slot->kind) {
    case ZXM_SLOT_DISK: {
        fd_disk *d;
        if (media->kind == ZXM_MEDIA_BYTES)
            d = fd_open(media->data, media->len, media->name);
        else if (media->kind == ZXM_MEDIA_BLOCKS)
            d = fd_open_blocks(media->blocks);
        else
            return -1;
        if (!d)
            return -1;
        sam_insert_disk(s, slot->id[5] - '1', d);
        return 0;
    }
    case ZXM_SLOT_TAPE: {
        zx_tape *t;
        if (media->kind != ZXM_MEDIA_BYTES)
            return -1;
        t = zx_tape_open(media->data, media->len, zxm_has_ext(media->name, ".tzx"));
        if (!t)
            return -1;
        /* The SAM ROM loads in real time; there is no instant-load trap. */
        zx_tape_set_fast(t, 0);
        zx_tape_free(s->tape);
        s->tape = t;
        return 0;
    }
    default:
        return -1;
    }
}

static int media_eject(zxm *b, const zxm_slot *slot)
{
    sam *s = &SAM(b)->s;
    switch (slot->kind) {
    case ZXM_SLOT_DISK:
        sam_insert_disk(s, slot->id[5] - '1', NULL);
        return 0;
    case ZXM_SLOT_TAPE:
        zx_tape_free(s->tape);
        s->tape = NULL;
        return 0;
    default:
        return -1;
    }
}

static size_t state_size(const zxm *b)
{
    return sizeof CSAM(b)->typer + sam_state_size(&CSAM(b)->s);
}

static int state_save(const zxm *b, void *buf, size_t len)
{
    const zxm_sam *m = CSAM(b);
    if (len < state_size(b))
        return -1;
    memcpy(buf, &m->typer, sizeof m->typer);
    return sam_state_save(&m->s, (uint8_t *)buf + sizeof m->typer, len - sizeof m->typer);
}

static int state_load(zxm *b, const void *buf, size_t len)
{
    zxm_sam *m = SAM(b);
    if (len < sizeof m->typer
        || sam_state_load(&m->s, (const uint8_t *)buf + sizeof m->typer, len - sizeof m->typer))
        return -1;
    memcpy(&m->typer, buf, sizeof m->typer);
    return 0;
}

/* ---- debugger ------------------------------------------------------------- */

static void get_regs(const zxm *b, zxm_regs *r)
{
    const z80 *c = &CSAM(b)->s.cpu;
    r->af = (uint16_t)(c->a << 8 | c->f);
    r->bc = (uint16_t)(c->b << 8 | c->c);
    r->de = (uint16_t)(c->d << 8 | c->e);
    r->hl = (uint16_t)(c->h << 8 | c->l);
    r->af_ = (uint16_t)(c->a_ << 8 | c->f_);
    r->bc_ = (uint16_t)(c->b_ << 8 | c->c_);
    r->de_ = (uint16_t)(c->d_ << 8 | c->e_);
    r->hl_ = (uint16_t)(c->h_ << 8 | c->l_);
    r->ix = c->ix;
    r->iy = c->iy;
    r->sp = c->sp;
    r->pc = c->pc;
    r->memptr = c->memptr;
    r->i = c->i;
    r->r = c->r;
    r->iff1 = c->iff1;
    r->iff2 = c->iff2;
    r->im = c->im;
    r->halted = c->halted;
}

static void set_regs(zxm *b, const zxm_regs *r)
{
    z80 *c = &SAM(b)->s.cpu;
    c->a = (uint8_t)(r->af >> 8); c->f = (uint8_t)r->af;
    c->b = (uint8_t)(r->bc >> 8); c->c = (uint8_t)r->bc;
    c->d = (uint8_t)(r->de >> 8); c->e = (uint8_t)r->de;
    c->h = (uint8_t)(r->hl >> 8); c->l = (uint8_t)r->hl;
    c->a_ = (uint8_t)(r->af_ >> 8); c->f_ = (uint8_t)r->af_;
    c->b_ = (uint8_t)(r->bc_ >> 8); c->c_ = (uint8_t)r->bc_;
    c->d_ = (uint8_t)(r->de_ >> 8); c->e_ = (uint8_t)r->de_;
    c->h_ = (uint8_t)(r->hl_ >> 8); c->l_ = (uint8_t)r->hl_;
    c->ix = r->ix;
    c->iy = r->iy;
    c->sp = r->sp;
    c->pc = r->pc;
    c->memptr = r->memptr;
    c->i = r->i;
    c->r = r->r;
    c->iff1 = r->iff1;
    c->iff2 = r->iff2;
    c->im = r->im;
    c->halted = r->halted;
}

static uint8_t peek(const zxm *b, uint16_t addr) { return sam_peek(&CSAM(b)->s, addr); }
static void poke(zxm *b, uint16_t addr, uint8_t v) { sam_poke(&SAM(b)->s, addr, v); }
static uint64_t frames(const zxm *b) { return CSAM(b)->s.frames; }

static const zxm_debug debug = { get_regs, set_regs, peek, poke, frames };

static const zxm_debug *debugger(zxm *m)
{
    (void)m;
    return &debug;
}

const zxm_family zxm_family_sam = {
    machines, (int)(sizeof machines / sizeof machines[0]), NULL, size, create, destroy,
    reset, run_frame, framebuffer, audio, input, type, typing, media_insert, media_eject,
    state_size, state_save, state_load, debugger, NULL, NULL, NULL, NULL
};
