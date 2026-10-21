/*
 * WD1772 / VL1772 floppy disk controller with one drive, in real time.
 *
 * The disk turns at 300 rpm and the head reads a byte every 32 us (250
 * kbit/s MFM, 6250 bytes a track): ID fields and data fields pass under
 * the head at their own times, DRQ rises for each byte and a byte the CPU
 * does not take in time is lost, steps take the programmed step rate,
 * the motor spins up over six index pulses and stops after nine idle
 * revolutions. Without a disk there are no index pulses, as on a real
 * drive.
 *
 * Time is the machine's absolute CPU clock (T-states since power on); the
 * machine runs the controller up to "now" on each access and once a frame.
 * Plain data, no pointers: it can be saved with memcpy. The disk is passed
 * in on each call.
 */
#ifndef ZX_WD177X_H
#define ZX_WD177X_H

#include <stdint.h>
#include "floppy.h"

#define WD_TRACK_BYTES 6250

typedef struct wd177x {
    uint8_t cmd, status, track, sector, data;
    uint8_t cyl, side, step_in;    /* head position, side select, last direction */
    uint8_t type;                  /* 1..4: what the status register means */
    uint8_t motor, spun;           /* motor on; spin-up complete */
    uint8_t phase, drq, intrq;
    uint8_t found;                 /* sector index the head is working on */
    uint16_t pos, len;             /* transfer position and length in buf */
    uint32_t byte_t, ms;           /* CPU clocks per byte and per ms */
    uint64_t when;                 /* time of the next event */
    uint64_t deadline;             /* when a search gives up */
    uint64_t motor_off;            /* when the motor stops if idle */
    uint8_t buf[WD_TRACK_BYTES];
} wd177x;

/* Registers: 0 command/status, 1 track, 2 sector, 3 data. */
void wd_init(wd177x *w, uint32_t cpu_hz);
void wd_reset(wd177x *w);
void wd_run(wd177x *w, fd_disk *d, uint64_t now);
uint8_t wd_read(wd177x *w, fd_disk *d, int reg, uint64_t now);
void wd_write(wd177x *w, fd_disk *d, int reg, uint8_t v, uint64_t now);
/* The drive light: motor on. */
int wd_active(const wd177x *w);

#endif
