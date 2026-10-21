/*
 * zxm: the dispatcher. Each machine family (zxm_impl.h) brings its own
 * machines and functions; this file finds machines and passes calls on.
 */
#include "zxm_impl.h"

#include <string.h>

static const zxm_family *const families[] = {
    &zxm_family_zx,
    &zxm_family_oric,
};

#define FAMILIES ((int)(sizeof families / sizeof families[0]))

static const zxm_family *family_of(const zxm_info *info)
{
    int f;
    for (f = 0; f < FAMILIES; f++)
        if (info >= families[f]->machines && info < families[f]->machines + families[f]->count)
            return families[f];
    return NULL;
}

const zxm_info *zxm_machine(int i)
{
    int f;
    if (i < 0)
        return NULL;
    for (f = 0; f < FAMILIES; f++) {
        const zxm_family *fam = families[f];
        if (i < fam->count) {
            zxm_info *info = &fam->machines[i];
            if (!info->cpu_hz && fam->init_info)
                fam->init_info(info, i);
            return info;
        }
        i -= fam->count;
    }
    return NULL;
}

const zxm_info *zxm_lookup(const char *id)
{
    const zxm_info *info;
    int i;
    for (i = 0; (info = zxm_machine(i)) != NULL; i++)
        if (!strcmp(id, info->id))
            return info;
    return NULL;
}

size_t zxm_size(const zxm_info *info)
{
    const zxm_family *fam = info ? family_of(info) : NULL;
    return fam ? fam->size(info) : 0;
}

int zxm_create(zxm *m, const zxm_info *info, const zxm_rom_set *roms)
{
    const zxm_family *fam = info ? family_of(info) : NULL;
    int i;
    if (!fam)
        return -1;
    for (i = 0; i < 4 && info->roms[i]; i++)
        if (!(info->rom_optional >> i & 1) && (!roms || !roms->rom[i] || !roms->len[i]))
            return -1;
    m->info = info;
    m->fam = fam;
    return fam->create(m, roms);
}

void zxm_destroy(zxm *m) { m->fam->destroy(m); }
const zxm_info *zxm_info_of(const zxm *m) { return m->info; }
void zxm_reset(zxm *m, zxm_reset_kind kind) { m->fam->reset(m, kind); }
void zxm_run_frame(zxm *m) { m->fam->run_frame(m); }
const void *zxm_framebuffer(const zxm *m, int *pitch) { return m->fam->framebuffer(m, pitch); }
const int16_t *zxm_audio(const zxm *m, int *samples) { return m->fam->audio(m, samples); }
void zxm_input(zxm *m, const zxm_input_event *ev) { m->fam->input(m, ev); }
void zxm_type(zxm *m, const char *text, int delay_frames) { m->fam->type(m, text, delay_frames); }
int zxm_typing(const zxm *m) { return m->fam->typing(m); }
size_t zxm_state_size(const zxm *m) { return m->fam->state_size(m); }
int zxm_state_save(const zxm *m, void *buf, size_t len) { return m->fam->state_save(m, buf, len); }
int zxm_state_load(zxm *m, const void *buf, size_t len) { return m->fam->state_load(m, buf, len); }
const zxm_debug *zxm_debugger(zxm *m) { return m->fam->debugger(m); }

int zxm_has_ext(const char *name, const char *ext)
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
    if (!s || !media)
        return -1;
    return m->fam->media_insert(m, s, media);
}

int zxm_media_eject(zxm *m, const char *slot)
{
    const zxm_slot *s = find_slot(m, slot);
    if (!s)
        return -1;
    return m->fam->media_eject(m, s);
}
