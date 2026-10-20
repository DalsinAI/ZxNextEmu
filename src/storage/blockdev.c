/*
 * Image-file block devices, and opening a path as an image or a folder.
 */
#define _POSIX_C_SOURCE 200809L
#include "blockdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct image {
    zx_blockdev bd;
    FILE *fp;
} image;

static int img_read(zx_blockdev *bd, uint64_t lba, uint8_t *buf)
{
    image *im = (image *)bd;
    if (lba >= bd->blocks || fseek(im->fp, (long)(lba * ZX_BLOCK), SEEK_SET)
        || fread(buf, 1, ZX_BLOCK, im->fp) != ZX_BLOCK) {
        memset(buf, 0, ZX_BLOCK);
        return -1;
    }
    return 0;
}

static int img_write(zx_blockdev *bd, uint64_t lba, const uint8_t *buf)
{
    image *im = (image *)bd;
    if (bd->readonly || lba >= bd->blocks || fseek(im->fp, (long)(lba * ZX_BLOCK), SEEK_SET)
        || fwrite(buf, 1, ZX_BLOCK, im->fp) != ZX_BLOCK)
        return -1;
    fflush(im->fp);
    return 0;
}

static void img_close(zx_blockdev *bd)
{
    fclose(((image *)bd)->fp);
    free(bd);
}

zx_blockdev *zx_bd_open_image(const char *path)
{
    image *im;
    FILE *fp = fopen(path, "r+b");
    int ro = 0;
    long size;
    if (!fp) {
        fp = fopen(path, "rb");
        ro = 1;
    }
    if (!fp)
        return NULL;
    im = calloc(1, sizeof *im);
    if (!im) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    im->fp = fp;
    im->bd.blocks = size > 0 ? (uint64_t)size / ZX_BLOCK : 0;
    im->bd.readonly = ro;
    im->bd.read = img_read;
    im->bd.write = img_write;
    im->bd.close = img_close;
    return &im->bd;
}

zx_blockdev *zx_bd_open(const char *path)
{
    struct stat st;
    if (stat(path, &st))
        return NULL;
    if (S_ISDIR(st.st_mode))
        return zx_bd_open_folder(path, 0);
    return zx_bd_open_image(path);
}

void zx_bd_close(zx_blockdev *bd)
{
    if (!bd)
        return;
    if (bd->flush)
        bd->flush(bd);
    bd->close(bd);
}
