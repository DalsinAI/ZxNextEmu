#include "rtc.h"

#include <string.h>

/* Bits that exist in each register; the others read 0. */
static const uint8_t mask[4][13] = {
    { 0xf, 0x7, 0xf, 0x7, 0xf, 0x3, 0x7, 0xf, 0x3, 0xf, 0x1, 0xf, 0xf },
    { 0x0, 0x0, 0xf, 0x7, 0xf, 0x3, 0x7, 0xf, 0x3, 0x0, 0x1, 0x3, 0x0 },
    { 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf },
    { 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf },
};

static int get2(const msx_rtc *r, int i)
{
    return r->reg[0][i] + 10 * r->reg[0][i + 1];
}

static void put2(msx_rtc *r, int i, int v)
{
    r->reg[0][i] = (uint8_t)(v % 10);
    r->reg[0][i + 1] = (uint8_t)(v / 10);
}

static int days_in_month(const msx_rtc *r, int month)
{
    static const uint8_t d[13] = { 31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && r->reg[1][11] == 0)
        return 29;
    return d[month > 12 ? 0 : month];
}

static void tick_second(msx_rtc *r)
{
    int h24 = r->reg[1][10] & 1, v;
    v = get2(r, 0) + 1;
    put2(r, 0, v % 60);
    if (v < 60)
        return;
    v = get2(r, 2) + 1;
    put2(r, 2, v % 60);
    if (v < 60)
        return;
    if (h24) {
        v = get2(r, 4) + 1;
        put2(r, 4, v % 24);
        if (v < 24)
            return;
    } else {
        int pm = r->reg[0][5] & 2, h = r->reg[0][4] + 10 * (r->reg[0][5] & 1);
        h = h % 12 + 1;
        if (h == 12)
            pm ^= 2;
        r->reg[0][4] = (uint8_t)(h % 10);
        r->reg[0][5] = (uint8_t)(h / 10 | pm);
        if (h != 12 || pm)
            return;
    }
    r->reg[0][6] = (uint8_t)((r->reg[0][6] + 1) % 7);
    v = get2(r, 7) + 1;
    if (v <= days_in_month(r, get2(r, 9))) {
        put2(r, 7, v);
        return;
    }
    put2(r, 7, 1);
    v = get2(r, 9) + 1;
    if (v <= 12) {
        put2(r, 9, v);
        return;
    }
    put2(r, 9, 1);
    put2(r, 11, (get2(r, 11) + 1) % 100);
    r->reg[1][11] = (uint8_t)((r->reg[1][11] + 1) & 3);
}

void rtc_init(msx_rtc *r, uint32_t cpu_hz)
{
    memset(r, 0, sizeof *r);
    r->hz = cpu_hz;
    /* 2026-01-01 00:00:00, a Thursday; years count from 1980. */
    put2(r, 7, 1);
    put2(r, 9, 1);
    put2(r, 11, 46);
    r->reg[0][6] = 4;
    r->reg[1][10] = 1;          /* 24 hour clock */
    r->reg[1][11] = 46 & 3;
    rtc_reset(r);
}

void rtc_reset(msx_rtc *r)
{
    r->mode = 0x8;              /* timer on, block 0 */
    r->test = 0;
}

void rtc_run(msx_rtc *r, uint32_t t)
{
    if (!(r->mode & 8))
        return;
    r->frac += t;
    while (r->frac >= r->hz) {
        r->frac -= r->hz;
        tick_second(r);
    }
}

void rtc_write(msx_rtc *r, int port, uint8_t val)
{
    int i;
    if (!(port & 1)) {
        r->sel = val & 15;
        return;
    }
    val &= 15;
    switch (r->sel) {
    case 13:
        r->mode = val;
        break;
    case 14:
        r->test = val;
        break;
    case 15:
        if (val & 2)
            r->frac = 0;
        if (val & 1)
            for (i = 0; i < 13; i++)
                r->reg[1][i] = (uint8_t)(i >= 10 ? r->reg[1][i] : 0);
        break;
    default:
        r->reg[r->mode & 3][r->sel] = val & mask[r->mode & 3][r->sel];
        break;
    }
}

uint8_t rtc_read(const msx_rtc *r)
{
    switch (r->sel) {
    case 13:
        return (uint8_t)(0xf0 | r->mode);
    case 14:
    case 15:
        return 0xff;
    default:
        return (uint8_t)(0xf0 | (r->reg[r->mode & 3][r->sel] & mask[r->mode & 3][r->sel]));
    }
}
