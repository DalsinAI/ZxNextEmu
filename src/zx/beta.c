#include "beta.h"
#include "../storage/blockdev.h"

#include <stdlib.h>
#include <string.h>

/* WD1793 status bits */
#define ST_BUSY     0x01
#define ST_DRQ      0x02     /* type II/III */
#define ST_INDEX    0x02     /* type I */
#define ST_LOST     0x04     /* type II/III */
#define ST_TR00     0x04     /* type I */
#define ST_CRC      0x08
#define ST_RNF      0x10     /* type II/III; seek error in type I */
#define ST_DELETED  0x20     /* read: record type F8 */
#define ST_HLD      0x20     /* type I */
#define ST_WP       0x40
#define ST_NOTREADY 0x80

/* System port bits */
#define SYS_RESET   0x04     /* /MR: 0 holds the controller in reset */
#define SYS_SIDE    0x10     /* 0 = upper head (side 1) */

/* Layout of a TR-DOS track as the controller formats it. */
#define SECTORS     16
#define SECTOR_LEN  256

enum {
    S_IDLE,
    S_STEP,          /* type I: compare, step, wait the step rate */
    S_SETTLE,        /* type I verify: head settled, look for an ID */
    S_DONE,          /* finish with `result` at next */
    S_FIND,          /* type II/III: start looking */
    S_READ,          /* a byte for the CPU at next */
    S_READ_CRC,      /* the data field's CRC has gone by */
    S_WR_DRQ,        /* write sector: ask for the first byte */
    S_WR_CHECK,      /* write sector: the byte must be there by now */
    S_WRITE,         /* a byte to the disk at next */
    S_WT_WAIT,       /* write track: the index pulse */
    S_WTRACK         /* write track: a byte to the disk at next */
};

/* What the command reads: a sector's data, an ID field, or the raw track. */
#define READ_SECTOR  0
#define READ_ADDRESS 1
#define READ_TRACK   2

static const uint8_t step_ms[4] = { 6, 12, 20, 30 };   /* 1 MHz WD1793 */

/* ---- CRC (CCITT, as the controller computes it) ---------------------------- */

static uint16_t crc_byte(uint16_t crc, uint8_t v)
{
    int i;
    crc ^= (uint16_t)(v << 8);
    for (i = 0; i < 8; i++)
        crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    return crc;
}

/* CRC of a field that starts with three A1 sync marks. */
static uint16_t crc_after_sync(void)
{
    return crc_byte(crc_byte(crc_byte(0xffff, 0xa1), 0xa1), 0xa1);
}

/* ---- the disk image ---------------------------------------------------------- */

static int drive_of(const zx_beta *b) { return b->sys & 3; }
static int side_of(const zx_beta *b) { return (b->sys & SYS_SIDE) ? 0 : 1; }
static int ready(const zx_beta *b) { return b->disk[drive_of(b)] != NULL; }

/* Sides and cylinders from the image size and the TR-DOS disk type byte. */
static int geometry(zx_beta *b, int drive, int *sides, int *cyls)
{
    zx_blockdev *bd = b->disk[drive];
    uint8_t blk[ZX_BLOCK];
    uint32_t total;
    if (!bd || bd->blocks < 8)
        return -1;
    total = bd->blocks > 0x10000 ? 0x20000 : (uint32_t)bd->blocks * 2;
    if (bd->read(bd, 4, blk))
        memset(blk, 0, sizeof blk);
    *sides = (blk[0xe3] == 0x18 || blk[0xe3] == 0x19) ? 1 : 2;
    *cyls = (int)(total / (SECTORS * (uint32_t)*sides));
    if (*cyls > BETA_CYLS)
        *cyls = BETA_CYLS;
    return 0;
}

/* The image's sector for (cyl, side, r): its 512-byte block and offset. */
static int locate(zx_beta *b, int drive, int cyl, int side, int r, uint64_t *lba, int *off)
{
    int sides, cyls;
    uint32_t n;
    if (r < 1 || r > SECTORS || geometry(b, drive, &sides, &cyls) || cyl >= cyls || side >= sides)
        return -1;
    n = ((uint32_t)cyl * (uint32_t)sides + (uint32_t)side) * SECTORS + (uint32_t)(r - 1);
    *lba = n / 2;
    *off = (int)(n & 1) * SECTOR_LEN;
    return 0;
}

