/*
 * MOS 6581 / 8580 SID: three voices (sawtooth, triangle, pulse, noise,
 * ring modulation, hard sync, ADSR envelope) and the multimode filter.
 *
 * Driven by its own clock (985248 Hz for a PAL C64, 1022730 Hz NTSC): the
 * machine converts its time to SID clocks and calls sid_run. Oscillators
 * and envelopes step once per clock, as on the chip; the filter is the
 * two-integrator state-variable filter in integer arithmetic, so the
 * output is the same on every host (native and WebAssembly agree).
 * Combined waveforms are approximated by ANDing the single ones.
 *
 * Plain data, no pointers: it can be saved with memcpy.
 */
#ifndef ZX_SID_H
#define ZX_SID_H

#include <stdint.h>

#define SID_PAL_HZ  985248
#define SID_NTSC_HZ 1022730

typedef struct sid_voice {
    uint32_t acc;                   /* 24-bit phase accumulator */
    uint32_t lfsr;                  /* 23-bit noise shift register */
    uint16_t freq, pw;
    uint8_t ctrl, ad, sr;
    uint8_t msb_rose;               /* bit 23 rose this clock (sync source) */
    /* envelope */
    uint8_t env;                    /* 0..255 */
    uint8_t state;                  /* 0 attack, 1 decay/sustain, 2 release */
    uint8_t hold_zero;
    uint16_t rate_count;
    uint8_t exp_count;
} sid_voice;

typedef struct sid {
    sid_voice v[3];
    uint8_t reg[32];                /* as last written */
    uint8_t model;                  /* 0 6581, 1 8580 */
    uint8_t ntsc;
    uint16_t fc;                    /* 11-bit cutoff */
    uint8_t res_filt, mode_vol;     /* registers 0x17, 0x18 */
    int32_t w0, q1024;              /* filter coefficients */
    int32_t vlp, vbp, vhp;          /* filter state */
    int64_t acc;                    /* output summed since the last sid_take */
    uint32_t acc_n;
} sid;

void sid_reset(sid *s);
void sid_write(sid *s, uint8_t reg, uint8_t val);
uint8_t sid_read(sid *s, uint8_t reg);
/* 0 for the 6581, 1 for the 8580; ntsc picks the clock sid_hz reports. */
void sid_set_model(sid *s, int model, int ntsc);
static inline uint32_t sid_hz(const sid *s) { return s->ntsc ? SID_NTSC_HZ : SID_PAL_HZ; }
void sid_run(sid *s, uint32_t clocks);
/* Average output since the last call, about -SID_MAX .. SID_MAX (mono). */
#define SID_MAX 8192
int32_t sid_take(sid *s);

#endif
