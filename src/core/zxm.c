/*
 * zxm over the Spectrum family (src/zx) and the Next (src/next).
 */
#include "zxm.h"
#include "../zx/zx.h"
#include "../zx/keys.h"
#include "../zx/tape.h"
#include "../next/next.h"
#include "../storage/blockdev.h"
#include "../devices/serial.h"

#include <string.h>

struct zxm {
    const zxm_info *info;
    zx m;
    zx_typer typer;
};

static const char *const roms_48[] = { "48.rom", NULL };
static const char *const roms_128[] = { "128-0.rom", "128-1.rom", NULL };
static const char *const roms_plus2[] = { "plus2-0.rom", "plus2-1.rom", NULL };
static const char *const roms_plus3[] = { "plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom", NULL };
static const char *const roms_none[] = { NULL };

static const zxm_slot slots_classic[] = {
    { "tape", "Tape", ZXM_SLOT_TAPE, ".tap .tzx" },
    { "snapshot", "Snapshot", ZXM_SLOT_SNAPSHOT, ".sna .z80" },
    { NULL, NULL, 0, NULL },
};

static const zxm_slot slots_next[] = {
    { "sd0", "SD card", ZXM_SLOT_SD, ".img" },
    { "sd1", "Second SD card", ZXM_SLOT_SD, ".img" },
    { "tape", "Tape", ZXM_SLOT_TAPE, ".tap .tzx" },
    { "uart0", "Wifi (ESP8266)", ZXM_SLOT_SERIAL, "" },
    { "uart1", "Raspberry Pi", ZXM_SLOT_SERIAL, "" },
    { NULL, NULL, 0, NULL },
};

static const zxm_input_desc inputs_classic[] = {
    { "keyboard", ZXM_IN_KEYBOARD },
    { "kempston", ZXM_IN_KEMPSTON },
    { NULL, 0 },
};

static const zxm_input_desc inputs_next[] = {
    { "keyboard", ZXM_IN_KEYBOARD },
    { "kempston", ZXM_IN_KEMPSTON },
    { "mouse", ZXM_IN_MOUSE },
    { NULL, 0 },
};

#define CLASSIC(id, name, model, roms) \
    { id, name, 0, 0, ZX_SCREEN_W, ZX_SCREEN_H, ZXM_RGB333, ZX_AUDIO_RATE, roms, slots_classic, inputs_classic }

/* cpu_hz and frame_t are filled from zx_timings on first lookup. */
static zxm_info machines[] = {
    CLASSIC("zx48", "ZX Spectrum 48K", ZX_48K, roms_48),
    CLASSIC("zx128", "ZX Spectrum 128K", ZX_128K, roms_128),
    CLASSIC("plus2", "ZX Spectrum +2", ZX_PLUS2, roms_plus2),
    CLASSIC("plus2a", "ZX Spectrum +2A", ZX_PLUS2A, roms_plus3),
    CLASSIC("plus3", "ZX Spectrum +3", ZX_PLUS3, roms_plus3),
    CLASSIC("pentagon", "Pentagon 128", ZX_PENTAGON, roms_128),
    { "next", "ZX Spectrum Next", 0, 0, NX_W, NX_H, ZXM_RGB333, ZX_AUDIO_RATE, roms_none, slots_next, inputs_next },
};

/* machines[i] is model i. */
#define MACHINES ((int)(sizeof machines / sizeof machines[0]))

static zx_model model_of(const zxm_info *info)
{
    return (zx_model)(info - machines);
}

const zxm_info *zxm_machine(int i)
{
    if (i < 0 || i >= MACHINES)
        return NULL;
    if (!machines[i].cpu_hz) {
        machines[i].cpu_hz = zx_timings[i].cpu_hz;
        machines[i].frame_t = zx_timings[i].frame_t;
    }
    return &machines[i];
}

const zxm_info *zxm_lookup(const char *id)
{
    int i;
    for (i = 0; i < MACHINES; i++)
        if (!strcmp(id, machines[i].id))
            return zxm_machine(i);
    return NULL;
}

size_t zxm_size(const zxm_info *info)
{
    (void)info;
    return sizeof(zxm);
}

int zxm_create(zxm *m, const zxm_info *info, const zxm_rom_set *roms)
{
    const uint8_t *r[4] = { NULL, NULL, NULL, NULL };
    int i;
    if (!info || info < machines || info >= machines + MACHINES)
        return -1;
    for (i = 0; i < 4 && info->roms[i]; i++) {
        if (!roms || !roms->rom[i] || roms->len[i] != 0x4000)
            return -1;
        r[i] = roms->rom[i];
    }
    memset(&m->typer, 0, sizeof m->typer);
    m->info = info;
    return zx_create(&m->m, model_of(info), r);
}

void zxm_destroy(zxm *m)
{
    zx_tape_free(m->m.tape);
    m->m.tape = NULL;
    zx_free(&m->m);
}

const zxm_info *zxm_info_of(const zxm *m)
{
    return m->info;
}

void zxm_reset(zxm *m, zxm_reset_kind kind)
{
    zx *z = &m->m;
    switch (kind) {
    case ZXM_POWER_ON:
        if (z->model == ZX_NEXT) {
            nx_power_on(z);
        } else {
            memset(z->ram, 0, sizeof z->ram);
            zx_reset(z);
        }
        memset(&m->typer, 0, sizeof m->typer);
        break;
    case ZXM_RESET:
        zx_reset(z);
        break;
    case ZXM_NMI:
        z80_nmi(&z->cpu, &z->bus);
        break;
    }
}

