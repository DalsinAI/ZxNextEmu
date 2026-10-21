#include "scc.h"

#include <string.h>

void scc_reset(scc_chip *s)
{
    memset(s, 0, sizeof *s);
}

uint8_t scc_read(const scc_chip *s, uint8_t off)
{
    if (off < 0x80)
        return (uint8_t)s->wave[off >> 5][off & 31];
    if (off >= 0xa0 && off < 0xc0)          /* channel 5's view of wave 4 */
        return (uint8_t)s->wave[3][off & 31];
    return 0xff;                            /* the other registers are write only */
}

void scc_write(scc_chip *s, uint8_t off, uint8_t val)
{
    int ch;
    if (off < 0x80) {
        s->wave[off >> 5][off & 31] = (int8_t)val;
        if ((off >> 5) == 3)
            s->wave[4][off & 31] = (int8_t)val;
        return;
    }
    if (off >= 0xe0) {
        s->deform = val;
        return;
    }
    if (off >= 0xa0)
        return;
    off &= 0x8f;                            /* 9890-989F mirror 9880-988F */
    if (off < 0x8a) {
        ch = (off - 0x80) >> 1;
        if (off & 1)
            s->period[ch] = (uint16_t)((s->period[ch] & 0xff) | (val & 15) << 8);
        else
            s->period[ch] = (uint16_t)((s->period[ch] & 0xf00) | val);
        if (s->deform & 0x20)
            s->pos[ch] = 0;
    } else if (off < 0x8f) {
        s->volume[off - 0x8a] = val & 15;
    } else {
        s->enable = val & 0x1f;
    }
}

void scc_run(scc_chip *s, uint32_t clocks)
{
    int ch;
    int32_t out = 0;
    for (ch = 0; ch < 5; ch++) {
        uint32_t step = s->period[ch] + 1u, c = s->count[ch] + clocks;
        if (s->period[ch] < 9) {            /* too high to hear: the chip holds */
            s->count[ch] = 0;
        } else {
            s->pos[ch] = (uint8_t)((s->pos[ch] + c / step) & 31);
            s->count[ch] = c % step;
        }
        if (s->enable & (1 << ch))
            out += s->wave[ch][s->pos[ch]] * s->volume[ch];
    }
    s->acc += out * (int32_t)clocks;
    s->acc_n += (int32_t)clocks;
}

int32_t scc_take(scc_chip *s)
{
    /* Five channels of 8 bit samples times 4 bit volume: +-9600. */
    int32_t v = s->acc_n ? s->acc / s->acc_n : 0;
    s->acc = 0;
    s->acc_n = 0;
    return v * 3;
}
