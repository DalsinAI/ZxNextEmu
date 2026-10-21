/*
 * Beta 128 disk interface: a WD1793 floppy controller and four drives, as
 * built into the Pentagon, the Scorpion ZS-256 and the ATM Turbo, driven by
 * TR-DOS.
 *
 * The controller works on the raw bytes of the track under the head (6250
 * MFM bytes a revolution, 32 us each at 300 rpm), so every command takes as
 * long as it does on the real drive: seeks at the programmed step rate,
 * sector searches by rotation, one DRQ per byte. The disk itself is a .trd
 * image (16 sectors of 256 bytes a track) behind a block device; a track is
 * built from the image when the head arrives, and what the machine writes
 * goes back to the image sector by sector.
 *
 * Time is the machine's absolute T-state count (frames * frame_t + t).
 */
#ifndef ZX_BETA_H
#define ZX_BETA_H

#include <stdint.h>

struct zx_blockdev;

#define BETA_DRIVES   4
#define BETA_TRACK    6250        /* raw bytes a revolution */
#define BETA_CYLS     86          /* how far the head can travel */
#define BETA_RAW_SLOTS (BETA_CYLS * 2)

typedef struct zx_beta {
    /* WD1793 */
    uint8_t cmd, status, track, sector, data;
    uint8_t intrq, drq;
    uint8_t type1;               /* status shows the type I bits */
    uint8_t hld;                 /* head loaded */
    uint8_t state;
    int8_t dir;                  /* last step direction, +1 in, -1 out */
    uint8_t steps;               /* steps taken by this restore */
    uint8_t single;              /* a single step command (verify follows) */
    uint8_t result;              /* status bits to finish with */
    uint8_t mark;                /* last data mark read; in write track, last byte was F5 */
    uint8_t sys;                 /* last write to the system port (#FF) */
    uint8_t cyl[BETA_DRIVES];    /* where each head is */
    uint16_t pos, left, count;   /* next raw byte of a transfer, bytes to go, of */
    uint16_t crc;                /* running CRC of the field being read or written */
    uint64_t next;               /* when the command takes its next step */
    uint64_t give_up;            /* the 5th index pulse of a search */
    uint32_t byte_t, ms_t;       /* T-states per disk byte and per ms */

    /* The track under the selected head: raw bytes, and which of them are
     * missing-clock sync marks (A1 / C2). */
    int8_t trk_drive, trk_cyl, trk_side;
    uint8_t trk_dirty;
    uint8_t trk[BETA_TRACK];
    uint8_t sync[BETA_TRACK / 8 + 1];

    /* The disks (live, not part of a saved state), and the tracks written
     * on each that a .trd cannot hold (a format other than 16 x 256, as
     * FORMAT's double-sided test writes): BETA_RAW_SLOTS raw tracks,
     * allocated on the first such write, kept until the disk comes out. */
    struct zx_blockdev *disk[BETA_DRIVES];
    uint8_t *raw[BETA_DRIVES];
} zx_beta;

void beta_init(zx_beta *b, uint32_t cpu_hz);
/* The controller's master reset; the disks stay in. */
void beta_reset(zx_beta *b);

/* Ports by their low byte: #1F #3F #5F #7F the WD1793, #FF the system port. */
int beta_port(uint8_t port);
uint8_t beta_read(zx_beta *b, uint8_t port, uint64_t now);
void beta_write(zx_beta *b, uint8_t port, uint8_t v, uint64_t now);
/* Run the controller up to now (the ports do this themselves). */
void beta_update(zx_beta *b, uint64_t now);

/* A disk in drive 0..3, as a block device of .trd layout; the interface
 * owns it from now on. */
int beta_insert(zx_beta *b, int drive, struct zx_blockdev *bd);
/* A disk from a .trd or .scl image (kept in memory, writable). An empty
 * image is a blank 80-track double-sided disk for TR-DOS to format. */
int beta_insert_image(zx_beta *b, int drive, const uint8_t *data, uint32_t len, int scl);
void beta_eject(zx_beta *b, int drive);
void beta_free(zx_beta *b);

/* An in-memory .trd disk, also usable on its own (tests, converters). */
struct zx_blockdev *beta_memdisk(const uint8_t *trd, uint32_t len);
/* .scl to a 640K .trd: 0 on success. */
int beta_scl_to_trd(const uint8_t *scl, uint32_t len, uint8_t *trd, uint32_t trd_len);

#endif