static void write_sector(zx_beta *b, int drive, int cyl, int side, int r, const uint8_t *src)
{
    zx_blockdev *bd = b->disk[drive];
    uint8_t blk[ZX_BLOCK];
    uint64_t lba;
    int off;
    if (!bd || bd->readonly || locate(b, drive, cyl, side, r, &lba, &off))
        return;
    if (bd->read(bd, lba, blk))
        memset(blk, 0, sizeof blk);
    memcpy(blk + off, src, SECTOR_LEN);
    bd->write(bd, lba, blk);
}

/* ---- the track under the head ------------------------------------------------ */

static void put(zx_beta *b, int *p, uint8_t v, int n, int sync)
{
    while (n-- > 0 && *p < BETA_TRACK) {
        b->trk[*p] = v;
        if (sync)
            b->sync[*p >> 3] |= (uint8_t)(1 << (*p & 7));
        else
            b->sync[*p >> 3] &= (uint8_t)~(1 << (*p & 7));
        (*p)++;
    }
}

static int is_sync(const zx_beta *b, int p)
{
    p %= BETA_TRACK;
    return (b->sync[p >> 3] >> (p & 7)) & 1;
}

static uint8_t at(const zx_beta *b, int p)
{
    return b->trk[p % BETA_TRACK];
}

/* A raw track slot: valid flag, the bytes, the sync bitmap. */
#define RAW_LEN (1 + BETA_TRACK + BETA_TRACK / 8 + 1)

static uint8_t *raw_slot(const zx_beta *b, int drive, int cyl, int side)
{
    return b->raw[drive] ? b->raw[drive] + (size_t)(cyl * 2 + side) * RAW_LEN : NULL;
}

static void keep_raw(const zx_beta *b, uint8_t *slot)
{
    slot[0] = 1;
    memcpy(slot + 1, b->trk, BETA_TRACK);
    memcpy(slot + 1 + BETA_TRACK, b->sync, sizeof b->sync);
}

/* Raw MFM track as TR-DOS formats it: gap 4a, index mark, then per sector
 * gap 1, ID field, gap 2, data field. TR-DOS writes the side in the ID's
 * head byte, and FORMAT reads it back to tell a double-sided drive. */
static void build_track(zx_beta *b, int drive, int cyl, int side)
{
    zx_blockdev *bd = b->disk[drive];
    uint8_t blk[ZX_BLOCK];
    int p = 0, r, i;
    memset(b->trk, 0x4e, sizeof b->trk);
    memset(b->sync, 0, sizeof b->sync);
    b->trk_drive = (int8_t)drive;
    b->trk_cyl = (int8_t)cyl;
    b->trk_side = (int8_t)side;
    b->trk_dirty = 0;
    if (!bd)
        return;
    {
        const uint8_t *slot = raw_slot(b, drive, cyl, side);
        if (slot && slot[0]) {
            memcpy(b->trk, slot + 1, BETA_TRACK);
            memcpy(b->sync, slot + 1 + BETA_TRACK, sizeof b->sync);
            return;
        }
    }
    put(b, &p, 0x4e, 80, 0);
    put(b, &p, 0x00, 12, 0);
    put(b, &p, 0xc2, 3, 1);
    put(b, &p, 0xfc, 1, 0);
    for (r = 1; r <= SECTORS; r++) {
        uint64_t lba;
        int off;
        uint16_t crc;
        if (locate(b, drive, cyl, side, r, &lba, &off))
            return;                         /* off the image: unformatted */
        put(b, &p, 0x4e, 40, 0);
        put(b, &p, 0x00, 12, 0);
        put(b, &p, 0xa1, 3, 1);
        put(b, &p, 0xfe, 1, 0);
        put(b, &p, (uint8_t)cyl, 1, 0);
        put(b, &p, (uint8_t)side, 1, 0);
        put(b, &p, (uint8_t)r, 1, 0);
        put(b, &p, 1, 1, 0);
        crc = crc_after_sync();
        for (i = p - 5; i < p; i++)
            crc = crc_byte(crc, b->trk[i]);
        put(b, &p, (uint8_t)(crc >> 8), 1, 0);
        put(b, &p, (uint8_t)crc, 1, 0);
        put(b, &p, 0x4e, 22, 0);
        put(b, &p, 0x00, 12, 0);
        put(b, &p, 0xa1, 3, 1);
        put(b, &p, 0xfb, 1, 0);
        if (bd->read(bd, lba, blk))
            memset(blk, 0, sizeof blk);
        memcpy(b->trk + p, blk + off, SECTOR_LEN);
        crc = crc_after_sync();
        for (i = p - 1; i < p + SECTOR_LEN; i++)
            crc = crc_byte(crc, b->trk[i]);
        p += SECTOR_LEN;
        put(b, &p, (uint8_t)(crc >> 8), 1, 0);
        put(b, &p, (uint8_t)crc, 1, 0);
    }
}

