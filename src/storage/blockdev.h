/*
 * Block devices for emulated storage (SD cards, later disks and
 * microdrives).
 *
 * A device is backed either by an image file or by a host folder. A folder
 * is shown to the machine as a FAT32 volume built on the fly (vfat.c): the
 * machine reads the host files directly, and what it writes is mapped back
 * into the folder shortly after it stops writing, and when the device is
 * closed.
 */
#ifndef ZX_BLOCKDEV_H
#define ZX_BLOCKDEV_H

#include <stdint.h>

#define ZX_BLOCK 512

typedef struct zx_blockdev zx_blockdev;

struct zx_blockdev {
    uint64_t blocks;                /* size in 512-byte blocks */
    int readonly;
    /* 0 on success; a failed read fills the buffer with zeros */
    int (*read)(zx_blockdev *bd, uint64_t lba, uint8_t *buf);
    int (*write)(zx_blockdev *bd, uint64_t lba, const uint8_t *buf);
    /* called once per emulated frame; may write back to the host */
    void (*tick)(zx_blockdev *bd);
    /* write everything back to the host now */
    int (*flush)(zx_blockdev *bd);
    void (*close)(zx_blockdev *bd);
};

/* Host side (blockdev.c, vfat.c). Open an image file, or a folder as a FAT32 volume of the default size. */
zx_blockdev *zx_bd_open(const char *path);
zx_blockdev *zx_bd_open_image(const char *path);
/* size_mb 0 picks a size with room to spare for the folder's contents. */
zx_blockdev *zx_bd_open_folder(const char *path, uint32_t size_mb);

/* Flush and close (inline: the core uses it without the host-side code). */
static inline void zx_bd_close(zx_blockdev *bd)
{
    if (!bd)
        return;
    if (bd->flush)
        bd->flush(bd);
    bd->close(bd);
}

#endif
