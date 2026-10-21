/*
 * ULA+ check on every classic machine: a short program sets palette entry
 * 15 (CLUT 0 paper 7) to red and turns the palette on; the border and the
 * white paper must then both be drawn red.
 *
 * usage: ulaplus romdir
 */
#include "../src/core/zxm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t prog[] = {
    0x01, 0x3b, 0xbf,   /* ld bc,0xbf3b   */
    0x3e, 0x0f,         /* ld a,15        palette entry 15 */
    0xed, 0x79,         /* out (c),a      */
    0x06, 0xff,         /* ld b,0xff      */
    0x3e, 0x1c,         /* ld a,0x1c      GGGRRRBB: full red */
    0xed, 0x79,         /* out (c),a      */
    0x06, 0xbf,         /* ld b,0xbf      */
    0x3e, 0x40,         /* ld a,0x40      mode group */
    0xed, 0x79,         /* out (c),a      */
    0x06, 0xff,         /* ld b,0xff      */
    0x3e, 0x01,         /* ld a,1         palette on */
    0xed, 0x79,         /* out (c),a      */
    0x18, 0xfe,         /* jr $           */
};

int main(int argc, char **argv)
{
    static const char *const ids[] = { "zx48", "zx128", "plus2", "plus2a", "plus3", "pentagon" };
    static uint8_t rom[4][0x4000];
    size_t k;
    int fails = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: %s romdir\n", argv[0]);
        return 2;
    }
    for (k = 0; k < sizeof ids / sizeof ids[0]; k++) {
        const zxm_info *info = zxm_lookup(ids[k]);
        const zxm_debug *dbg;
        zxm_rom_set roms;
        zxm_regs r;
        const uint16_t *fb;
        zxm *m = malloc(zxm_size(info));
        int i, pitch;
        memset(&roms, 0, sizeof roms);
        for (i = 0; info->roms[i]; i++) {
            char path[1024];
            FILE *fp;
            snprintf(path, sizeof path, "%s/%s", argv[1], info->roms[i]);
            if (!(fp = fopen(path, "rb")) && (info->rom_optional >> i & 1))
                continue;
            if (!fp || fread(rom[i], 1, 0x4000, fp) != 0x4000) {
                fprintf(stderr, "cannot read %s\n", path);
                return 1;
            }
            fclose(fp);
            roms.rom[i] = rom[i];
            roms.len[i] = 0x4000;
        }
        if (!m || zxm_create(m, info, &roms))
            return 1;
        for (i = 0; i < 100; i++)
            zxm_run_frame(m);
        dbg = zxm_debugger(m);
        for (i = 0; i < (int)sizeof prog; i++)
            dbg->poke(m, (uint16_t)(0x8000 + i), prog[i]);
        dbg->get_regs(m, &r);
        r.pc = 0x8000;
        r.iff1 = r.iff2 = 0;
        dbg->set_regs(m, &r);
        zxm_run_frame(m);
        zxm_run_frame(m);
        fb = zxm_framebuffer(m, &pitch);
        /* (0,0) is border; (40,40) is paper at the top of the screen. */
        if (fb[0] != 0x1c0 || fb[40 * pitch + 40] != 0x1c0) {
            printf("%s: ULA+ FAIL border %03x paper %03x\n", ids[k], fb[0], fb[40 * pitch + 40]);
            fails++;
        } else {
            printf("%s: ULA+ ok\n", ids[k]);
        }
        zxm_destroy(m);
        free(m);
    }
    return fails != 0;
}
