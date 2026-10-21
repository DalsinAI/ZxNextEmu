/* Konami SCC (K051649) wavetable sound, in Konami's SCC cartridges. */
#ifndef MSX_SCC_H
#define MSX_SCC_H

#include <stdint.h>

typedef struct scc_chip {
    int8_t wave[5][32];        /* channel 4 plays channel 3's wave */
    uint16_t period[5];        /* 12 bits */
    uint8_t volume[5];         /* 4 bits */
    uint8_t enable;            /* bit per channel */
    uint8_t deform;
    uint32_t count[5];         /* clocks until the next step */
    uint8_t pos[5];            /* 0..31 */
    int32_t acc;               /* summed output since the last take */
    int32_t acc_n;
} scc_chip;

void scc_reset(scc_chip *s);
/* Offset in the 9800-98FF register window. */
uint8_t scc_read(const scc_chip *s, uint8_t off);
void scc_write(scc_chip *s, uint8_t off, uint8_t val);
/* Advance by this many clocks (the CPU clock, 3.58 MHz). */
void scc_run(scc_chip *s, uint32_t clocks);
/* Mean output since the last call, about -32768..32767. */
int32_t scc_take(scc_chip *s);

#endif
