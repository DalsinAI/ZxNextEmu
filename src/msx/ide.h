/*
 * Sunrise IDE: a flash ROM (Nextor) paged 16K at a time through the
 * control register at 4104, and an ATA interface at 7C00-7EFF, with a
 * master and a slave disk. Disks are block devices: an image, or a host
 * folder shown as a FAT16 volume (Nextor reads FAT12 and FAT16 only).
 */
#ifndef MSX_IDE_H
#define MSX_IDE_H

#include <stdint.h>

#include "../devices/ata.h"

struct zx_blockdev;

typedef struct msx_ide {
    uint8_t control;           /* 4104: bit 0 IDE on, bits 7..3 ROM page (reversed) */
    uint8_t latch;             /* data register high byte */
    ata_bus bus;
} msx_ide;

void ide_init(msx_ide *d);
void ide_reset(msx_ide *d);
/* The 16K ROM page shown at 4000-7FFF. */
int ide_bank(const msx_ide *d, uint32_t rom_pages);
int ide_regs_on(const msx_ide *d);
/* Accesses to the IDE's slot; reads return 1 when it answered. */
int ide_read(msx_ide *d, uint16_t addr, uint8_t *val);
/* Returns 1 when the ROM page or the register window changed. */
int ide_write(msx_ide *d, uint16_t addr, uint8_t val);
void ide_attach(msx_ide *d, int unit, struct zx_blockdev *bd);
void ide_frame(msx_ide *d);
void ide_free(msx_ide *d);

#endif
