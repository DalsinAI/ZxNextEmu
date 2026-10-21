#include "via6522.h"

#include <string.h>

static void changed(via6522 *v)
{
    if (v->changed)
        v->changed(v->ctx);
}

static int ca2_mode(const via6522 *v) { return v->pcr >> 1 & 7; }
static int cb2_mode(const via6522 *v) { return v->pcr >> 5 & 7; }

/* Manual output modes drive the line; input modes leave it pulled high. */
static void pcr_lines(via6522 *v)
{
    int m = ca2_mode(v);
    if (m == 6 || m == 7)
        v->ca2 = m & 1;
    else if (m < 4)
        v->ca2 = 1;
    m = cb2_mode(v);
    if (m == 6 || m == 7)
        v->cb2 = m & 1;
    else if (m < 4)
        v->cb2 = 1;
}

void via_reset(via6522 *v)
{
    void (*cb)(void *) = v->changed;
    void *ctx = v->ctx;
    uint8_t pa = v->pa_in, pb = v->pb_in, ca1 = v->ca1, cb1 = v->cb1;
    memset(v, 0, sizeof *v);
    v->changed = cb;
    v->ctx = ctx;
    v->pa_in = pa;
    v->pb_in = pb;
    v->ca1 = ca1;
    v->cb1 = cb1;
    /* The timers and latches are not cleared by RESET; these are what a
     * part tends to power up with. */
    v->t1c = v->t1l = 0xffff;
    v->t2c = 0xffff;
    v->t2l = 0xff;
    v->pb7 = 1;
    v->pb6_prev = 1;
    pcr_lines(v);
    changed(v);
}

uint8_t via_port_a(const via6522 *v)
{
    return (uint8_t)((v->ora & v->ddra) | (v->pa_in & ~v->ddra));
}

uint8_t via_port_b(const via6522 *v)
{
    uint8_t b = (uint8_t)((v->orb & v->ddrb) | (v->pb_in & ~v->ddrb));
    if (v->acr & 0x80)
        b = (uint8_t)((b & 0x7f) | v->pb7 << 7);
    return b;
}

/* Reading or writing ORA (register 1) or ORB clears the port's flags and
 * runs the CA2 / CB2 handshake. */
static void port_a_access(via6522 *v)
{
    int m = ca2_mode(v);
    v->ifr &= (uint8_t)~VIA_IFR_CA1;
    if (m != 1 && m != 3)
        v->ifr &= (uint8_t)~VIA_IFR_CA2;
    if (m == 4 || m == 5) {
        v->ca2 = 0;
        v->ca2_pulse = m == 5 ? 2 : 0;
        changed(v);
    }
}

static void port_b_access(via6522 *v, int write)
{
    int m = cb2_mode(v);
    v->ifr &= (uint8_t)~VIA_IFR_CB1;
    if (m != 1 && m != 3)
        v->ifr &= (uint8_t)~VIA_IFR_CB2;
    if (write && (m == 4 || m == 5)) {
        v->cb2 = 0;
        v->cb2_pulse = m == 5 ? 2 : 0;
        changed(v);
    }
}

uint8_t via_read(via6522 *v, int reg)
{
    uint8_t r;
    switch (reg & 15) {
    case 0:
        r = (v->acr & 2) ? (uint8_t)((v->orb & v->ddrb) | (v->irb & ~v->ddrb)) : via_port_b(v);
        if ((v->acr & 0x80))
            r = (uint8_t)((r & 0x7f) | v->pb7 << 7);
        port_b_access(v, 0);
        return r;
    case 1:
        r = (v->acr & 1) ? v->ira : via_port_a(v);
        port_a_access(v);
        return r;
    case 2: return v->ddrb;
    case 3: return v->ddra;
    case 4:
        v->ifr &= (uint8_t)~VIA_IFR_T1;
        return (uint8_t)v->t1c;
    case 5: return (uint8_t)(v->t1c >> 8);
    case 6: return (uint8_t)v->t1l;
    case 7: return (uint8_t)(v->t1l >> 8);
    case 8:
        v->ifr &= (uint8_t)~VIA_IFR_T2;
        return (uint8_t)v->t2c;
    case 9: return (uint8_t)(v->t2c >> 8);
    case 10:
        v->ifr &= (uint8_t)~VIA_IFR_SR;
        return v->sr;
    case 11: return v->acr;
    case 12: return v->pcr;
    case 13: return (uint8_t)(v->ifr | (via_irq(v) ? 0x80 : 0));
    case 14: return (uint8_t)(v->ier | 0x80);
    default: return (v->acr & 1) ? v->ira : via_port_a(v);
    }
}

