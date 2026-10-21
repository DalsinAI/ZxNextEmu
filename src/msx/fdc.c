#include "fdc.h"
#include "../storage/blockdev.h"

#include <string.h>

/* Status bits */
#define S_BUSY     0x01
#define S_INDEX    0x02        /* type I */
#define S_DRQ      0x02        /* type II/III */
#define S_TRACK0   0x04
#define S_LOST     0x04
#define S_CRC      0x08
#define S_SEEKERR  0x10
#define S_RNF      0x10
#define S_HEAD     0x20
#define S_WPROT    0x40
#define S_NOTREADY 0x80

void fdc_init(msx_fdc *f, uint32_t cpu_hz)
{
    memset(f, 0, sizeof *f);
    f->cpu_hz = cpu_hz;
    fdc_reset(f);
}

void fdc_reset(msx_fdc *f)
{
    f->status = 0;
    f->track = 0;
    f->sector = 1;
    f->data = 0;
    f->cmd = 0;
    f->irq = f->drq = 0;
    f->side_reg = 0;
    f->drive_reg = 0;
    f->type1 = 1;
    f->len = f->pos = 0;
}

static int drive_of(const msx_fdc *f)
{
    switch (f->drive_reg & 3) {
    case 0:
    case 2: return 0;
    case 1: return 1;
    default: return -1;
    }
}

static zx_blockdev *disk(const msx_fdc *f)
{
    int d = drive_of(f);
    return d < 0 ? NULL : f->disk[d];
}

/* Geometry from the image size. */
static void geometry(const zx_blockdev *bd, int *spt, int *sides)
{
    uint64_t b = bd->blocks;
    *spt = (b % 9 == 0) ? 9 : (b % 8 == 0) ? 8 : 9;
    *sides = b > (uint64_t)*spt * 84 ? 2 : 1;
}

/* The index hole passes for 4 ms of every 200 ms turn. */
static int index_pulse(const msx_fdc *f)
{
    uint64_t turn = f->cpu_hz / 5;
    return disk(f) && (f->drive_reg & 0x80) && (f->now % turn) < turn / 50;
}

static uint8_t type1_status(const msx_fdc *f)
{
    uint8_t s = (uint8_t)(f->status & (S_BUSY | S_SEEKERR | S_CRC));
    int d = drive_of(f);
    zx_blockdev *bd = disk(f);
    if (!bd)
        s |= S_NOTREADY;
    else if (bd->readonly)
        s |= S_WPROT;
    if (d >= 0 && f->cyl[d] == 0)
        s |= S_TRACK0;
    if (index_pulse(f))
        s |= S_INDEX;
    if (f->drive_reg & 0x80)
        s |= S_HEAD;
    return s;
}

static void finish(msx_fdc *f, uint8_t flags)
{
    f->status = (uint8_t)((f->status & ~(S_BUSY | S_DRQ)) | flags);
    f->drq = 0;
    f->irq = 1;
    f->len = f->pos = 0;
}

/* Find the sector the registers name; -1 if there is none. */
static int locate(msx_fdc *f, uint32_t *lba)
{
    zx_blockdev *bd = disk(f);
    int spt, sides, d = drive_of(f), side = f->side_reg & 1;
    if (!bd || d < 0)
        return -1;
    geometry(bd, &spt, &sides);
    if (side >= sides || f->sector < 1 || f->sector > spt || f->track != f->cyl[d])
        return -1;
    *lba = ((uint32_t)f->cyl[d] * (uint32_t)sides + (uint32_t)side) * (uint32_t)spt + f->sector - 1u;
    if (*lba >= bd->blocks)
        return -1;
    return 0;
}

static void start_read(msx_fdc *f)
{
    if (locate(f, &f->lba)) {
        finish(f, S_RNF);
        return;
    }
    disk(f)->read(disk(f), f->lba, f->buf);
    f->pos = 0;
    f->len = 512;
    f->data = f->buf[0];
    f->status |= S_DRQ;
    f->drq = 1;
}

static void start_write(msx_fdc *f)
{
    zx_blockdev *bd = disk(f);
    if (bd && bd->readonly) {
        finish(f, S_WPROT);
        return;
    }
    if (locate(f, &f->lba)) {
        finish(f, S_RNF);
        return;
    }
    f->pos = 0;
    f->len = 512;
    f->status |= S_DRQ;
    f->drq = 1;
}

