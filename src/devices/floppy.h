/*
 * Floppy disk images as the drive sees them: per track, the sector IDs in
 * the order they pass the head, and the data behind each one.
 *
 * Formats: MGT (raw SAM / MGT +D, 80x2x10x512 or x9), SAD (Aley Keprt's
 * SAM disk backups), DSK and EDSK (CPC "standard" and "extended", which
 * also hold copy-protected SAM and +3 disks). An MGT can also sit on a
 * block device, so writes go straight back to the host.
 */
#ifndef ZX_FLOPPY_H
#define ZX_FLOPPY_H

#include <stdint.h>

#define FD_MAX_CYLS 84
#define FD_MAX_SECTORS 32

/* fd_sector.flags */
#define FD_ID_CRC     0x01   /* the ID field has a CRC error */
#define FD_DATA_CRC   0x02   /* the data field has a CRC error */
#define FD_DELETED    0x04   /* deleted data address mark */
#define FD_NO_DATA    0x08   /* an ID with no data field */

typedef struct fd_sector {
    uint8_t c, h, r, n;      /* the ID field */
    uint8_t flags;
    uint16_t len;            /* bytes stored for it */
    uint32_t off;            /* where: image offset or block device byte offset */
} fd_sector;

typedef struct fd_track {
    uint8_t count;
    fd_sector s[FD_MAX_SECTORS];
} fd_track;

enum { FD_MGT = 1, FD_SAD, FD_EDSK };

struct zx_blockdev;

typedef struct fd_disk {
    int format;
    int cyls, heads, readonly;
    uint8_t *data;           /* the image, owned (NULL on a block device) */
    uint32_t len;
    struct zx_blockdev *bd;
    fd_track track[FD_MAX_CYLS][2];
} fd_disk;

/* An image from bytes (copied), known by its signature (SAD, DSK, EDSK)
 * or its size (MGT). NULL if it is not one. */
fd_disk *fd_open(const uint8_t *data, uint32_t len, const char *name);
/* An MGT image on a block device (1600 or 1440 blocks); the disk owns it. */
fd_disk *fd_open_blocks(struct zx_blockdev *bd);
void fd_close(fd_disk *d);

/* The track under the head, or NULL if the disk has none there. */
const fd_track *fd_track_at(const fd_disk *d, int cyl, int head);
/* Sector idx of that track; 0 on success. buf holds 128 << (n & 3) bytes. */
int fd_read(fd_disk *d, int cyl, int head, int idx, uint8_t *buf);
int fd_write(fd_disk *d, int cyl, int head, int idx, const uint8_t *buf);
/* Reformat a track from what a Write Track laid down: only a layout the
 * image can hold (the same sector count and sizes). 0 on success. */
int fd_format(fd_disk *d, int cyl, int head, const fd_sector *ids, int n, const uint8_t *const data[]);

/* CRC-CCITT as the FDC computes it. */
uint16_t fd_crc(const uint8_t *p, int n, uint16_t crc);

#endif