void via_write(via6522 *v, int reg, uint8_t val)
{
    switch (reg & 15) {
    case 0:
        v->orb = val;
        port_b_access(v, 1);
        changed(v);
        break;
    case 1:
        v->ora = val;
        port_a_access(v);
        changed(v);
        break;
    case 2: v->ddrb = val; changed(v); break;
    case 3: v->ddra = val; changed(v); break;
    case 4: case 6:
        v->t1l = (uint16_t)((v->t1l & 0xff00) | val);
        break;
    case 5:
        v->t1l = (uint16_t)((v->t1l & 0x00ff) | val << 8);
        v->t1c = v->t1l;
        v->ifr &= (uint8_t)~VIA_IFR_T1;
        v->t1_armed = 1;
        v->t1_load = 1;
        v->t1_reload = 0;
        if (v->acr & 0x80) {
            v->pb7 = 0;
            changed(v);
        }
        break;
    case 7:
        v->t1l = (uint16_t)((v->t1l & 0x00ff) | val << 8);
        v->ifr &= (uint8_t)~VIA_IFR_T1;
        break;
    case 8: v->t2l = val; break;
    case 9:
        v->t2c = (uint16_t)(v->t2l | val << 8);
        v->ifr &= (uint8_t)~VIA_IFR_T2;
        v->t2_armed = 1;
        v->t2_load = 1;
        break;
    case 10:
        v->sr = val;
        v->ifr &= (uint8_t)~VIA_IFR_SR;
        break;
    case 11: v->acr = val; changed(v); break;
    case 12:
        v->pcr = val;
        pcr_lines(v);
        changed(v);
        break;
    case 13: v->ifr &= (uint8_t)~(val & 0x7f); break;
    case 14:
        if (val & 0x80)
            v->ier |= val & 0x7f;
        else
            v->ier &= (uint8_t)~val;
        break;
    default:
        v->ora = val;
        changed(v);
        break;
    }
}

void via_tick(via6522 *v)
{
    if (v->t1_load) {
        v->t1_load = 0;
    } else if (v->t1_reload) {
        v->t1_reload = 0;
        v->t1c = v->t1l;
    } else if (v->t1c-- == 0) {
        if (v->t1_armed) {
            v->ifr |= VIA_IFR_T1;
            if (v->acr & 0x40) {
                v->t1_reload = 1;
                if (v->acr & 0x80) {
                    v->pb7 ^= 1;
                    changed(v);
                }
            } else {
                v->t1_armed = 0;
                if (v->acr & 0x80) {
                    v->pb7 = 1;
                    changed(v);
                }
            }
        } else if (v->acr & 0x40) {
            v->t1_reload = 1;
        }
    }

    if (v->acr & 0x20) {
        int pb6 = via_port_b(v) >> 6 & 1;
        if (v->pb6_prev && !pb6 && v->t2c-- == 1 && v->t2_armed) {
            v->ifr |= VIA_IFR_T2;
            v->t2_armed = 0;
        }
        v->pb6_prev = (uint8_t)pb6;
        v->t2_load = 0;
    } else if (v->t2_load) {
        v->t2_load = 0;
    } else if (v->t2c-- == 0 && v->t2_armed) {
        v->ifr |= VIA_IFR_T2;
        v->t2_armed = 0;
    }

    if (v->ca2_pulse && !--v->ca2_pulse) {
        v->ca2 = 1;
        changed(v);
    }
    if (v->cb2_pulse && !--v->cb2_pulse) {
        v->cb2 = 1;
        changed(v);
    }
}

void via_set_ca1(via6522 *v, int level)
{
    level = !!level;
    if (level == v->ca1)
        return;
    v->ca1 = (uint8_t)level;
    if (level == (v->pcr & 1)) {
        v->ifr |= VIA_IFR_CA1;
        if (v->acr & 1)
            v->ira = via_port_a(v);
        if (ca2_mode(v) == 4) {
            v->ca2 = 1;
            changed(v);
        }
    }
}

void via_set_cb1(via6522 *v, int level)
{
    level = !!level;
    if (level == v->cb1)
        return;
    v->cb1 = (uint8_t)level;
    if (level == (v->pcr >> 4 & 1)) {
        v->ifr |= VIA_IFR_CB1;
        if (v->acr & 2)
            v->irb = via_port_b(v);
        if (cb2_mode(v) == 4) {
            v->cb2 = 1;
            changed(v);
        }
    }
}

void via_set_ca2(via6522 *v, int level)
{
    int m = ca2_mode(v);
    level = !!level;
    if (m >= 4 || level == v->ca2)
        return;
    v->ca2 = (uint8_t)level;
    if (level == (m >> 1 & 1))
        v->ifr |= VIA_IFR_CA2;
}