/* A data mark within 43 bytes of q: the index of its first data byte. */
static int find_dam(const zx_beta *b, int q, uint8_t *mark)
{
    int k;
    for (k = 0; k < 43; k++) {
        int p = q + k;
        uint8_t v = at(b, p + 1);
        if (is_sync(b, p) && at(b, p) == 0xa1 && !is_sync(b, p + 1) && v >= 0xf8 && v <= 0xfb) {
            *mark = v;
            return (p + 2) % BETA_TRACK;
        }
    }
    return -1;
}

/* Write every whole 256-byte sector on the cached track back to the image
 * (after a write track: what TR-DOS formatted). A track that is not the
 * standard 16 sectors of 256 bytes, numbered 1 to 16 with this cylinder and
 * side, is also kept raw so it reads back as written. */
static void flush_track(zx_beta *b)
{
    int p, ids = 0, std = 1;
    uint32_t seen = 0;
    uint8_t buf[SECTOR_LEN], *slot;
    b->trk_dirty = 0;
    if (b->trk_drive < 0)
        return;
    for (p = 0; p < BETA_TRACK; p++) {
        uint8_t mark;
        int d, i, r;
        if (!(is_sync(b, p) && b->trk[p] == 0xa1 && at(b, p + 1) == 0xfe && !is_sync(b, p + 1)))
            continue;
        ids++;
        r = at(b, p + 4);
        if (at(b, p + 2) != b->trk_cyl || at(b, p + 3) != b->trk_side || r < 1 || r > SECTORS
            || (seen >> r & 1))
            std = 0;
        seen |= 1u << (r & 31);
        if (at(b, p + 5) != 1 || (d = find_dam(b, p + 8, &mark)) < 0) {
            std = 0;                         /* N: 256-byte sectors only */
            continue;
        }
        for (i = 0; i < SECTOR_LEN; i++)
            buf[i] = at(b, d + i);
        write_sector(b, b->trk_drive, b->trk_cyl, b->trk_side, r, buf);
    }
    if (ids != SECTORS)
        std = 0;
    slot = raw_slot(b, b->trk_drive, b->trk_cyl, b->trk_side);
    if (std) {
        if (slot)
            slot[0] = 0;
        return;
    }
    if (!slot) {
        b->raw[b->trk_drive] = calloc(BETA_RAW_SLOTS, RAW_LEN);
        if (!(slot = raw_slot(b, b->trk_drive, b->trk_cyl, b->trk_side)))
            return;
    }
    keep_raw(b, slot);
}

static void ensure_track(zx_beta *b)
{
    int d = drive_of(b), c = b->cyl[d], s = side_of(b);
    if (b->trk_drive == d && b->trk_cyl == c && b->trk_side == s)
        return;
    if (b->trk_dirty)
        flush_track(b);
    build_track(b, d, c, s);
}

/* ---- rotation ---------------------------------------------------------------- */

/* When raw byte p next starts under the head, at or after t. Byte 0 starts
 * at the index pulse. */
static uint64_t byte_time(const zx_beta *b, uint64_t t, int p)
{
    uint64_t n = (t + b->byte_t - 1) / b->byte_t;
    uint32_t cur = (uint32_t)(n % BETA_TRACK);
    uint32_t d = ((uint32_t)p + BETA_TRACK - cur) % BETA_TRACK;
    return (n + d) * b->byte_t;
}

static int index_pulse(const zx_beta *b, uint64_t now)
{
    /* about 4 ms of the 200 ms revolution */
    return ready(b) && (now / b->byte_t) % BETA_TRACK < 125;
}

/* The fifth index pulse from t: when a search gives up. */
static uint64_t fifth_index(const zx_beta *b, uint64_t t)
{
    return byte_time(b, t, 0) + 4ull * BETA_TRACK * b->byte_t;
}

/* The first ID field to pass the head from time t on that the command
 * wants: *when it has gone by, *q the byte after it. A wanted field with a
 * bad CRC sets *crc_bad and the search goes on. 0 if none in a revolution. */
#define WANT_ANY    0
#define WANT_TRACK  1
#define WANT_SECTOR 2

