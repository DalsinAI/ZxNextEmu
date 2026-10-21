/*
 * Inside zxm: how a machine family plugs into the dispatcher in zxm.c.
 *
 * Every instance starts with struct zxm; a family's own instance struct
 * puts it first and casts. zxm.c checks the ROM set against the machine's
 * ROM list (present unless rom_optional says otherwise) and leaves the
 * lengths to the family.
 */
#ifndef ZXM_IMPL_H
#define ZXM_IMPL_H

#include "zxm.h"

typedef struct zxm_family zxm_family;

struct zxm {
    const zxm_info *info;
    const zxm_family *fam;
};

struct zxm_family {
    zxm_info *machines;          /* this family's machines */
    int count;
    void (*init_info)(zxm_info *info, int i);   /* fill computed fields; may be NULL */
    size_t (*size)(const zxm_info *info);
    int (*create)(zxm *m, const zxm_rom_set *roms);  /* m->info, m->fam are set */
    void (*destroy)(zxm *m);
    void (*reset)(zxm *m, zxm_reset_kind kind);
    void (*run_frame)(zxm *m);
    const void *(*framebuffer)(const zxm *m, int *pitch);
    const int16_t *(*audio)(const zxm *m, int *samples);
    void (*input)(zxm *m, const zxm_input_event *ev);
    void (*type)(zxm *m, const char *text, int delay_frames);
    int (*typing)(const zxm *m);
    int (*media_insert)(zxm *m, const zxm_slot *slot, const zxm_media *media);
    int (*media_eject)(zxm *m, const zxm_slot *slot);
    size_t (*state_size)(const zxm *m);
    int (*state_save)(const zxm *m, void *buf, size_t len);
    int (*state_load)(zxm *m, const void *buf, size_t len);
    const zxm_debug *(*debugger)(zxm *m);
    /* Optional (NULL): an expansion-port interface (zxm_attach), the
     * instance's own slot and input lists (else zxm_info's), a busy slot,
     * and how far a slot's medium has got. */
    int (*attach)(zxm *m, const char *id, const zxm_rom_set *roms);
    const zxm_slot *(*slots)(const zxm *m);
    const zxm_input_desc *(*inputs)(const zxm *m);
    int (*media_active)(const zxm *m, const zxm_slot *slot);
    /* Optional: how far through its medium a slot is, 0 to 100, or -1. */
    int (*media_progress)(const zxm *m, const zxm_slot *slot);
};

extern const zxm_family zxm_family_zx;     /* zxm_zx.c: Spectrums and the Next */
extern const zxm_family zxm_family_sam;    /* zxm_sam.c: SAM Coupé */
extern const zxm_family zxm_family_msx;    /* zxm_msx.c: MSX1 and MSX2 */

/* zxm.c: case-insensitive file name extension test (".tap"). */
int zxm_has_ext(const char *name, const char *ext);

#endif
