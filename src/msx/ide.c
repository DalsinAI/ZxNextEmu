#include "ide.h"
#include "../storage/blockdev.h"

#include <string.h>

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DSC  0x10
#define ST_DRDY 0x40
#define ST_BSY  0x80
#define ER_ABRT 0x04
#define ER_IDNF 0x10

#define HEADS 16
#define SPT   63

static void signature(ata_dev *a)
{
    a->error = 1;
    a->count = 1;
    a->sector = 1;
    a->cyl_lo = a->cyl_hi = 0;
    a->head = 0;
    a->status = ST_DRDY | ST_DSC;
    a->dir = 0;
    a->pos = 0;
    a->left = 0;
}

void ide_init(msx_ide *d)
{
    memset(d, 0, sizeof *d);
    d->control = 0xff;          /* power on: registers on, page 31 */
    ide_reset(d);
}

void ide_reset(msx_ide *d)
{
    d->sel = 0;
    d->srst = 0;
    signature(&d->dev[0]);
    signature(&d->dev[1]);
}

static uint8_t reverse(uint8_t b)
{
    b = (uint8_t)((b & 0xf0) >> 4 | (b & 0x0f) << 4);
    b = (uint8_t)((b & 0xcc) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xaa) >> 1 | (b & 0x55) << 1);
    return b;
}

int ide_bank(const msx_ide *d, uint32_t rom_pages)
{
    uint32_t bank = reverse(d->control & 0xf8);
    if (rom_pages && bank >= rom_pages)
        bank &= rom_pages - 1;
    return (int)bank;
}

int ide_regs_on(const msx_ide *d)
{
    return d->control & 1;
}

/* ---- ATA ---------------------------------------------------------------- */

static uint32_t lba_of(const ata_dev *a)
{
    if (a->head & 0x40)
        return (uint32_t)(a->head & 15) << 24 | (uint32_t)a->cyl_hi << 16 | (uint32_t)a->cyl_lo << 8 | a->sector;
    return (((uint32_t)a->cyl_hi << 8 | a->cyl_lo) * HEADS + (a->head & 15)) * SPT + a->sector - 1u;
}

static void set_lba(ata_dev *a, uint32_t lba)
{
    if (a->head & 0x40) {
        a->sector = (uint8_t)lba;
        a->cyl_lo = (uint8_t)(lba >> 8);
        a->cyl_hi = (uint8_t)(lba >> 16);
        a->head = (uint8_t)((a->head & 0xf0) | ((lba >> 24) & 15));
    } else {
        uint32_t cyl = lba / (HEADS * SPT), rest = lba % (HEADS * SPT);
        a->cyl_lo = (uint8_t)cyl;
        a->cyl_hi = (uint8_t)(cyl >> 8);
        a->head = (uint8_t)((a->head & 0xf0) | (rest / SPT));
        a->sector = (uint8_t)(rest % SPT + 1);
    }
}

static void abort_cmd(ata_dev *a, uint8_t err)
{
    a->error = err;
    a->status = ST_DRDY | ST_DSC | ST_ERR;
    a->dir = 0;
}

static void done_cmd(ata_dev *a)
{
    a->error = 0;
    a->status = ST_DRDY | ST_DSC;
    a->dir = 0;
}

static void put_string(uint8_t *buf, int word, int words, const char *s)
{
    int i, n = (int)strlen(s);
    for (i = 0; i < words * 2; i++) {
        char c = i < n ? s[i] : ' ';
        buf[word * 2 + (i ^ 1)] = (uint8_t)c;
    }
}

static void put_word(uint8_t *buf, int word, uint32_t v)
{
    buf[word * 2] = (uint8_t)v;
    buf[word * 2 + 1] = (uint8_t)(v >> 8);
}