static int find_id(zx_beta *b, uint64_t t, int want, uint64_t *when, int *q, int *crc_bad)
{
    uint64_t n = (t + b->byte_t - 1) / b->byte_t, base = n * b->byte_t;
    int cur = (int)(n % BETA_TRACK), k;
    ensure_track(b);
    for (k = 0; k < BETA_TRACK; k++) {
        int p = cur + k, i;
        uint16_t crc;
        if (!(is_sync(b, p) && at(b, p) == 0xa1 && at(b, p + 1) == 0xfe && !is_sync(b, p + 1)))
            continue;
        if (want >= WANT_TRACK && at(b, p + 2) != b->track)
            continue;
        if (want == WANT_SECTOR) {
            if (at(b, p + 4) != b->sector)
                continue;
            if ((b->cmd & 0x02) && at(b, p + 3) != ((b->cmd >> 3) & 1))
                continue;
        }
        crc = crc_after_sync();
        for (i = 1; i < 6; i++)
            crc = crc_byte(crc, at(b, p + i));
        if (crc != (uint16_t)(at(b, p + 6) << 8 | at(b, p + 7))) {
            *crc_bad = 1;
            continue;
        }
        *when = base + (uint64_t)(k + 8) * b->byte_t;
        *q = (p + 8) % BETA_TRACK;
        return 1;
    }
    return 0;
}

/* ---- the command engine ------------------------------------------------------- */

static void finish(zx_beta *b, uint8_t st)
{
    b->status = (uint8_t)((b->type1 ? 0 : (b->status & ST_LOST)) | st);
    b->intrq = 1;
    b->state = S_IDLE;
}

static void step_head(zx_beta *b)
{
    int d = drive_of(b), c = b->cyl[d] + b->dir;
    if (c < 0)
        c = 0;
    if (c >= BETA_CYLS)
        c = BETA_CYLS - 1;
    b->cyl[d] = (uint8_t)c;
}

static void type1_end(zx_beta *b)
{
    if (b->cmd & 0x04) {                 /* verify: let the head settle */
        b->hld = 1;
        b->state = S_SETTLE;
        b->next += 15ull * b->ms_t;
    } else {
        finish(b, 0);
    }
}

static void do_step(zx_beta *b)
{
    int op = b->cmd >> 4;
    uint32_t rate = step_ms[b->cmd & 3] * b->ms_t;
    if (op <= 1) {                       /* restore, seek */
        if (op == 0 && b->cyl[drive_of(b)] == 0) {
            b->track = 0;
            type1_end(b);
            return;
        }
        if (op == 1 && b->track == b->data) {
            type1_end(b);
            return;
        }
        if (op == 0 && ++b->steps == 255) {
            finish(b, ST_RNF);           /* no track 0: seek error */
            return;
        }
        b->dir = (op == 0 || b->data < b->track) ? -1 : 1;
        b->track = (uint8_t)(b->track + b->dir);
        step_head(b);
        b->next += rate;
        return;
    }
    if (op >= 4)
        b->dir = (op >= 6) ? -1 : 1;
    if (b->cmd & 0x10)
        b->track = (uint8_t)(b->track + b->dir);
    step_head(b);
    b->next += rate;
    b->state = S_DONE;
    b->single = 1;                       /* S_DONE then goes on to type1_end */
}

static void start_find(zx_beta *b)
{
    int op = b->cmd >> 4, q = 0, crc_bad = 0, d;
    uint64_t when = 0;
    uint8_t mark;
    b->give_up = b->give_up ? b->give_up : fifth_index(b, b->next);
    switch (op) {
    case 0x8: case 0x9: case 0xa: case 0xb:      /* read / write sector */
        if (!find_id(b, b->next, WANT_SECTOR, &when, &q, &crc_bad) || when > b->give_up) {
            b->next = b->give_up;
            b->state = S_DONE;
            b->result = ST_RNF | (crc_bad ? ST_CRC : 0);
            return;
        }
        b->left = b->count = (uint16_t)(128u << (at(b, q - 3) & 3));
        if (op >= 0xa) {
            b->pos = (uint16_t)q;
            b->next = when + 2ull * b->byte_t;
            b->state = S_WR_DRQ;
            return;
        }
        if ((d = find_dam(b, q, &mark)) < 0) {
            b->next = when + 43ull * b->byte_t;
            b->state = S_DONE;
            b->result = ST_RNF;
            return;
        }
        b->mark = mark;
        b->crc = crc_byte(crc_after_sync(), mark);
        b->pos = (uint16_t)d;
        b->next = when + (uint64_t)((d - q + BETA_TRACK) % BETA_TRACK) * b->byte_t;
        b->state = S_READ;
        return;
    case 0xc:                                    /* read address */
        if (!find_id(b, b->next, WANT_ANY, &when, &q, &crc_bad) || when > b->give_up) {
            b->next = b->give_up;
            b->state = S_DONE;
            b->result = ST_RNF;
            return;
        }
        b->pos = (uint16_t)((q - 6 + BETA_TRACK) % BETA_TRACK);
        b->left = 6;
        b->next = when - 6ull * b->byte_t;
        b->state = S_READ;
        return;
    case 0xe:                                    /* read track */
        b->pos = 0;
        b->left = BETA_TRACK;
        b->next = byte_time(b, b->next, 0);
        b->state = S_READ;
        return;
    default:                                     /* write track */
        b->drq = 1;
        b->next = byte_time(b, b->next, 0);
        b->state = S_WT_WAIT;
        return;
    }
}

