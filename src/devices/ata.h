/*
 * ATA (IDE) disks: a master and a slave on one cable, with the eight task
 * file registers (0 data, 1 error/feature, 2 count, 3 sector, 4 cylinder
 * low, 5 cylinder high, 6 drive/head, 7 status/command) and 14, the device
 * control register. Disks are block devices: an image, or a host folder
 * shown as a FAT volume. Reads and writes complete at once (no BSY time);
 * IDENTIFY, READ/WRITE SECTORS, VERIFY and the do-nothing power and cache
 * commands are understood, CHS and LBA addressing.
 */
#ifndef ZX_ATA_H
#define ZX_ATA_H

#include <stdint.h>

struct zx_blockdev;

typedef struct ata_dev {
    uint8_t error, count, sector, cyl_lo, cyl_hi, head, status, feature;
    uint8_t buf[512];
    uint16_t pos;              /* byte position in buf while DRQ */
    uint8_t dir;               /* 0 none, 1 to the host, 2 from the host */
    uint32_t lba;              /* next sector to transfer */
    uint16_t left;             /* sectors left in the command */
    struct zx_blockdev *bd;
} ata_dev;

typedef struct ata_bus {
    uint8_t sel;               /* selected device */
    uint8_t srst;              /* soft reset held */
    ata_dev dev[2];
} ata_bus;

void ata_reset(ata_bus *d);
uint8_t ata_reg_read(ata_bus *d, int r);
void ata_reg_write(ata_bus *d, int r, uint8_t v);
/* The data register 16 bits at a time (0x7f7f with no disk). */
uint16_t ata_data_read(ata_bus *d);
void ata_data_write(ata_bus *d, uint16_t w);
/* unit 0 master, 1 slave; NULL ejects. The bus owns the device. */
void ata_attach(ata_bus *d, int unit, struct zx_blockdev *bd);
void ata_tick(ata_bus *d);     /* once a frame: lets folder volumes sync */
void ata_free(ata_bus *d);

#endif