static void identify(ata_dev *a)
{
    uint64_t total = a->bd->blocks;
    uint32_t cyls = (uint32_t)(total / (HEADS * SPT)), lba = total > 0x0fffffff ? 0x0fffffff : (uint32_t)total;
    uint32_t chs;
    if (cyls > 16383)
        cyls = 16383;
    chs = cyls * HEADS * SPT;
    memset(a->buf, 0, sizeof a->buf);
    put_word(a->buf, 0, 0x0040);
    put_word(a->buf, 1, cyls);
    put_word(a->buf, 3, HEADS);
    put_word(a->buf, 6, SPT);
    put_string(a->buf, 10, 10, "ZXCRADLE0001");
    put_string(a->buf, 23, 4, "1.0");
    put_string(a->buf, 27, 20, "ZX Cradle IDE disk");
    put_word(a->buf, 47, 0x8001);
    put_word(a->buf, 49, 0x0200);              /* LBA */
    put_word(a->buf, 51, 0x0200);
    put_word(a->buf, 53, 0x0001);
    put_word(a->buf, 54, cyls);
    put_word(a->buf, 55, HEADS);
    put_word(a->buf, 56, SPT);
    put_word(a->buf, 57, chs & 0xffff);
    put_word(a->buf, 58, chs >> 16);
    put_word(a->buf, 60, lba & 0xffff);
    put_word(a->buf, 61, lba >> 16);
    a->pos = 0;
    a->dir = 1;
    a->left = 1;
    a->lba = 0xffffffffu;
    a->status = ST_DRDY | ST_DSC | ST_DRQ;
    a->error = 0;
}