static int read_kind(const zx_beta *b)
{
    int op = b->cmd >> 4;
    return op == 0xc ? READ_ADDRESS : op == 0xe ? READ_TRACK : READ_SECTOR;
}

static void run_step(zx_beta *b)
{
    switch (b->state) {
    case S_STEP:
        do_step(b);
        break;
    case S_SETTLE: {
        uint64_t when;
        int q, crc_bad = 0;
        uint64_t give_up = fifth_index(b, b->next);
        if (find_id(b, b->next, WANT_TRACK, &when, &q, &crc_bad) && when <= give_up) {
            b->next = when;
            b->result = 0;
        } else {
            b->next = give_up;
            b->result = ST_RNF | (crc_bad ? ST_CRC : 0);
        }
        b->state = S_DONE;
        b->cmd &= (uint8_t)~0x04;
        break;
    }
    case S_DONE:
        if (b->single) {                         /* after a single step */
            b->single = 0;
            type1_end(b);
        } else {
            finish(b, b->result);
        }
        break;
    case S_FIND:
        start_find(b);
        break;
    case S_READ: {
        int kind = read_kind(b);
        ensure_track(b);
        if (b->drq)
            b->status |= ST_LOST;
        b->data = at(b, b->pos);
        b->drq = 1;
        if (kind == READ_SECTOR)
            b->crc = crc_byte(b->crc, b->data);
        else if (kind == READ_ADDRESS && b->left == 6)
            b->sector = b->data;                 /* the track number from the ID */
        b->pos = (uint16_t)((b->pos + 1) % BETA_TRACK);
        b->next += b->byte_t;
        if (--b->left)
            break;
        if (kind == READ_SECTOR) {
            b->next += b->byte_t;                /* the second CRC byte */
            b->state = S_READ_CRC;
        } else if (kind == READ_ADDRESS) {
            int i;
            uint16_t crc = crc_after_sync();
            for (i = 7; i >= 3; i--)
                crc = crc_byte(crc, at(b, b->pos + BETA_TRACK - i));
            finish(b, crc != (uint16_t)(at(b, b->pos + BETA_TRACK - 2) << 8 | at(b, b->pos + BETA_TRACK - 1)) ? ST_CRC : 0);
        } else {
            finish(b, 0);
        }
        break;
    }
    case S_READ_CRC: {
        uint16_t want = (uint16_t)(at(b, b->pos) << 8 | at(b, b->pos + 1));
        uint8_t rt = b->mark <= 0xf9 ? ST_DELETED : 0;
        if (want != b->crc) {
            finish(b, ST_CRC | rt);
        } else if (b->cmd & 0x10) {              /* multiple sectors */
            b->sector++;
            b->give_up = 0;
            b->state = S_FIND;
        } else {
            finish(b, rt);
        }
        break;
    }
    case S_WR_DRQ:
        b->drq = 1;
        b->next += 9ull * b->byte_t;
        b->state = S_WR_CHECK;
        break;
    case S_WR_CHECK: {
        int p = b->pos + 22;
        if (b->drq) {
            b->drq = 0;
            finish(b, ST_LOST);
            break;
        }
        ensure_track(b);
        p %= BETA_TRACK;
        {
            int i;
            uint8_t mark = (b->cmd & 1) ? 0xf8 : 0xfb;
            for (i = 0; i < 16; i++, p = (p + 1) % BETA_TRACK) {
                uint8_t v = i < 12 ? 0x00 : i < 15 ? 0xa1 : mark;
                b->trk[p] = v;
                if (i >= 12 && i < 15)
                    b->sync[p >> 3] |= (uint8_t)(1 << (p & 7));
                else
                    b->sync[p >> 3] &= (uint8_t)~(1 << (p & 7));
            }
            b->crc = crc_byte(crc_after_sync(), mark);
        }
        b->next = byte_time(b, b->next, p);
        b->pos = (uint16_t)p;
        b->state = S_WRITE;
        break;
    }
    case S_WRITE: {
        uint8_t v = b->data;
        int p = b->pos;
        ensure_track(b);
        if (b->drq) {
            b->status |= ST_LOST;
            v = 0;
        }
        b->trk[p] = v;
        b->sync[p >> 3] &= (uint8_t)~(1 << (p & 7));
        b->crc = crc_byte(b->crc, v);
        b->pos = (uint16_t)((p + 1) % BETA_TRACK);
        b->next += b->byte_t;
        if (--b->left) {
            b->drq = 1;
            break;
        }
        {
            uint8_t buf[SECTOR_LEN];
            int i, start = (b->pos + BETA_TRACK - SECTOR_LEN) % BETA_TRACK;
            b->trk[b->pos] = (uint8_t)(b->crc >> 8);
            b->trk[(b->pos + 1) % BETA_TRACK] = (uint8_t)b->crc;
            b->trk[(b->pos + 2) % BETA_TRACK] = 0xff;
            for (i = 0; i < 3; i++)
                b->sync[((b->pos + i) % BETA_TRACK) >> 3] &= (uint8_t)~(1 << ((b->pos + i) % BETA_TRACK & 7));
            if (b->count == SECTOR_LEN) {
                for (i = 0; i < SECTOR_LEN; i++)
                    buf[i] = at(b, start + i);
                write_sector(b, b->trk_drive, b->trk_cyl, b->trk_side, b->sector, buf);
            }
            {
                uint8_t *slot = raw_slot(b, b->trk_drive, b->trk_cyl, b->trk_side);
                if (slot && slot[0])
                    keep_raw(b, slot);
            }
        }
        b->next += 3ull * b->byte_t;
        if (b->cmd & 0x10) {
            b->sector++;
            b->give_up = 0;
            b->state = S_FIND;
        } else {
            b->state = S_DONE;
            b->result = 0;
        }
        break;
    }
    case S_WT_WAIT:
        if (b->drq) {
            b->drq = 0;
            finish(b, ST_LOST);
            break;
        }
        ensure_track(b);
        b->pos = 0;
        b->left = BETA_TRACK;
        b->mark = 0;                             /* previous byte was not F5 */
        b->trk_dirty = 1;
        b->state = S_WTRACK;
        /* fall through */
    case S_WTRACK: {
        uint8_t v = b->data;
        int n = 1, p = b->pos;
        if (b->drq) {
            b->status |= ST_LOST;
            v = 0;
        }
        if (v == 0xf5) {                         /* A1 sync, CRC preset */
            if (!b->mark)
                b->crc = 0xffff;
            b->trk[p] = 0xa1;
            b->sync[p >> 3] |= (uint8_t)(1 << (p & 7));
            b->crc = crc_byte(b->crc, 0xa1);
            b->mark = 1;
        } else if (v == 0xf6) {                  /* C2 sync */
            b->trk[p] = 0xc2;
            b->sync[p >> 3] |= (uint8_t)(1 << (p & 7));
            b->mark = 0;
        } else if (v == 0xf7) {                  /* the CRC, two bytes */
            uint16_t crc = b->crc;
            b->trk[p] = (uint8_t)(crc >> 8);
            b->sync[p >> 3] &= (uint8_t)~(1 << (p & 7));
            if (b->left > 1) {
                int p2 = p + 1;
                b->trk[p2] = (uint8_t)crc;
                b->sync[p2 >> 3] &= (uint8_t)~(1 << (p2 & 7));
                n = 2;
            }
            b->mark = 0;
        } else {
            b->trk[p] = v;
            b->sync[p >> 3] &= (uint8_t)~(1 << (p & 7));
            b->crc = crc_byte(b->crc, v);
            b->mark = 0;
        }
        b->pos = (uint16_t)(p + n);
        b->left = (uint16_t)(b->left - n);
        b->next += (uint64_t)n * b->byte_t;
        if (b->left) {
            b->drq = 1;
        } else {
            b->drq = 0;
            flush_track(b);
            finish(b, 0);
        }
        break;
    }
    default:
        b->state = S_IDLE;
        break;
    }
}

