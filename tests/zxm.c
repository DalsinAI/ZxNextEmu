/*
 * zxm API check: run a machine with typed input, save its state part way,
 * run on, load the state and run the same frames again. Both runs must give
 * the same pictures and sound. Prints a hash of the whole run, which the
 * WebAssembly build (tests/wasm.mjs) must reproduce exactly.
 *
 * usage: zxm romdir machine frames [text]
 * ZXM_SD=folder-or-image puts an SD card in the machine (the Next, or an
 * interface's divMMC). ZXM_IFACE=divmmc|mb03 plugs an interface into a
 * classic machine, with its 8K esxDOS ROM from ZXM_IFACE_ROM.
 * ZXM_SPEC256=folder loads a Spec256 game (machine spec256): its graphics
 * files into the "gfx" slot, then its .sna.
 */
#define _DEFAULT_SOURCE /* scandir */
#include "../src/core/zxm.h"
#include "../src/storage/blockdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static int insert_file(zxm *m, const char *slot, const char *dir, const char *name)
{
    char path[1024];
    FILE *fp;
    long n;
    uint8_t *data;
    zxm_media md;
    int r;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(fp = fopen(path, "rb")))
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = malloc(n ? (size_t)n : 1);
    if (!data || fread(data, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(data);
        return -1;
    }
    fclose(fp);
    memset(&md, 0, sizeof md);
    md.kind = ZXM_MEDIA_BYTES;
    md.name = name;
    md.data = data;
    md.len = (uint32_t)n;
    r = zxm_media_insert(m, slot, &md);
    free(data);
    return r;
}

/* Every file but the snapshot into "gfx" (in name order, so the native and
 * WebAssembly checks agree), then the .sna. */
static int load_spec256(zxm *m, const char *dir)
{
    struct dirent **list;
    char sna[256] = "";
    int n = scandir(dir, &list, NULL, alphasort), i;
    if (n < 0)
        return -1;
    for (i = 0; i < n; i++) {
        const char *f = list[i]->d_name;
        size_t l = strlen(f);
        if (f[0] != '.') {
            if (l > 4 && (!strcmp(f + l - 4, ".sna") || !strcmp(f + l - 4, ".SNA")))
                snprintf(sna, sizeof sna, "%s", f);
            else
                insert_file(m, "gfx", dir, f);   /* other files are refused */
        }
        free(list[i]);
    }
    free(list);
    return sna[0] ? insert_file(m, "snapshot", dir, sna) : -1;
}

static uint32_t run(zxm *m, int frames, uint32_t h)
{
    const zxm_info *info = zxm_info_of(m);
    int i, y, pitch, samples;
    for (i = 0; i < frames; i++) {
        const uint16_t *fb;
        const int16_t *au;
        zxm_run_frame(m);
        fb = zxm_framebuffer(m, &pitch);
        for (y = 0; y < info->fb_h; y++)
            h = fnv(h, fb + (size_t)y * (size_t)pitch, info->fb_w * 2u);
        au = zxm_audio(m, &samples);
        h = fnv(h, au, (size_t)samples * 4);
    }
    return h;
}

int main(int argc, char **argv)
{
    static uint8_t rom[4][0x4000];
    const zxm_info *info;
    zxm_rom_set roms;
    zxm *m;
    void *state;
    size_t len;
    int frames, half, i;
    uint32_t h1, h2, h0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s romdir machine frames [text]\n", argv[0]);
        return 2;
    }
    if (!(info = zxm_lookup(argv[2]))) {
        fprintf(stderr, "unknown machine %s\n", argv[2]);
        return 2;
    }
    frames = atoi(argv[3]);
    half = frames / 2;
    memset(&roms, 0, sizeof roms);
    for (i = 0; info->roms[i]; i++) {
        char path[1024];
        FILE *fp;
        snprintf(path, sizeof path, "%s/%s", argv[1], info->roms[i]);
        if (!(fp = fopen(path, "rb")) || fread(rom[i], 1, 0x4000, fp) != 0x4000) {
            fprintf(stderr, "cannot read %s\n", path);
            return 1;
        }
        fclose(fp);
        roms.rom[i] = rom[i];
        roms.len[i] = 0x4000;
    }
    m = malloc(zxm_size(info));
    if (!m || zxm_create(m, info, &roms)) {
        fprintf(stderr, "cannot create %s\n", info->id);
        return 1;
    }
    if (getenv("ZXM_IFACE")) {
        static uint8_t esx[0x2000];
        const char *path = getenv("ZXM_IFACE_ROM");
        zxm_rom_set ir;
        FILE *fp;
        if (!path || !(fp = fopen(path, "rb")) || fread(esx, 1, sizeof esx, fp) != sizeof esx) {
            fprintf(stderr, "cannot read the esxDOS ROM (ZXM_IFACE_ROM)\n");
            return 1;
        }
        fclose(fp);
        memset(&ir, 0, sizeof ir);
        ir.rom[0] = esx;
        ir.len[0] = sizeof esx;
        if (zxm_attach(m, getenv("ZXM_IFACE"), &ir)) {
            fprintf(stderr, "cannot plug in %s\n", getenv("ZXM_IFACE"));
            return 1;
        }
    }
    if (getenv("ZXM_SD")) {
        zxm_media sd;
        memset(&sd, 0, sizeof sd);
        sd.kind = ZXM_MEDIA_BLOCKS;
        sd.blocks = zx_bd_open(getenv("ZXM_SD"));
        if (!sd.blocks || zxm_media_insert(m, "sd0", &sd)) {
            fprintf(stderr, "cannot insert SD card %s\n", getenv("ZXM_SD"));
            return 1;
        }
    }
    if (getenv("ZXM_SPEC256") && load_spec256(m, getenv("ZXM_SPEC256"))) {
        fprintf(stderr, "cannot load Spec256 game %s\n", getenv("ZXM_SPEC256"));
        return 1;
    }
    if (argc > 4) {
        /* "\n" in the text is Enter */
        char *t = argv[4], *r = t, *w = t;
        for (; *r; r++)
            *w++ = r[0] == '\\' && r[1] == 'n' ? (r++, '\n') : *r;
        *w = 0;
        zxm_type(m, t, 0);
    }

    h0 = run(m, half, 2166136261u);
    len = zxm_state_size(m);
    state = malloc(len);
    if (!state || zxm_state_save(m, state, len)) {
        fprintf(stderr, "state save failed\n");
        return 1;
    }
    h1 = run(m, frames - half, h0);
    /* Upset the machine, then put it back. */
    zxm_reset(m, ZXM_POWER_ON);
    run(m, 3, 0);
    if (zxm_state_load(m, state, len)) {
        fprintf(stderr, "state load failed\n");
        return 1;
    }
    h2 = run(m, frames - half, h0);
    printf("%s %d frames: %08x state %zu bytes%s\n", info->id, frames, h1, len,
           h1 == h2 ? "" : " MISMATCH after state load");
    zxm_destroy(m);
    free(m);
    free(state);
    return h1 == h2 ? 0 : 1;
}
