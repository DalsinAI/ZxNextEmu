/*
 * zxm over the Spectrum family (src/zx) and the Next (src/next).
 */
#include "zxm_impl.h"
#include "../zx/zx.h"
#include "../zx/keys.h"
#include "../zx/tape.h"
#include "../zx/expansion.h"
#include "../next/next.h"
#include "../storage/blockdev.h"
#include "../devices/serial.h"
#include "../spec256/spec256.h"

#include <string.h>

#define MAX_SLOTS 12
#define MAX_INPUTS 8

typedef struct zxm_zx {
    struct zxm base;
    const zxm_interface *iface;
    zxm_slot slots[MAX_SLOTS];        /* the machine's and the interface's */
    zxm_input_desc inputs[MAX_INPUTS];
    zx m;
    zx_typer typer;
    s256 *s256;                  /* the Spec256 machine's shadow CPUs */
} zxm_zx;

#define ZX(p) ((zxm_zx *)(p))
#define CZX(p) ((const zxm_zx *)(p))

static const char *const roms_48[] = { "48.rom", NULL };
static const char *const roms_128[] = { "128-0.rom", "128-1.rom", NULL };
static const char *const roms_plus2[] = { "plus2-0.rom", "plus2-1.rom", NULL };
static const char *const roms_plus3[] = { "plus3-0.rom", "plus3-1.rom", "plus3-2.rom", "plus3-3.rom", NULL };
static const char *const roms_none[] = { NULL };
static const char *const roms_pentagon[] = { "128-0.rom", "128-1.rom", "trdos.rom", NULL };
static const char *const roms_scorpion[] = { "256s-0.rom", "256s-1.rom", "256s-2.rom", "256s-3.rom", NULL };
static const char *const roms_atm2[] = { "atm2-0.rom", "atm2-1.rom", "atm2-2.rom", "atm2-3.rom", NULL };
static const char *const roms_spec256[] = { "48.rom", "128-0.rom", "128-1.rom", NULL };

static const zxm_slot slots_classic[] = {
    { "tape", "Tape", ZXM_SLOT_TAPE, ".tap .tzx" },
    { "snapshot", "Snapshot", ZXM_SLOT_SNAPSHOT, ".sna .z80" },
    { NULL, NULL, 0, NULL },
};

/* The Russian clones: a Beta 128 with four drives. A blocks disk is .trd
 * layout; bytes may be .trd or .scl (an empty .trd is a blank disk). */