void beta_update(zx_beta *b, uint64_t now)
{
    while (b->state != S_IDLE && b->next <= now)
        run_step(b);
}

static void command(zx_beta *b, uint8_t v, uint64_t now)
{
    if ((v & 0xf0) == 0xd0) {            /* force interrupt */
        if (b->state != S_IDLE) {
            if (b->trk_dirty)
                flush_track(b);
            b->state = S_IDLE;
            b->status &= (uint8_t)~ST_BUSY;
        } else {
            b->type1 = 1;
        }
        b->intrq = (v & 0x08) ? 1 : 0;
        return;
    }
    if (b->state != S_IDLE)
        return;
    b->cmd = v;
    b->intrq = 0;
    b->drq = 0;
    b->next = now;
    b->give_up = 0;
    b->steps = 0;
    b->single = 0;
    b->result = 0;
    if (!(v & 0x80)) {                   /* type I */
        b->type1 = 1;
        b->status = ST_BUSY;
        if (v & 0x08)
            b->hld = 1;
        if ((v & 0xf0) == 0) {           /* restore */
            b->track = 0xff;
            b->data = 0;
        }
        b->state = S_STEP;
        return;
    }
    b->type1 = 0;
    b->status = ST_BUSY;
    b->hld = 1;
    if (!ready(b)) {
        finish(b, ST_NOTREADY);
        return;
    }
    if ((v & 0xe0) == 0xa0 || (v & 0xf0) == 0xf0) {   /* writes */
        zx_blockdev *bd = b->disk[drive_of(b)];
        if (bd->readonly) {
            finish(b, ST_WP);
            return;
        }
    }
    if (v & 0x04)                        /* E: 15 ms head settle */
        b->next += 15ull * b->ms_t;
    b->state = S_FIND;
}

