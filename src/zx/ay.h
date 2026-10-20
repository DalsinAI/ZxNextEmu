/* General Instrument AY-3-8912 / Yamaha YM2149 sound chip. */
#ifndef ZXN_AY_H
#define ZXN_AY_H

#include <stdint.h>

typedef struct ay_chip {
    uint8_t reg[16];
    uint8_t sel;
    uint16_t tone_cnt[3];
    uint8_t tone_out[3];
    uint16_t noise_cnt;
    uint32_t lfsr;
    uint8_t noise_half;    /* noise runs at half the tone tick rate */
    uint32_t env_cnt;
    uint8_t env_half;
    int env_step;          /* 0..15 */
    uint8_t env_holding, env_attack, env_alt, env_hold;
    /* Tick sub-divider: the chip ticks once per 8 input clocks. */
    uint32_t clk_div;
    /* Accumulated output since the last ay_take(), per channel. */
    int32_t acc[3];
    int32_t acc_n;
} ay_chip;

void ay_reset(ay_chip *ay);
void ay_write(ay_chip *ay, uint8_t val);    /* write to selected register */
uint8_t ay_read(const ay_chip *ay);
/* Advance by `clocks` AY input clocks, accumulating output. */
void ay_run(ay_chip *ay, uint32_t clocks);
/* Average channel levels (0..32767) since the last call. */
void ay_take(ay_chip *ay, int32_t out[3]);

#endif
