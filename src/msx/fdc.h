/*
 * Floppy disks: a WD2793 behind the Philips-style interface (as in the
 * NMS 8250), with its registers at xFF8-xFFF of every page of the slot.
 * The disk ROM is the user's. Disks are 512-byte-sector images (.dsk:
 * 360K, 720K and the 8-sector sizes) on block devices; sectors move as
 * fast as the CPU takes them.
 */
#ifndef MSX_FDC_H
#define MSX_FDC_H

#include <stdint.h>

struct zx_blockdev;

typedef struct msx_fdc {
    uint8_t status, track, sector, data, cmd;
    uint8_t side_reg, drive_reg;
    uint8_t irq, drq;
    uint8_t cyl[2];            /* where each drive's head is */
    uint8_t changed[2];
    uint8_t step_in;           /* last step direction */
    uint16_t pos, len;         /* transfer position in buf */
    uint8_t buf[512];
    uint32_t lba;              /* sector being transferred */
    uint8_t multi, writing, type1;
    uint64_t now;              /* machine time in T-states, set by the caller */
    uint32_t cpu_hz;
    struct zx_blockdev *disk[2];
} msx_fdc;

void fdc_init(msx_fdc *f, uint32_t cpu_hz);
void fdc_reset(msx_fdc *f);
/* addr & 0x3fff in 0x3ff8..0x3fff; returns 1 if it is an FDC address. */
int fdc_read(msx_fdc *f, uint16_t addr, uint8_t *val);
int fdc_write(msx_fdc *f, uint16_t addr, uint8_t val);
/* The drive takes ownership of the block device; NULL ejects. */
void fdc_insert(msx_fdc *f, int drive, struct zx_blockdev *bd);
void fdc_frame(msx_fdc *f);
void fdc_free(msx_fdc *f);

#endif
