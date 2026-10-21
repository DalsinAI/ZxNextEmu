/*
 * Kempston mouse, shared by the Next and the expansion-port interfaces:
 * X counts up to the right, Y counts up going up, the wheel is a 4-bit
 * count, buttons read 0 when pressed (ps2_mouse.v on the Next).
 *
 * Ports: xBDF reads X, xFDF reads Y, xADF the buttons and wheel.
 */
#ifndef ZXN_KMOUSE_H
#define ZXN_KMOUSE_H

#include <stdint.h>

typedef struct zx_kmouse {
    uint8_t x, y, z, btn;
    int fx, fy;               /* movement left over from DPI scaling */
} zx_kmouse;

/* Host movement (dy down is positive), buttons 1 left, 2 right, 4 middle.
 * unit: host quarter-pixels per count (2 = x2, 4 = x1, 8 = /2, 16 = /4). */
static inline void zx_kmouse_move(zx_kmouse *k, int dx, int dy, int dz, int buttons, int unit)
{
    int x = k->fx + dx * 4, y = k->fy - dy * 4;
    k->x = (uint8_t)(k->x + x / unit);
    k->y = (uint8_t)(k->y + y / unit);
    k->fx = x % unit;
    k->fy = y % unit;
    k->z = (uint8_t)(k->z + dz);
    k->btn = (uint8_t)(buttons & 7);
}

/* hi: the low nibble of the port's high byte (0x0b X, 0x0f Y, else
 * buttons); swap exchanges the left and right buttons. */
static inline uint8_t zx_kmouse_port(const zx_kmouse *k, int hi, int swap)
{
    int l = k->btn & 1, r = k->btn >> 1 & 1, mid = k->btn >> 2 & 1;
    if (swap) {
        int t = l;
        l = r;
        r = t;
    }
    if (hi == 0x0b)
        return k->x;
    if (hi == 0x0f)
        return k->y;
    return (uint8_t)((k->z & 15) << 4 | 8 | !mid << 2 | !l << 1 | !r);
}

#endif
