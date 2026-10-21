/*
 * General Sound: a Spectrum sound card with its own 12 MHz Z80, a 32K ROM
 * (gs105a.rom) and 512K of RAM, playing four 8-bit sample channels with
 * 6-bit volumes; channels 0 and 1 are on the left, 2 and 3 on the right.
 *
 * The Spectrum talks to it through two ports: BB (write a command, read
 * the status: bit 7 data waiting, bit 0 command waiting) and B3 (data in
 * either direction). Inside, the GS's own ports are:
 *
 *   00 w  page at 8000-FFFF: 0 the ROM, n the n'th 32K of RAM
 *   01 r  the command        02 r  the data (clears status bit 7)
 *   03 w  data to the Spectrum (sets bit 7)   04 r  the status
 *   05    clears status bit 0   06-09 w  volumes of channels 0-3
 *   0A    status bit 7 := page bit 0   0B  status bit 0 := volume 0 bit 5
 *
 * 0000-3FFF is the first half of the ROM, 4000-7FFF the first 16K of RAM.
 * Every read from 6000-7FFF also latches the byte into the DAC of channel
 * (address >> 8) & 3. A maskable interrupt comes 37500 times a second.
 * Memory map and ports follow Unreal Speccy's GS emulation.
 *
 * Plain data, no pointers: it can be saved with memcpy.
 */
#ifndef ZX_GS_H
#define ZX_GS_H

#include <stdint.h>
#include "../z80/z80.h"

#define GS_HZ      12000000
#define GS_INT_T   320               /* 12 MHz / 37.5 kHz */
#define GS_ROM     0x8000
#define GS_RAM     0x80000
#define GS_PAGES   (GS_RAM / 0x8000)

typedef struct gs {
    z80 cpu;
    uint64_t clock;                  /* GS T-states since power on */
    uint64_t target;                 /* where gs_run has to get to */
    uint64_t next_int;
    uint8_t page;
    uint8_t cmd, to_gs, from_gs, stat;
    uint8_t vol[4], dac[4];
    int32_t level_l, level_r;
    int64_t acc_l, acc_r;            /* level x T-states since gs_take */
    uint64_t acc_t;
    uint8_t rom[GS_ROM];
    uint8_t ram[GS_RAM];
} gs;

/* rom is the 32K GS ROM. */
void gs_init(gs *g, const uint8_t *rom);
void gs_reset(gs *g);
/* Run for this many GS T-states (the GS catches up in whole instructions). */
void gs_run(gs *g, uint64_t clocks);
/* The Spectrum's side: ports B3 and BB. */
uint8_t gs_read(gs *g, uint8_t port);
void gs_write(gs *g, uint8_t port, uint8_t v);
/* Average output since the last call, about -16000 .. 16000 per side. */
void gs_take(gs *g, int32_t *l, int32_t *r);

#endif