/* ---- ports ----------------------------------------------------------------------- */

int beta_port(uint8_t port)
{
    return (port & 0x9f) == 0x1f || port == 0xff;
}

uint8_t beta_read(zx_beta *b, uint8_t port, uint64_t now)
{
    beta_update(b, now);
    if (port == 0xff)
        return (uint8_t)((b->intrq ? 0x80 : 0) | (b->drq ? 0x40 : 0) | 0x3f);
    switch ((port >> 5) & 3) {
    case 0: {
        uint8_t s;
        b->intrq = 0;
        if (b->type1) {
            s = b->status & (ST_BUSY | ST_CRC | ST_RNF);
            if (b->hld)
                s |= ST_HLD;
            if (b->cyl[drive_of(b)] == 0)
                s |= ST_TR00;
            if (index_pulse(b, now))
                s |= ST_INDEX;
            if (ready(b) && b->disk[drive_of(b)]->readonly)
                s |= ST_WP;
        } else {
            s = (uint8_t)(b->status & 0x7d);
            if (b->drq)
                s |= ST_DRQ;
        }
        if (!ready(b))
            s |= ST_NOTREADY;
        return s;
    }
    case 1: return b->track;
    case 2: return b->sector;
    default:
        b->drq = 0;
        return b->data;
    }
}

void beta_write(zx_beta *b, uint8_t port, uint8_t v, uint64_t now)
{
    beta_update(b, now);
    if (port == 0xff) {
        uint8_t old = b->sys;
        b->sys = v;
        if (!(v & SYS_RESET) && (old & SYS_RESET))
            beta_reset(b);
        return;
    }
    switch ((port >> 5) & 3) {
    case 0: command(b, v, now); break;
    case 1: b->track = v; break;
    case 2: b->sector = v; break;
    default:
        b->data = v;
        b->drq = 0;
        break;
    }
}

/* ---- setup ----------------------------------------------------------------------- */

void beta_init(zx_beta *b, uint32_t cpu_hz)
{
    memset(b, 0, sizeof *b);
    b->ms_t = cpu_hz / 1000;
    b->byte_t = b->ms_t * 32 / 1000;
    b->trk_drive = -1;
    b->sys = SYS_RESET | SYS_SIDE;
    beta_reset(b);
}

void beta_reset(zx_beta *b)
{
    if (b->trk_dirty)
        flush_track(b);
    b->state = S_IDLE;
    b->cmd = 0;
    b->status = 0;
    b->sector = 1;
    b->intrq = b->drq = 0;
    b->type1 = 1;
}

int beta_insert(zx_beta *b, int drive, zx_blockdev *bd)
{
    if (drive < 0 || drive >= BETA_DRIVES || !bd)
        return -1;
    beta_eject(b, drive);
    b->disk[drive] = bd;
    if (b->trk_drive == drive)
        b->trk_drive = -1;               /* rebuild from the new disk */
    return 0;
}

