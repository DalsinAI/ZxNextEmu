/*
 * zxm over the MSX machines (src/msx).
 *
 * ROMs, in order: the main BIOS (32K), the MSX2 sub ROM (16K), the logo
 * ROM (16K, C-BIOS only) and a disk ROM (a 16K floppy disk ROM, or a
 * Sunrise IDE flash ROM such as Nextor). The defaults are C-BIOS's file
 * names; a real BIOS goes in the same places and has no logo ROM.
 */
#include "zxm_impl.h"
#include "../msx/msx.h"
#include "../storage/blockdev.h"

#include <string.h>

typedef struct zxm_msx {
    struct zxm base;
    msx m;
} zxm_msx;

#define MSX(p) (&((zxm_msx *)(p))->m)
#define CMSX(p) (&((const zxm_msx *)(p))->m)

static const char *const roms_msx1[] = { "cbios_main_msx1.rom", "cbios_logo_msx1.rom", "disk.rom", NULL };
static const char *const roms_msx1eu[] = { "cbios_main_msx1_eu.rom", "cbios_logo_msx1.rom", "disk.rom", NULL };
static const char *const roms_msx2[] = { "cbios_main_msx2.rom", "cbios_sub.rom", "cbios_logo_msx2.rom", "disk.rom", NULL };
static const char *const roms_msx2eu[] = { "cbios_main_msx2_eu.rom", "cbios_sub.rom", "cbios_logo_msx2.rom", "disk.rom", NULL };

static const zxm_slot slots_msx[] = {
    { "cart1", "Cartridge slot 1", ZXM_SLOT_CARTRIDGE, ".rom .mx1 .mx2" },
    { "cart2", "Cartridge slot 2", ZXM_SLOT_CARTRIDGE, ".rom .mx1 .mx2" },
    { "disk0", "Floppy drive A", ZXM_SLOT_DISK, ".dsk" },
    { "disk1", "Floppy drive B", ZXM_SLOT_DISK, ".dsk" },
    { "ide0", "Hard disk (IDE master)", ZXM_SLOT_HARDDISK, ".img .hdd .dsk" },
    { "ide1", "Hard disk (IDE slave)", ZXM_SLOT_HARDDISK, ".img .hdd .dsk" },
    { NULL, NULL, 0, NULL },
};

static const zxm_input_desc inputs_msx[] = {
    { "keyboard", ZXM_IN_MSX_KEYBOARD },
    { "joystick", ZXM_IN_JOYSTICK },
    { "mouse", ZXM_IN_MOUSE },
    { NULL, 0 },
};

#define NTSC_T (262 * 228)
#define PAL_T  (313 * 228)

/* machines[i] is msx_models[i]. The logo ROM and the disk ROM are optional. */
static zxm_info machines[] = {
    { "msx1", "MSX1", MSX_CPU_HZ, NTSC_T, VDP_FB_W, VDP_FB_H, ZXM_RGB333, MSX_AUDIO_RATE,
      roms_msx1, slots_msx, inputs_msx, "z80", 0x6 },
    { "msx1eu", "MSX1 (50 Hz)", MSX_CPU_HZ, PAL_T, VDP_FB_W, VDP_FB_H, ZXM_RGB333, MSX_AUDIO_RATE,
      roms_msx1eu, slots_msx, inputs_msx, "z80", 0x6 },
    { "msx2", "MSX2", MSX_CPU_HZ, NTSC_T, VDP_FB_W, VDP_FB_H, ZXM_RGB333, MSX_AUDIO_RATE,
      roms_msx2, slots_msx, inputs_msx, "z80", 0xc },
    { "msx2eu", "MSX2 (50 Hz)", MSX_CPU_HZ, PAL_T, VDP_FB_W, VDP_FB_H, ZXM_RGB333, MSX_AUDIO_RATE,
      roms_msx2eu, slots_msx, inputs_msx, "z80", 0xc },
};

#define MACHINES ((int)(sizeof machines / sizeof machines[0]))

static size_t size(const zxm_info *info)
{
    (void)info;
    return sizeof(zxm_msx);
}

static int create(zxm *mm, const zxm_rom_set *roms)
{
    const msx_model *model = &msx_models[mm->info - machines];
    int sub = model->msx2 ? 1 : -1, logo = model->msx2 ? 2 : 1, ext = logo + 1;
    return msx_create(MSX(mm), model, roms->rom[0], roms->len[0],
                      sub >= 0 ? roms->rom[sub] : NULL, sub >= 0 ? roms->len[sub] : 0,
                      roms->rom[logo], roms->len[logo], roms->rom[ext], roms->len[ext]);
}