/* Fetch the next sector to read, or finish. */
static void read_next(ata_dev *a)
{
    if (!a->left) {
        done_cmd(a);
        return;
    }
    if (a->lba >= a->bd->blocks) {
        abort_cmd(a, ER_IDNF);
        return;
    }
    a->bd->read(a->bd, a->lba, a->buf);
    a->pos = 0;
    a->dir = 1;
    a->status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void command(ata_dev *a, uint8_t c)
{
    if (!a->bd)
        return;
    switch (c) {
    case 0xec:
        identify(a);
        break;
    case 0x20:
    case 0x21:
        a->lba = lba_of(a);
        a->left = a->count ? a->count : 256;
        read_next(a);
        break;
    case 0x30:
    case 0x31:
        a->lba = lba_of(a);
        a->left = a->count ? a->count : 256;
        if (a->bd->readonly) {
            abort_cmd(a, ER_ABRT);
            break;
        }
        if (a->lba >= a->bd->blocks) {
            abort_cmd(a, ER_IDNF);
            break;
        }
        a->pos = 0;
        a->dir = 2;
        a->status = ST_DRDY | ST_DSC | ST_DRQ;
        break;
    case 0x40:
    case 0x41:
        if (lba_of(a) + (a->count ? a->count : 256u) > a->bd->blocks)
            abort_cmd(a, ER_IDNF);
        else
            done_cmd(a);
        break;
    case 0xe5:
        a->count = 0xff;
        done_cmd(a);
        break;
    case 0x70: case 0x91: case 0xef: case 0xe0: case 0xe1: case 0xe2: case 0xe3:
    case 0xe6: case 0xe7: case 0xc6:
        done_cmd(a);
        break;
    default:
        if ((c & 0xf0) == 0x10)                /* recalibrate */
            done_cmd(a);
        else
            abort_cmd(a, ER_ABRT);
        break;
    }
}

static uint16_t data_read(ata_dev *a)
{
    uint16_t w;
    if (a->dir != 1)
        return 0xffff;
    w = (uint16_t)(a->buf[a->pos] | a->buf[a->pos + 1] << 8);
    a->pos += 2;
    if (a->pos >= 512) {
        if (a->lba == 0xffffffffu) {
            done_cmd(a);
        } else {
            a->lba++;
            a->left--;
            set_lba(a, a->lba);
            if (a->count)
                a->count--;
            read_next(a);
        }
    }
    return w;
}

static void data_write(ata_dev *a, uint16_t w)
{
    if (a->dir != 2)
        return;
    a->buf[a->pos] = (uint8_t)w;
    a->buf[a->pos + 1] = (uint8_t)(w >> 8);
    a->pos += 2;
    if (a->pos >= 512) {
        a->bd->write(a->bd, a->lba, a->buf);
        a->lba++;
        a->left--;
        set_lba(a, a->lba);
        if (a->count)
            a->count--;
        a->pos = 0;
        if (!a->left)
            done_cmd(a);
        else if (a->lba >= a->bd->blocks)
            abort_cmd(a, ER_IDNF);
    }
}

static uint8_t reg_read(msx_ide *d, int r)
{
    ata_dev *a = &d->dev[d->sel];
    if (r == 14)
        r = 7;
    if (d->srst)
        return r == 7 ? 0xff : 0x7f;
    if (!a->bd)
        return r == 6 ? (uint8_t)(0x6f | d->sel << 4) : 0x7f;
    switch (r) {
    case 0: return (uint8_t)data_read(a);
    case 1: return a->error;
    case 2: return a->count;
    case 3: return a->sector;
    case 4: return a->cyl_lo;
    case 5: return a->cyl_hi;
    case 6: return (uint8_t)((a->head & 0xef) | d->sel << 4);
    case 7: return a->status;
    default: return 0x7f;
    }
}

static void reg_write(msx_ide *d, int r, uint8_t v)
{
    int i;
    if (r == 14) {
        if (v & 4) {
            d->srst = 1;
        } else if (d->srst) {
            d->srst = 0;
            ide_reset(d);
        }
        return;
    }
    if (d->srst)
        return;
    for (i = 0; i < 2; i++) {
        ata_dev *a = &d->dev[i];
        switch (r) {
        case 1: a->feature = v; break;
        case 2: a->count = v; break;
        case 3: a->sector = v; break;
        case 4: a->cyl_lo = v; break;
        case 5: a->cyl_hi = v; break;
        case 6: a->head = v; break;
        default: break;
        }
    }
    if (r == 6)
        d->sel = (v >> 4) & 1;
    else if (r == 7)
        command(&d->dev[d->sel], v);
}

/* ---- the Sunrise interface ---------------------------------------------- */

int ide_read(msx_ide *d, uint16_t addr, uint8_t *val)
{
    if (!ide_regs_on(d))
        return 0;
    if ((addr & 0x3e00) == 0x3c00) {
        if (!(addr & 1)) {
            uint16_t w = d->srst || !d->dev[d->sel].bd ? 0x7f7f : data_read(&d->dev[d->sel]);
            d->latch = (uint8_t)(w >> 8);
            *val = (uint8_t)w;
        } else {
            *val = d->latch;
        }
        return 1;
    }
    if ((addr & 0x3f00) == 0x3e00) {
        *val = reg_read(d, addr & 15);
        return 1;
    }
    return 0;
}

int ide_write(msx_ide *d, uint16_t addr, uint8_t val)
{
    if ((addr & 0xbf04) == 0x0104) {
        int changed = d->control != val;
        d->control = val;
        return changed;
    }
    if (!ide_regs_on(d))
        return 0;
    if ((addr & 0x3e00) == 0x3c00) {
        if (!(addr & 1))
            d->latch = val;
        else if (!d->srst && d->dev[d->sel].bd)
            data_write(&d->dev[d->sel], (uint16_t)(d->latch | val << 8));
        return 0;
    }
    if ((addr & 0x3f00) == 0x3e00)
        reg_write(d, addr & 15, val);
    return 0;
}

void ide_attach(msx_ide *d, int unit, zx_blockdev *bd)
{
    if (unit < 0 || unit > 1)
        return;
    zx_bd_close(d->dev[unit].bd);
    d->dev[unit].bd = bd;
    signature(&d->dev[unit]);
}

void ide_frame(msx_ide *d)
{
    int i;
    for (i = 0; i < 2; i++)
        if (d->dev[i].bd && d->dev[i].bd->tick)
            d->dev[i].bd->tick(d->dev[i].bd);
}

void ide_free(msx_ide *d)
{
    ide_attach(d, 0, NULL);
    ide_attach(d, 1, NULL);
}