void beta_eject(zx_beta *b, int drive)
{
    if (drive < 0 || drive >= BETA_DRIVES)
        return;
    if (b->trk_drive == drive) {
        if (b->trk_dirty)
            flush_track(b);
        b->trk_drive = -1;
    }
    zx_bd_close(b->disk[drive]);
    b->disk[drive] = NULL;
    free(b->raw[drive]);
    b->raw[drive] = NULL;
}

void beta_free(zx_beta *b)
{
    int i;
    for (i = 0; i < BETA_DRIVES; i++)
        beta_eject(b, i);
}

/* ---- in-memory disks and .scl ------------------------------------------------ */

#define TRD_SIZE (80 * 2 * SECTORS * SECTOR_LEN)

typedef struct memdisk {
    zx_blockdev bd;
    uint8_t *data;
} memdisk;

static int md_read(zx_blockdev *bd, uint64_t lba, uint8_t *buf)
{
    memdisk *d = (memdisk *)bd;
    if (lba >= bd->blocks) {
        memset(buf, 0, ZX_BLOCK);
        return -1;
    }
    memcpy(buf, d->data + lba * ZX_BLOCK, ZX_BLOCK);
    return 0;
}

static int md_write(zx_blockdev *bd, uint64_t lba, const uint8_t *buf)
{
    memdisk *d = (memdisk *)bd;
    if (lba >= bd->blocks)
        return -1;
    memcpy(d->data + lba * ZX_BLOCK, buf, ZX_BLOCK);
    return 0;
}

static void md_close(zx_blockdev *bd)
{
    memdisk *d = (memdisk *)bd;
    free(d->data);
    free(d);
}

zx_blockdev *beta_memdisk(const uint8_t *trd, uint32_t len)
{
    memdisk *d = calloc(1, sizeof *d);
    uint32_t size = len > TRD_SIZE ? (len + ZX_BLOCK - 1) / ZX_BLOCK * ZX_BLOCK : TRD_SIZE;
    if (!d || !(d->data = calloc(1, size))) {
        free(d);
        return NULL;
    }
    if (trd && len)
        memcpy(d->data, trd, len);
    d->bd.blocks = size / ZX_BLOCK;
    d->bd.read = md_read;
    d->bd.write = md_write;
    d->bd.close = md_close;
    return &d->bd;
}

int beta_scl_to_trd(const uint8_t *scl, uint32_t len, uint8_t *trd, uint32_t trd_len)
{
    uint32_t n, i, src, sec = 16, files;
    uint8_t *info = trd + 8 * SECTOR_LEN;
    if (len < 9 || memcmp(scl, "SINCLAIR", 8) || trd_len < TRD_SIZE)
        return -1;
    files = scl[8];
    if (files > 128 || 9 + files * 14 > len)
        return -1;
    memset(trd, 0, TRD_SIZE);
    src = 9 + files * 14;
    for (i = 0; i < files; i++) {
        const uint8_t *h = scl + 9 + i * 14;
        uint8_t *e = trd + i * 16;
        n = h[13];
        if (src + n * SECTOR_LEN > len || sec + n > 160 * SECTORS)
            return -1;
        memcpy(e, h, 14);
        e[14] = (uint8_t)(sec % SECTORS);
        e[15] = (uint8_t)(sec / SECTORS);
        memcpy(trd + sec * SECTOR_LEN, scl + src, n * SECTOR_LEN);
        src += n * SECTOR_LEN;
        sec += n;
    }
    info[0xe1] = (uint8_t)(sec % SECTORS);
    info[0xe2] = (uint8_t)(sec / SECTORS);
    info[0xe3] = 0x16;                    /* 80 tracks, double sided */
    info[0xe4] = (uint8_t)files;
    info[0xe5] = (uint8_t)((160 * SECTORS - sec) & 0xff);
    info[0xe6] = (uint8_t)((160 * SECTORS - sec) >> 8);
    info[0xe7] = 0x10;                    /* TR-DOS */
    memset(info + 0xea, ' ', 9);
    memset(info + 0xf5, ' ', 8);
    return 0;
}

int beta_insert_image(zx_beta *b, int drive, const uint8_t *data, uint32_t len, int scl)
{
    zx_blockdev *bd;
    if (scl) {
        uint8_t *trd = malloc(TRD_SIZE);
        if (!trd)
            return -1;
        if (beta_scl_to_trd(data, len, trd, TRD_SIZE)) {
            free(trd);
            return -1;
        }
        bd = beta_memdisk(trd, TRD_SIZE);
        free(trd);
    } else {
        bd = beta_memdisk(data, len);
    }
    if (!bd)
        return -1;
    return beta_insert(b, drive, bd);
}
