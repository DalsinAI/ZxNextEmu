/*
 * MOS 6522 Versatile Interface Adapter, stepped one CPU cycle at a time.
 *
 * Timers follow the NMOS part: writing T1 high loads the counter, it
 * counts down from the next cycle, the interrupt flag sets as it passes
 * 0 to FFFF, and in free-run mode the latch reloads on the cycle after,
 * so the period is latch + 2. T2 counts cycles or PB6 falling edges.
 * The shift register keeps its value but does not shift (nothing on the
 * Oric uses it).
 *
 * The machine wires the pins: it sets pa_in/pb_in and the CA1/CB1 lines,
 * reads the port outputs and CA2/CB2, and gets a call when they change.
 */
#ifndef ZXN_VIA6522_H
#define ZXN_VIA6522_H

#include <stdint.h>

enum {
    VIA_IFR_CA2 = 0x01, VIA_IFR_CA1 = 0x02, VIA_IFR_SR = 0x04, VIA_IFR_CB2 = 0x08,
    VIA_IFR_CB1 = 0x10, VIA_IFR_T2 = 0x20, VIA_IFR_T1 = 0x40
};

typedef struct via6522 {
    uint8_t ora, orb, ddra, ddrb;
    uint8_t ira, irb;            /* input latches (ACR bits 0, 1) */
    uint8_t acr, pcr, ifr, ier, sr;
    uint16_t t1c, t1l, t2c;
    uint8_t t2l;
    uint8_t t1_armed, t2_armed;  /* the next underflow raises the flag */
    uint8_t t1_load, t2_load;    /* counter was just written: no decrement this cycle */
    uint8_t t1_reload;           /* free-run: reload from the latch this cycle */
    uint8_t pb7;                 /* T1's PB7 output */
    uint8_t pa_in, pb_in;        /* levels driven onto the port pins from outside */
    uint8_t ca1, cb1;            /* input levels */
    uint8_t ca2, cb2;            /* output levels (or input levels in input modes) */
    uint8_t ca2_pulse, cb2_pulse;    /* pulse mode: back high after this cycle */
    uint8_t pb6_prev;
    /* Called when CA2, CB2, port A or port B outputs may have changed. */
    void (*changed)(void *ctx);
    void *ctx;
} via6522;

void via_reset(via6522 *v);
uint8_t via_read(via6522 *v, int reg);
void via_write(via6522 *v, int reg, uint8_t val);
/* One CPU cycle, after that cycle's bus access. */
void via_tick(via6522 *v);
void via_set_ca1(via6522 *v, int level);
void via_set_cb1(via6522 *v, int level);
void via_set_ca2(via6522 *v, int level);     /* only in CA2 input modes */
static inline int via_irq(const via6522 *v) { return (v->ifr & v->ier & 0x7f) != 0; }
/* What the port pins carry: outputs where DDR is 1, outside levels elsewhere. */
uint8_t via_port_a(const via6522 *v);
uint8_t via_port_b(const via6522 *v);

#endif
