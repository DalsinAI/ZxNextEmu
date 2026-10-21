/*
 * Philips SAA1099 sound generator: six tone channels, two noise
 * generators, two envelope controllers, stereo.
 *
 * Driven by its own master clock (8 MHz on the SAM Coupé and the MB03+):
 * the machine converts CPU time to SAA clocks and calls saa_run. Every
 * generator edge falls on an exact clock, so the output does not depend on
 * how often the machine calls in. Behaviour follows the datasheet plus
 * Dave Hooper's measurements of real chips (SAASound), including the
 * buffered frequency and envelope writes and the 18-bit noise LFSR.
 *
 * Plain data, no pointers: it can be saved with memcpy.
 */
#ifndef ZX_SAA1099_H
#define ZX_SAA1099_H

#include <stdint.h>

typedef struct saa_env {
    uint8_t enabled, ended, looping, phases, mode;
    uint8_t phase, pos, res;        /* res 1: 4-bit steps, 2: 3-bit steps */
    uint8_t invert_right, external;
    uint8_t new_data, next_data;
    uint8_t left, right;            /* current levels 0..15 */
} saa_env;

typedef struct saa_tone {
    uint32_t count;                 /* clocks until the next edge */
    uint8_t level;
    uint8_t octave, offset;         /* in use */
    uint8_t next_octave, next_offset, new_data, ignore_offset;
} saa_tone;

typedef struct saa_noise {
    uint32_t count;
    uint8_t source;                 /* 0..2 internal rates, 3: tone 0 / 3 */
    uint32_t lfsr;
} saa_noise;

typedef struct saa1099 {
    uint8_t reg[32];
    uint8_t addr;
    uint8_t enabled, sync;
    uint8_t amp[6];                 /* bits 0..3 left, 4..7 right */
    uint8_t tone_mix, noise_mix;    /* registers 0x14, 0x15 */
    saa_tone tone[6];
    saa_noise noise[2];
    saa_env env[2];
    /* Output summed over clocks since the last saa_take. */
    int64_t acc_l, acc_r;
    uint32_t acc_n;
} saa1099;

void saa_reset(saa1099 *s);
void saa_write_address(saa1099 *s, uint8_t reg);
void saa_write_data(saa1099 *s, uint8_t val);
/* Advance by `clocks` master clocks. */
void saa_run(saa1099 *s, uint32_t clocks);
/* Average output since the last call, per side, 0 .. SAA_MAX. */
#define SAA_MAX 2880
void saa_take(saa1099 *s, int32_t *left, int32_t *right);

#endif