static const zxm_slot slots_beta[] = {
    { "tape", "Tape", ZXM_SLOT_TAPE, ".tap .tzx" },
    { "snapshot", "Snapshot", ZXM_SLOT_SNAPSHOT, ".sna .z80" },
    { "beta.a", "Drive A", ZXM_SLOT_DISK, ".trd .scl" },
    { "beta.b", "Drive B", ZXM_SLOT_DISK, ".trd .scl" },
    { "beta.c", "Drive C", ZXM_SLOT_DISK, ".trd .scl" },
    { "beta.d", "Drive D", ZXM_SLOT_DISK, ".trd .scl" },
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

/* Spec256: the companion files first (every .gfx .gf? .b?? .cfg .pal of
 * the game), then the .sna, which picks 48K or 128K. */
static const zxm_slot slots_spec256[] = {
    { "gfx", "Spec256 graphics", ZXM_SLOT_COMPANION, ".gfx .gf0 .gf1 .gf2 .gf3 .gf4 .gf5 .gf6 .gf7 .gfa .gfb .b00 .cfg .pal" },
    { "snapshot", "Snapshot", ZXM_SLOT_SNAPSHOT, ".sna .z80" },
    { "tape", "Tape", ZXM_SLOT_TAPE, ".tap .tzx" },
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

static const char *const roms_esxdos[] = { "esxmmc.bin", NULL };

static const zxm_slot slots_divmmc[] = {
    { "sd0", "SD card", ZXM_SLOT_SD, ".img .mmc" },
    { NULL, NULL, 0, NULL },
};

static const zxm_slot slots_mb03[] = {
    { "sd0", "SD card", ZXM_SLOT_SD, ".img .mmc" },
    { "sd1", "Second SD card", ZXM_SLOT_SD, ".img .mmc" },
    { NULL, NULL, 0, NULL },
};

static const zxm_input_desc inputs_none[] = {
    { NULL, 0 },
};

static const zxm_input_desc inputs_mb03[] = {
    { "mouse", ZXM_IN_MOUSE },
    { NULL, 0 },
};

static const zxm_interface interfaces[] = {
    { "divmmc", "divMMC", roms_esxdos, slots_divmmc, inputs_none },
    { "mb03", "MB03+ Ultimate", roms_esxdos, slots_mb03, inputs_mb03 },
};

static const uint32_t interface_features[] = { ZX_EXP_DIVMMC, ZX_EXP_MB03 };

#define INTERFACES ((int)(sizeof interfaces / sizeof interfaces[0]))

#define CLASSIC(id, name, model, roms) \
    { id, name, 0, 0, ZX_SCREEN_W, ZX_SCREEN_H, ZXM_RGB333, ZX_AUDIO_RATE, roms, slots_classic, inputs_classic, "z80", 0 }
#define BETA(id, name, model, roms, w, optional) \
    { id, name, 0, 0, w, ZX_SCREEN_H, ZXM_RGB333, ZX_AUDIO_RATE, roms, slots_beta, inputs_classic, "z80", optional }

/* cpu_hz and frame_t are filled from zx_timings on first lookup. */
static zxm_info machines[] = {
    CLASSIC("zx48", "ZX Spectrum 48K", ZX_48K, roms_48),
    CLASSIC("zx128", "ZX Spectrum 128K", ZX_128K, roms_128),
    CLASSIC("plus2", "ZX Spectrum +2", ZX_PLUS2, roms_plus2),
    CLASSIC("plus2a", "ZX Spectrum +2A", ZX_PLUS2A, roms_plus3),
    CLASSIC("plus3", "ZX Spectrum +3", ZX_PLUS3, roms_plus3),
    BETA("pentagon", "Pentagon 128", ZX_PENTAGON, roms_pentagon, ZX_SCREEN_W, 1u << 2),
    { "next", "ZX Spectrum Next", 0, 0, NX_W, NX_H, ZXM_RGB333, ZX_AUDIO_RATE, roms_none, slots_next, inputs_next, "z80", 0 },
    BETA("scorpion", "Scorpion ZS-256", ZX_SCORPION, roms_scorpion, ZX_SCREEN_W, 0),
    BETA("atm2", "ATM Turbo 2+", ZX_ATM2, roms_atm2, ZX_SCREEN_W * 2, 0),
    { "spec256", "Spec256 (256-colour Spectrum)", 0, 0, ZX_SCREEN_W, ZX_SCREEN_H, ZXM_RGB333, ZX_AUDIO_RATE, roms_spec256, slots_spec256, inputs_classic, "z80", 0 },
};

/* machines[i] is model i, then Spec256 (a 48K or 128K underneath). */
#define MACHINES ((int)(sizeof machines / sizeof machines[0]))
#define SPEC256 ZX_MODEL_COUNT

static zx_model model_of(const zxm_info *info)
{
    int i = (int)(info - machines);
    return i == SPEC256 ? ZX_48K : (zx_model)i;
}

static void init_info(zxm_info *info, int i)
{
    info->cpu_hz = zx_timings[model_of(&machines[i])].cpu_hz;
    info->frame_t = zx_timings[model_of(&machines[i])].frame_t;
}

const zxm_interface *zxm_interface_at(int i)
{
    return i >= 0 && i < INTERFACES ? &interfaces[i] : NULL;
}

const zxm_interface *zxm_interface_lookup(const char *id)
{
    int i;
    for (i = 0; i < INTERFACES; i++)
        if (!strcmp(id, interfaces[i].id))
            return &interfaces[i];
    return NULL;
}

/* The instance's lists: the machine's entries, then the interface's. */
static void build_lists(zxm_zx *m)
{
    const zxm_slot *s;
    const zxm_input_desc *in;
    int n = 0;
    for (s = m->base.info->slots; s->id && n < MAX_SLOTS - 1; s++)
        m->slots[n++] = *s;
    for (s = m->iface ? m->iface->slots : NULL; s && s->id && n < MAX_SLOTS - 1; s++)
        m->slots[n++] = *s;
    memset(&m->slots[n], 0, sizeof m->slots[n]);
    n = 0;
    for (in = m->base.info->inputs; in->id && n < MAX_INPUTS - 1; in++)
        m->inputs[n++] = *in;
    for (in = m->iface ? m->iface->inputs : NULL; in && in->id && n < MAX_INPUTS - 1; in++)
        m->inputs[n++] = *in;
    memset(&m->inputs[n], 0, sizeof m->inputs[n]);
}

static int attach(zxm *b, const char *id, const zxm_rom_set *roms)
{
    zxm_zx *m = ZX(b);
    const zxm_interface *f = zxm_interface_lookup(id);
    if (!f || m->iface || !roms || !roms->rom[0])
        return -1;
    if (zx_exp_attach(&m->m, interface_features[f - interfaces], roms->rom[0], roms->len[0]))
        return -1;
    m->iface = f;
    build_lists(m);
    zx_reset(&m->m);
    return 0;
}

static const zxm_slot *slots(const zxm *b)
{
    return CZX(b)->slots;
}

static const zxm_input_desc *inputs(const zxm *b)
{
    return CZX(b)->inputs;
}

static size_t size(const zxm_info *info)
{
    (void)info;
    return sizeof(zxm_zx);
}

static int create(zxm *b, const zxm_rom_set *roms)
{
    zxm_zx *m = ZX(b);
    const zxm_info *info = b->info;
    const uint8_t *r[4] = { NULL, NULL, NULL, NULL };
    int i;
    for (i = 0; i < 4 && info->roms[i]; i++) {
        if (!roms->rom[i] && (info->rom_optional >> i & 1))
            continue;
        if (roms->len[i] != 0x4000)
            return -1;
        r[i] = roms->rom[i];
    }
    memset(&m->typer, 0, sizeof m->typer);
    m->iface = NULL;
    build_lists(m);
    m->s256 = NULL;
    if (info - machines == SPEC256) {
        m->s256 = s256_new(r[0], r[1], r[2]);
        if (!m->s256 || zx_create(&m->m, ZX_48K, r))
            return -1;
        s256_attach(m->s256, &m->m);
        s256_start(m->s256);
        return 0;
    }
    return zx_create(&m->m, model_of(info), r);
}

/* A Spec256 snapshot: become the 48K or 128K it needs, load it, start the
 * shadow CPUs on the graphics given so far. */
static int spec256_load(zxm_zx *m, const zxm_media *media)
{
    zx *z = &m->m;
    const uint8_t *roms[4];
    int is128 = zxm_has_ext(media->name, ".sna") && s256_snapshot_is_128(media->len), r;
    zx_model want = is128 ? ZX_128K : ZX_48K;
    if (z->model != want) {
        zx_tape *tape = z->tape;
        s256_roms(m->s256, is128, roms);
        if (zx_create(z, want, roms))
            return -1;
        z->tape = tape;
        s256_attach(m->s256, z);
    }
    r = zxm_has_ext(media->name, ".sna") ? zx_load_sna(z, media->data, media->len)
                                         : zx_load_z80(z, media->data, media->len);
    if (r)
        return r;
    s256_set_name(m->s256, media->name);
    s256_start(m->s256);
    return 0;
}

static void destroy(zxm *b)
{
    zxm_zx *m = ZX(b);
    zx_tape_free(m->m.tape);
    m->m.tape = NULL;
    zx_free(&m->m);
    s256_free(m->s256);
    m->s256 = NULL;
}

static void reset(zxm *b, zxm_reset_kind kind)
{
    zxm_zx *m = ZX(b);
    zx *z = &m->m;
    switch (kind) {
    case ZXM_POWER_ON:
        if (z->model == ZX_NEXT) {
            nx_power_on(z);
        } else {
            memset(z->ram, 0, (size_t)z->ram_pages * 0x4000);
            zx_exp_power_on(z);
            zx_reset(z);
            if (m->s256)
                s256_start(m->s256);
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

static void run_frame(zxm *b)
{
    zxm_zx *m = ZX(b);
    zx_type_frame(&m->typer, &m->m);
    zx_run_frame(&m->m);
}

static const void *framebuffer(const zxm *b, int *pitch)
{
    const zxm_zx *m = CZX(b);
    if (pitch)
        *pitch = m->m.pitch;
    return m->m.pixels;
}

static int fb_width(const zxm *b)
{
    return CZX(b)->m.screen_w;
}

static const int16_t *audio(const zxm *b, int *samples)
{
    const zxm_zx *m = CZX(b);
    if (samples)
        *samples = m->m.audio_samples;
    return m->m.audio;
}

static void input(zxm *b, const zxm_input_event *ev)
{
    zxm_zx *m = ZX(b);
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

static void type(zxm *b, const char *text, int delay_frames)
{
    zxm_zx *m = ZX(b);
    zx_type(&m->typer, text);
    zx_type_delay(&m->typer, delay_frames);
}

static int typing(const zxm *b)
{
    const zxm_zx *m = CZX(b);
    return zx_typing(&m->typer);
}

/* ---- media ---------------------------------------------------------------- */

static int media_insert(zxm *b, const zxm_slot *s, const zxm_media *media)
{
    zxm_zx *m = ZX(b);
    const char *slot = s->id;
    zx *z = &m->m;
    switch (s->kind) {
    case ZXM_SLOT_TAPE: {
        zx_tape *t;
        if (media->kind != ZXM_MEDIA_BYTES)
            return -1;
        t = zx_tape_open(media->data, media->len, zxm_has_ext(media->name, ".tzx"));
        if (!t)
            return -1;
        zx_tape_free(z->tape);
        z->tape = t;
        return 0;
    }
    case ZXM_SLOT_SNAPSHOT:
        if (media->kind != ZXM_MEDIA_BYTES)
            return -1;
        if (m->s256 && (zxm_has_ext(media->name, ".sna") || zxm_has_ext(media->name, ".z80")))
            return spec256_load(m, media);
        if (zxm_has_ext(media->name, ".sna"))
            return zx_load_sna(z, media->data, media->len);
        if (zxm_has_ext(media->name, ".z80"))
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
    case ZXM_SLOT_DISK:
        if (media->kind == ZXM_MEDIA_BLOCKS)
            return zx_attach_disk_dev(z, slot[5] - 'a', media->blocks);
        if (media->kind == ZXM_MEDIA_BYTES)
            return zx_attach_disk_image(z, slot[5] - 'a', media->data, media->len, zxm_has_ext(media->name, ".scl"));
        return -1;
    case ZXM_SLOT_COMPANION:
        if (media->kind != ZXM_MEDIA_BYTES || !m->s256)
            return -1;
        return s256_add_file(m->s256, media->name, media->data, media->len);
    default:
        return -1;
    }
}

static int media_eject(zxm *b, const zxm_slot *s)
{
    zxm_zx *m = ZX(b);
    const char *slot = s->id;
    zx *z = &m->m;
    switch (s->kind) {
    case ZXM_SLOT_TAPE:
        zx_tape_free(z->tape);
        z->tape = NULL;
        return 0;
    case ZXM_SLOT_SD:
        if (!z->next)
            return zx_exp_attach_sd(z, slot[2] - '0', NULL);
        nx_sd_close(z->next->sd[slot[2] - '0']);
        z->next->sd[slot[2] - '0'] = NULL;
        return 0;
    case ZXM_SLOT_SERIAL:
        nx_attach_serial(z, slot[4] - '0', NULL);
        return 0;
    case ZXM_SLOT_DISK:
        zx_eject_disk(z, slot[5] - 'a');
        return 0;
    case ZXM_SLOT_COMPANION:
        if (m->s256)
            s256_clear_files(m->s256);
        return 0;
    default:
        return -1;
    }
}

static int media_active(const zxm *b, const zxm_slot *s)
{
    const zxm_zx *m = CZX(b);
    if (s->kind == ZXM_SLOT_TAPE)
        return m->m.tape && zx_tape_playing(m->m.tape);
    return 0;
}

static int media_progress(const zxm *b, const zxm_slot *s)
{
    const zxm_zx *m = CZX(b);
    if (s->kind == ZXM_SLOT_TAPE)
        return m->m.tape ? zx_tape_progress(m->m.tape) : -1;
    return -1;
}

/* ---- state ---------------------------------------------------------------- */

static size_t state_size(const zxm *b)
{
    const zxm_zx *m = CZX(b);
    return sizeof m->typer + zx_state_size(&m->m);
}

static int state_save(const zxm *b, void *buf, size_t len)
{
    const zxm_zx *m = CZX(b);
    if (len < state_size(b))
        return -1;
    memcpy(buf, &m->typer, sizeof m->typer);
    return zx_state_save(&m->m, (uint8_t *)buf + sizeof m->typer, len - sizeof m->typer);
}

static int state_load(zxm *b, const void *buf, size_t len)
{
    zxm_zx *m = ZX(b);
    if (len < sizeof m->typer
        || zx_state_load(&m->m, (const uint8_t *)buf + sizeof m->typer, len - sizeof m->typer))
        return -1;
    memcpy(&m->typer, buf, sizeof m->typer);
    return 0;
}

/* ---- debugger ------------------------------------------------------------- */

static void get_regs(const zxm *b, zxm_regs *r)
{
    const z80 *c = &CZX(b)->m.cpu;
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
    z80 *c = &ZX(b)->m.cpu;
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

static uint8_t peek(const zxm *b, uint16_t addr) { return zx_peek(&CZX(b)->m, addr); }
static void poke(zxm *b, uint16_t addr, uint8_t v) { zx_poke(&ZX(b)->m, addr, v); }
static uint64_t frames(const zxm *b) { return CZX(b)->m.frames; }

static const zxm_debug debug = { get_regs, set_regs, peek, poke, frames };

static const zxm_debug *debugger(zxm *m)
{
    (void)m;
    return &debug;
}

const zxm_family zxm_family_zx = {
    machines, MACHINES, init_info, size, create, destroy, reset, run_frame,
    framebuffer, audio, input, type, typing, media_insert, media_eject,
    state_size, state_save, state_load, debugger, attach, slots, inputs, media_active,
    media_progress, fb_width
};
