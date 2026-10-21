/* Ricoh RP5C01 real time clock (MSX2, ports B4/B5). It keeps emulated
 * time, so a machine runs the same every time; it starts at 2026-01-01
 * 00:00:00 when the machine is created. */
#ifndef MSX_RTC_H
#define MSX_RTC_H

#include <stdint.h>

typedef struct msx_rtc {
    uint8_t reg[4][13];        /* nibbles; block 0 is the time, BCD */
    uint8_t sel, mode, test;
    uint32_t frac;             /* CPU T-states into the current second */
    uint32_t hz;
} msx_rtc;

void rtc_init(msx_rtc *r, uint32_t cpu_hz);
void rtc_reset(msx_rtc *r);
void rtc_run(msx_rtc *r, uint32_t t);          /* advance by CPU T-states */
void rtc_write(msx_rtc *r, int port, uint8_t val);   /* port 0 = B4, 1 = B5 */
uint8_t rtc_read(const msx_rtc *r);

#endif
