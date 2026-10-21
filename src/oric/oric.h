/*
 * Tangerine Oric-1 and Oric Atmos.
 *
 * A 1 MHz 6502, 64K of RAM with the 16K BASIC ROM over its top, the
 * HCS10017 video ULA, a 6522 VIA and an AY-3-8912 driven through the
 * VIA's port A (CA2 = BC1, CB2 = BDIR). The VIA's port B selects the
 * keyboard row (PB0-2), reads the column sense (PB3, the AY's port A
 * picks the columns), drives the tape motor (PB6) and output (PB7); the
 * tape input is CB1.
 *
 * Time is counted in CPU cycles from the start of the frame (the ULA's
 * vertical sync). Every bus cycle the ULA draws one 6-pixel cell, before
 * the CPU's access in that cycle, as the ULA reads memory on the first
 * half of the clock.
 */
#ifndef ZXN_ORIC_H
#define ZXN_ORIC_H

#include <stddef.h>
#include <stdint.h>

#include "../m6502/m6502.h"
#include "../devices/via6522.h"
#include "../zx/ay.h"

typedef enum { ORIC_1, ORIC_ATMOS, ORIC_MODEL_COUNT } oric_model;

#define ORIC_W 240
#define ORIC_H 224
#define ORIC_HZ 1000000
#define ORIC_LINE 64              /* cycles per scanline */
#define ORIC_AUDIO_RATE 44100

/* The tape deck: .tap bytes sent as the ROM's 2400 / 1200 Hz cycles. */
typedef struct oric_tape {
    uint8_t *data;               /* owned copy of the .tap file */
    uint32_t len;
    /* Play state (part of the machine state). */
    uint32_t pos;                /* byte being sent */
    uint8_t bit;                 /* 0..13 within the byte's frame */
    uint8_t parity;
    uint8_t level;               /* what CB1 sees */
    uint8_t half;                /* second half of the bit's cycle */
    uint32_t count;              /* cycles to the next edge */
    uint16_t width;              /* half-cycle length of the bit being sent */
    uint16_t extra_leader;       /* sync bytes still to repeat */
    uint16_t gap;                /* 1 bits still to send before the next byte */
    uint32_t header_end;         /* gap goes in before this byte */
} oric_tape;

typedef struct oric {
    oric_model model;
    m6502 cpu;
    m6502_bus bus;
    via6522 via;
    ay_chip ay;
    uint8_t ay_bus, ay_data;     /* BDIR/BC1 and port A as last seen */

    uint8_t ram[0x10000];
    uint8_t rom[0x4000];
    uint32_t rom_crc;

    /* Keyboard: keys[row] bit col set while held. */
    uint8_t keys[8];

    /* ULA */
    uint32_t t;                  /* cycle within the frame */
    uint32_t frame_t;            /* this frame's length: 312 or 264 lines */
    uint64_t frames;
    uint8_t mode;                /* video mode attribute: bit 2 HIRES, bit 1 50 Hz */
    uint8_t ink, paper, text;    /* this line's serial attributes */
    uint16_t screen[ORIC_H][ORIC_W];

    /* Audio */
    int16_t audio[1024 * 2];
    int audio_samples;
    uint32_t audio_frac;

    oric_tape tape;
    uint8_t tape_fast;           /* trap the ROM's tape routines */
} oric;

int oric_create(oric *m, oric_model model, const uint8_t *rom, size_t rom_len);
void oric_free(oric *m);
void oric_power_on(oric *m);
void oric_reset(oric *m);        /* the CPU's RESET line */
void oric_nmi(oric *m);          /* the reset button under the case: NMI */
void oric_run_frame(oric *m);
void oric_key(oric *m, int row, int col, int down);
uint8_t oric_peek(const oric *m, uint16_t addr);
void oric_poke(oric *m, uint16_t addr, uint8_t v);

/* Key for an ASCII character, and whether SHIFT is held with it. */
int oric_char_key(int ch, int *row, int *col, int *shift);

/* tape.c */
int oric_tape_insert(oric *m, const uint8_t *data, size_t len);
void oric_tape_eject(oric *m);
void oric_tape_tick(oric *m);    /* one cycle */
/* Before each instruction: the ROM's read-byte and find-sync routines
 * done at once. Returns 1 if it changed the CPU. */
int oric_tape_trap(oric *m);

size_t oric_state_size(const oric *m);
int oric_state_save(const oric *m, void *buf, size_t len);
int oric_state_load(oric *m, const void *buf, size_t len);

#endif
