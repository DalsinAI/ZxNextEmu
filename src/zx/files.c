/*
 * Host-side helpers that read files for the core: ROMs from a folder, the
 * Next's boot ROM, SD cards from an image or a folder. The core itself
 * (zx.c, next/) never touches the file system, so it builds to WebAssembly
 * unchanged; native front ends link this file as well.
 */
#include "zx.h"
#include "expansion.h"
#include "../next/next.h"
#include "../storage/blockdev.h"

#include <stdio.h>
#include <string.h>

/* 1 when an optional ROM is not there, -1 on an error. */
static int load_rom(uint8_t *dst, const char *dir, const char *name, int optional)
{
    char path[1024];
    FILE *fp;
    size_t n;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    fp = fopen(path, "rb");
    if (!fp && optional)
        return 1;
    if (!fp) {
        fprintf(stderr, "zx: cannot open ROM %s\n", path);
        return -1;
    }
    n = fread(dst, 1, 0x4000, fp);
    fclose(fp);
    if (n != 0x4000) {
        fprintf(stderr, "zx: ROM %s is not 16K\n", path);
        return -1;
    }
    return 0;
}

int zx_init(zx *m, zx_model model, const char *roms_dir)
{
    static uint8_t rom[4][0x4000];
    const uint8_t *roms[4] = { NULL, NULL, NULL, NULL };
    const char *name;
    int i;
    for (i = 0; i < 4; i++) {
        if ((name = zx_rom_file(model, i)) == NULL)
            continue;
        switch (load_rom(rom[i], roms_dir, name, zx_rom_optional(model, i))) {
        case 0: roms[i] = rom[i]; break;
        case 1: break;             /* e.g. the Pentagon without TR-DOS */
        default: return -1;
        }
    }
    return zx_create(m, model, roms);
}

int zx_attach_sd(zx *m, int slot, const char *path)
{
    zx_blockdev *bd;
    if ((bd = zx_bd_open(path)) == NULL)
        return -1;
    if (zx_attach_sd_dev(m, slot, bd)) {
        zx_bd_close(bd);
        return -1;
    }
    return 0;
}

int zx_attach_interface(zx *m, const char *name, const char *rom_path)
{
    static uint8_t rom[0x2000];
    uint32_t features;
    FILE *fp;
    size_t n;
    if (!strcmp(name, "divmmc"))
        features = ZX_EXP_DIVMMC;
    else if (!strcmp(name, "mb03"))
        features = ZX_EXP_MB03;
    else
        return -1;
    if (!rom_path || !(fp = fopen(rom_path, "rb"))) {
        fprintf(stderr, "zx: cannot open the esxDOS ROM %s\n", rom_path ? rom_path : "(none)");
        return -1;
    }
    n = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    if (n != sizeof rom) {
        fprintf(stderr, "zx: %s is not an 8K esxDOS ROM\n", rom_path);
        return -1;
    }
    return zx_exp_attach(m, features, rom, n);
}

int nx_load_bootrom(zx *m, const char *path)
{
    uint8_t data[0x2000];
    FILE *fp = fopen(path, "rb");
    size_t got;
    if (!fp)
        return -1;
    got = fread(data, 1, sizeof data, fp);
    fclose(fp);
    if (got != sizeof data)
        return -1;
    nx_set_bootrom(m, data);
    return 0;
}
