/*
 * vfattool: check the folder-backed FAT32 volume with real FAT tools.
 *
 *   vfattool dump  folder size-MB image   write the volume to a (sparse) image
 *   vfattool apply folder size-MB image   write the image's changed sectors
 *                                         to the volume, then write back
 *   vfattool dump16 / apply16 ...         the same with a FAT16 volume
 *
 * tests/vfat.sh dumps a folder, changes the image with mtools and fsck.fat
 * the way a machine would, applies it and checks the folder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/storage/blockdev.h"

int main(int argc, char **argv)
{
    zx_blockdev *bd;
    FILE *fp;
    uint8_t a[ZX_BLOCK], b[ZX_BLOCK], zero[ZX_BLOCK] = { 0 };
    uint64_t lba, changed = 0;

    if (argc != 5 || (strcmp(argv[1], "dump") && strcmp(argv[1], "apply") && strcmp(argv[1], "dump16")
                      && strcmp(argv[1], "apply16"))) {
        fprintf(stderr, "usage: %s dump|apply|dump16|apply16 folder size-MB image\n", argv[0]);
        return 2;
    }
    if (strstr(argv[1], "16"))
        bd = zx_bd_open_folder16(argv[2], (uint32_t)atoi(argv[3]));
    else
        bd = zx_bd_open_folder(argv[2], (uint32_t)atoi(argv[3]));
    if (!bd) {
        fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    fp = fopen(argv[4], argv[1][0] == 'd' ? "wb" : "rb");
    if (!fp) {
        perror(argv[4]);
        return 1;
    }
    for (lba = 0; lba < bd->blocks; lba++) {
        bd->read(bd, lba, a);
        if (argv[1][0] == 'd') {
            if (memcmp(a, zero, ZX_BLOCK) || lba == bd->blocks - 1) {
                fseek(fp, (long)(lba * ZX_BLOCK), SEEK_SET);
                fwrite(a, 1, ZX_BLOCK, fp);
            }
        } else {
            if (fread(b, 1, ZX_BLOCK, fp) != ZX_BLOCK)
                memset(b, 0, ZX_BLOCK);
            if (memcmp(a, b, ZX_BLOCK)) {
                bd->write(bd, lba, b);
                changed++;
            }
        }
    }
    fclose(fp);
    if (argv[1][0] == 'a')
        printf("%llu sectors changed\n", (unsigned long long)changed);
    zx_bd_close(bd);
    return 0;
}