void zxm_run_frame(zxm *m)
{
    zx_type_frame(&m->typer, &m->m);
    zx_run_frame(&m->m);
}

const void *zxm_framebuffer(const zxm *m, int *pitch)
{
    if (pitch)
        *pitch = m->m.pitch;
    return m->m.pixels;
}

const int16_t *zxm_audio(const zxm *m, int *samples)
{
    if (samples)
        *samples = m->m.audio_samples;
    return m->m.audio;
}

void zxm_input(zxm *m, const zxm_input_event *ev)
{
    int i;
    switch (ev->kind) {
    case ZXM_EV_KEY:
        zx_key(&m->m, ev->a, ev->b, ev->down);
        break;
    case ZXM_EV_KEMPSTON:
        m->m.kempston = ev->a;
        break;
    case ZXM_EV_MOUSE:
        zx_mouse(&m->m, ev->dx, ev->dy, ev->dz, ev->a);
        break;
    case ZXM_EV_RELEASE_ALL:
        for (i = 0; i < 8; i++)
            m->m.keys[i] = 0x1f;
        break;
    }
}

void zxm_type(zxm *m, const char *text, int delay_frames)
{
    zx_type(&m->typer, text);
    zx_type_delay(&m->typer, delay_frames);
}

int zxm_typing(const zxm *m)
{
    return zx_typing(&m->typer);
}

/* ---- media ---------------------------------------------------------------- */

static int has_ext(const char *name, const char *ext)
{
    size_t ln, le = strlen(ext), i;
    if (!name || (ln = strlen(name)) < le)
        return 0;
    for (i = 0; i < le; i++) {
        char c = name[ln - le + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != ext[i])
            return 0;
    }
    return 1;
}

static const zxm_slot *find_slot(const zxm *m, const char *id)
{
    const zxm_slot *s;
    for (s = m->info->slots; s->id; s++)
        if (!strcmp(s->id, id))
            return s;
    return NULL;
}

int zxm_media_insert(zxm *m, const char *slot, const zxm_media *media)
{
    const zxm_slot *s = find_slot(m, slot);
    zx *z = &m->m;
    if (!s || !media)
        return -1;
    switch (s->kind) {
    case ZXM_SLOT_TAPE: {
        zx_tape *t;
        if (media->kind != ZXM_MEDIA_BYTES)
            return -1;
        t = zx_tape_open(media->data, media->len, has_ext(media->name, ".tzx"));
        if (!t)
            return -1;
        zx_tape_free(z->tape);
        z->tape = t;
        return 0;
    }
    case ZXM_SLOT_SNAPSHOT:
        if (media->kind != ZXM_MEDIA_BYTES)
            return -1;
        if (has_ext(media->name, ".sna"))
            return zx_load_sna(z, media->data, media->len);
        if (has_ext(media->name, ".z80"))
            return zx_load_z80(z, media->data, media->len);
        return -1;
    case ZXM_SLOT_SD:
        if (media->kind != ZXM_MEDIA_BLOCKS)
            return -1;
        return zx_attach_sd_dev(z, slot[2] - '0', media->blocks);
    case ZXM_SLOT_SERIAL:
        if (media->kind != ZXM_MEDIA_SERIAL || !z->next)
            return -1;
        nx_attach_serial(z, slot[4] - '0', media->serial);
        return 0;
    }
    return -1;
}

int zxm_media_eject(zxm *m, const char *slot)
{
    const zxm_slot *s = find_slot(m, slot);
    zx *z = &m->m;
    if (!s)
        return -1;
    switch (s->kind) {
    case ZXM_SLOT_TAPE:
        zx_tape_free(z->tape);
        z->tape = NULL;
        return 0;
    case ZXM_SLOT_SD:
        nx_sd_close(z->next->sd[slot[2] - '0']);
        z->next->sd[slot[2] - '0'] = NULL;
        return 0;
    case ZXM_SLOT_SERIAL:
        nx_attach_serial(z, slot[4] - '0', NULL);
        return 0;
    default:
        return -1;
    }
}

/* ---- state ---------------------------------------------------------------- */

size_t zxm_state_size(const zxm *m)
{
    return sizeof m->typer + zx_state_size(&m->m);
}

int zxm_state_save(const zxm *m, void *buf, size_t len)
{
    if (len < zxm_state_size(m))
        return -1;
    memcpy(buf, &m->typer, sizeof m->typer);
    return zx_state_save(&m->m, (uint8_t *)buf + sizeof m->typer, len - sizeof m->typer);
}

int zxm_state_load(zxm *m, const void *buf, size_t len)
{
    if (len < sizeof m->typer
        || zx_state_load(&m->m, (const uint8_t *)buf + sizeof m->typer, len - sizeof m->typer))
        return -1;
    memcpy(&m->typer, buf, sizeof m->typer);
    return 0;
}

/* ---- debugger ------------------------------------------------------------- */

static void get_regs(const zxm *m, zxm_regs *r)
{
    const z80 *c = &m->m.cpu;
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

static void set_regs(zxm *m, const zxm_regs *r)
{
    z80 *c = &m->m.cpu;
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

static uint8_t peek(const zxm *m, uint16_t addr) { return zx_peek(&m->m, addr); }
static void poke(zxm *m, uint16_t addr, uint8_t v) { zx_poke(&m->m, addr, v); }
static uint64_t frames(const zxm *m) { return m->m.frames; }

static const zxm_debug debug = { get_regs, set_regs, peek, poke, frames };

const zxm_debug *zxm_debugger(zxm *m)
{
    (void)m;
    return &debug;
}
