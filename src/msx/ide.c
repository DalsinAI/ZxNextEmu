#include "ide.h"

#include <string.h>

void ide_init(msx_ide *d)
{
    memset(d, 0, sizeof *d);
    d->control = 0xff;          /* power on: registers on, page 31 */
    ide_reset(d);
}

void ide_reset(msx_ide *d)
{
    ata_reset(&d->bus);
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

/* ---- the Sunrise interface ---------------------------------------------- */

int ide_read(msx_ide *d, uint16_t addr, uint8_t *val)
{
    if (!ide_regs_on(d))
        return 0;
    if ((addr & 0x3e00) == 0x3c00) {
        if (!(addr & 1)) {
            uint16_t w = ata_data_read(&d->bus);
            d->latch = (uint8_t)(w >> 8);
            *val = (uint8_t)w;
        } else {
            *val = d->latch;
        }
        return 1;
    }
    if ((addr & 0x3f00) == 0x3e00) {
        *val = ata_reg_read(&d->bus, addr & 15);
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
        else
            ata_data_write(&d->bus, (uint16_t)(d->latch | val << 8));
        return 0;
    }
    if ((addr & 0x3f00) == 0x3e00)
        ata_reg_write(&d->bus, addr & 15, val);
    return 0;
}

void ide_attach(msx_ide *d, int unit, struct zx_blockdev *bd)
{
    ata_attach(&d->bus, unit, bd);
}

void ide_frame(msx_ide *d)
{
    ata_tick(&d->bus);
}

void ide_free(msx_ide *d)
{
    ata_free(&d->bus);
}
