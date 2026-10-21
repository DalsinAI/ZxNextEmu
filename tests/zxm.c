/*
 * zxm API check: run a machine with typed input, save its state part way,
 * run on, load the state and run the same frames again. Both runs must give
 * the same pictures and sound. Prints a hash of the whole run, which the
 * WebAssembly build (tests/wasm.mjs) must reproduce exactly.
 *
 * usage: zxm romdir machine frames [text]
 * ZXM_SD=folder-or-image puts an SD card in a Next; ZXM_DISK=file.trd|.scl
 * a disk in a Beta 128's drive A; ZXM_DELAY=frames waits before typing. A machine whose ROMs are missing is
 * skipped (exit 77).
 */
#include "../src/core/zxm.h"
#include "../src/storage/blockdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
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
        if (!(fp = fopen(path, "rb"))) {
            if (info->roms_optional >> i & 1)
                continue;
            /* The clones' ROMs are not in the repository: skip. */
            printf("%s: skipped, no %s\n", info->id, path);
            return 77;
        }
        if (fread(rom[i], 1, 0x4000, fp) != 0x4000) {
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
    if (getenv("ZXM_DISK")) {
        zxm_media disk;
        FILE *fp = fopen(getenv("ZXM_DISK"), "rb");
        static uint8_t buf[1 << 20];
        memset(&disk, 0, sizeof disk);
        disk.kind = ZXM_MEDIA_BYTES;
        disk.name = getenv("ZXM_DISK");
        disk.data = buf;
        disk.len = fp ? (uint32_t)fread(buf, 1, sizeof buf, fp) : 0;
        if (!fp || zxm_media_insert(m, "beta.a", &disk)) {
            fprintf(stderr, "cannot insert disk %s\n", getenv("ZXM_DISK"));
            return 1;
        }
        fclose(fp);
    }
    if (argc > 4) {
        /* "\n" in the text is Enter */
        char *t = argv[4], *r = t, *w = t;
        for (; *r; r++)
            *w++ = r[0] == '\\' && r[1] == 'n' ? (r++, '\n') : *r;
        *w = 0;
        zxm_type(m, t, getenv("ZXM_DELAY") ? atoi(getenv("ZXM_DELAY")) : 0);
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