static void destroy(zxm *m) { msx_free(MSX(m)); }

static void reset(zxm *m, zxm_reset_kind kind)
{
    msx *x = MSX(m);
    switch (kind) {
    case ZXM_POWER_ON: msx_reset(x, 1); break;
    case ZXM_RESET: msx_reset(x, 0); break;
    case ZXM_NMI: z80_nmi(&x->cpu, &x->bus); break;
    }
}

static void run_frame(zxm *m) { msx_run_frame(MSX(m)); }

static const void *framebuffer(const zxm *m, int *pitch)
{
    if (pitch)
        *pitch = VDP_FB_W;
    return CMSX(m)->vdp.fb;
}

static const int16_t *audio(const zxm *m, int *samples)
{
    if (samples)
        *samples = CMSX(m)->audio_samples;
    return CMSX(m)->audio;
}

static void input(zxm *m, const zxm_input_event *ev)
{
    msx *x = MSX(m);
    switch (ev->kind) {
    case ZXM_EV_KEY:
        msx_key(x, ev->a, ev->b, ev->down);
        break;
    case ZXM_EV_KEMPSTON:
        msx_joystick(x, 0, ev->a);
        break;
    case ZXM_EV_JOYSTICK:
        msx_joystick(x, ev->b, ev->a);
        break;
    case ZXM_EV_MOUSE:
        msx_mouse_move(x, ev->dx, ev->dy, ev->a);
        break;
    case ZXM_EV_RELEASE_ALL:
        memset(x->keys, 0xff, sizeof x->keys);
        break;
    }
}

static void type(zxm *m, const char *text, int delay_frames) { msx_type(MSX(m), text, delay_frames); }
static int typing(const zxm *m) { return msx_typing(CMSX(m)); }

static int media_insert(zxm *m, const zxm_slot *s, const zxm_media *media)
{
    msx *x = MSX(m);
    int unit = s->id[strlen(s->id) - 1] - '0';
    switch (s->kind) {
    case ZXM_SLOT_CARTRIDGE:
        if (media->kind != ZXM_MEDIA_BYTES)
            return -1;
        return msx_cart_insert(x, unit - 1, media->data, media->len, media->name);
    case ZXM_SLOT_DISK:
        if (media->kind != ZXM_MEDIA_BLOCKS || !media->blocks)
            return -1;
        return msx_disk_insert(x, unit, media->blocks);
    case ZXM_SLOT_HARDDISK:
        if (media->kind != ZXM_MEDIA_BLOCKS || !media->blocks)
            return -1;
        return msx_ide_attach(x, unit, media->blocks);
    default:
        return -1;
    }
}

static int media_eject(zxm *m, const zxm_slot *s)
{
    msx *x = MSX(m);
    int unit = s->id[strlen(s->id) - 1] - '0';
    switch (s->kind) {
    case ZXM_SLOT_CARTRIDGE:
        msx_cart_eject(x, unit - 1);
        return 0;
    case ZXM_SLOT_DISK:
        return msx_disk_insert(x, unit, NULL);
    case ZXM_SLOT_HARDDISK:
        return msx_ide_attach(x, unit, NULL);
    default:
        return -1;
    }
}

static size_t state_size(const zxm *m) { return msx_state_size(CMSX(m)); }
static int state_save(const zxm *m, void *buf, size_t len) { return msx_state_save(CMSX(m), buf, len); }
static int state_load(zxm *m, const void *buf, size_t len) { return msx_state_load(MSX(m), buf, len); }

/* ---- debugger ------------------------------------------------------------- */

static void get_regs(const zxm *m, zxm_regs *r)
{
    const z80 *c = &CMSX(m)->cpu;
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
    z80 *c = &MSX(m)->cpu;
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

static uint8_t peek(const zxm *m, uint16_t addr) { return msx_peek(CMSX(m), addr); }
static void poke(zxm *m, uint16_t addr, uint8_t v) { msx_poke(MSX(m), addr, v); }
static uint64_t frames(const zxm *m) { return CMSX(m)->frames; }

static const zxm_debug debug = { get_regs, set_regs, peek, poke, frames };

static const zxm_debug *debugger(zxm *m)
{
    (void)m;
    return &debug;
}

const zxm_family zxm_family_msx = {
    machines, MACHINES, NULL, size, create, destroy, reset, run_frame,
    framebuffer, audio, input, type, typing, media_insert, media_eject,
    state_size, state_save, state_load, debugger, NULL, NULL, NULL, NULL, NULL, NULL
};