static void command(msx_fdc *f, uint8_t c)
{
    int d = drive_of(f);
    zx_blockdev *bd = disk(f);

    if ((c & 0xf0) == 0xd0) {               /* force interrupt */
        f->status &= ~S_BUSY;
        f->drq = 0;
        f->len = f->pos = 0;
        f->type1 = 1;
        f->irq = (c & 0x08) != 0;
        return;
    }
    if (f->status & S_BUSY)
        return;
    f->cmd = c;
    f->irq = 0;
    f->status = S_BUSY;

    if (!(c & 0x80)) {                      /* type I: restore, seek, step */
        int target = d >= 0 ? f->cyl[d] : 0;
        f->type1 = 1;
        switch (c >> 4) {
        case 0x0:
            f->track = 0;
            target = 0;
            break;
        case 0x1:
            target = f->data;
            f->track = f->data;
            break;
        default:
            if ((c & 0x60) == 0x40)
                f->step_in = 1;
            else if ((c & 0x60) == 0x60)
                f->step_in = 0;
            target += f->step_in ? 1 : -1;
            if (target < 0)
                target = 0;
            if (c & 0x10)
                f->track = (uint8_t)(f->track + (f->step_in ? 1 : -1));
            break;
        }
        if (target > 85)
            target = 85;
        if (d >= 0)
            f->cyl[d] = (uint8_t)target;
        f->status = 0;
        if ((c & 0x04) && bd && f->track != (d >= 0 ? f->cyl[d] : 0))
            f->status |= S_SEEKERR;
        finish(f, f->status);
        return;
    }

    f->type1 = 0;
    if (!bd) {
        finish(f, S_NOTREADY);
        return;
    }
    switch (c >> 5) {
    case 4:                                 /* read sector(s) */
        f->multi = (c & 0x10) != 0;
        f->writing = 0;
        start_read(f);
        break;
    case 5:                                 /* write sector(s) */
        f->multi = (c & 0x10) != 0;
        f->writing = 1;
        start_write(f);
        break;
    case 6: {                               /* read address */
        f->buf[0] = d >= 0 ? f->cyl[d] : 0;
        f->buf[1] = f->side_reg & 1;
        f->buf[2] = 1;
        f->buf[3] = 2;
        f->buf[4] = f->buf[5] = 0;
        f->sector = f->buf[0];
        f->pos = 0;
        f->len = 6;
        f->writing = 0;
        f->multi = 0;
        f->lba = 0xffffffffu;
        f->data = f->buf[0];
        f->status |= S_DRQ;
        f->drq = 1;
        break;
    }
    default:
        if (c & 0x10) {                     /* write track: format */
            if (bd->readonly) {
                finish(f, S_WPROT);
                break;
            }
            f->writing = 2;
            f->pos = 0;
            f->len = 6250;
            f->status |= S_DRQ;
            f->drq = 1;
        } else {                            /* read track: not kept, give gaps */
            finish(f, S_RNF);
        }
        break;
    }
}

/* A whole track written: what was formatted is empty sectors. */
static void format_track(msx_fdc *f)
{
    zx_blockdev *bd = disk(f);
    int spt, sides, s, d = drive_of(f);
    uint8_t b[512];
    if (!bd || d < 0)
        return;
    geometry(bd, &spt, &sides);
    memset(b, 0xe5, sizeof b);
    for (s = 0; s < spt; s++) {
        uint32_t lba = ((uint32_t)f->cyl[d] * (uint32_t)sides + (f->side_reg & 1u)) * (uint32_t)spt + (uint32_t)s;
        if (lba < bd->blocks)
            bd->write(bd, lba, b);
    }
}

int fdc_read(msx_fdc *f, uint16_t addr, uint8_t *val)
{
    switch (addr & 0x3fff) {
    case 0x3ff8:
        *val = f->type1 ? type1_status(f) : (uint8_t)(f->status | (disk(f) ? 0 : S_NOTREADY));
        f->irq = 0;
        return 1;
    case 0x3ff9:
        *val = f->track;
        return 1;
    case 0x3ffa:
        *val = f->sector;
        return 1;
    case 0x3ffb:
        *val = f->data;
        if (f->drq && !f->writing) {
            if (++f->pos < f->len) {
                f->data = f->buf[f->pos];
            } else if (f->multi && f->lba != 0xffffffffu) {
                f->sector++;
                start_read(f);
            } else {
                finish(f, 0);
            }
        }
        return 1;
    case 0x3ffc:
        *val = f->side_reg;
        return 1;
    case 0x3ffd: {
        int d = drive_of(f);
        uint8_t r = f->drive_reg & ~4;
        if (d < 0 || !f->changed[d])
            r |= 4;
        if (d >= 0)
            f->changed[d] = 0;
        *val = r;
        return 1;
    }
    case 0x3ffe:
        *val = 0xff;
        return 1;
    case 0x3fff:
        *val = (uint8_t)(0xff & ~(f->irq ? 0x40 : 0) & ~(f->drq ? 0x80 : 0));
        return 1;
    }
    return 0;
}

int fdc_write(msx_fdc *f, uint16_t addr, uint8_t val)
{
    switch (addr & 0x3fff) {
    case 0x3ff8:
        command(f, val);
        return 1;
    case 0x3ff9:
        f->track = val;
        return 1;
    case 0x3ffa:
        f->sector = val;
        return 1;
    case 0x3ffb:
        f->data = val;
        if (f->drq && f->writing == 1) {
            f->buf[f->pos] = val;
            if (++f->pos >= f->len) {
                zx_blockdev *bd = disk(f);
                if (bd)
                    bd->write(bd, f->lba, f->buf);
                if (f->multi) {
                    f->sector++;
                    start_write(f);
                } else {
                    finish(f, 0);
                }
            }
        } else if (f->drq && f->writing == 2) {
            if (++f->pos >= f->len) {
                format_track(f);
                finish(f, 0);
            }
        }
        return 1;
    case 0x3ffc:
        f->side_reg = val;
        return 1;
    case 0x3ffd:
        f->drive_reg = val;
        return 1;
    }
    return 0;
}

void fdc_insert(msx_fdc *f, int drive, zx_blockdev *bd)
{
    if (drive < 0 || drive > 1)
        return;
    zx_bd_close(f->disk[drive]);
    f->disk[drive] = bd;
    f->changed[drive] = 1;
}

void fdc_frame(msx_fdc *f)
{
    int i;
    for (i = 0; i < 2; i++)
        if (f->disk[i] && f->disk[i]->tick)
            f->disk[i]->tick(f->disk[i]);
}

void fdc_free(msx_fdc *f)
{
    fdc_insert(f, 0, NULL);
    fdc_insert(f, 1, NULL);
}
