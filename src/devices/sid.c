/*
 * MOS 6581 / 8580 SID (sid.h).
 *
 * Envelope rates, the exponential decay steps, the noise register taps and
 * the filter structure follow reSID's description of the chip (Dag Lem).
 */
#include "sid.h"

#include <string.h>

/* SID clocks per envelope step, for each 4-bit rate. */
static const uint16_t rate_period[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251
};

enum { ATTACK, DECAY_SUSTAIN, RELEASE };

/* Decay and release slow down as the level falls (a piecewise
 * exponential): steps per level change at each level. */
static uint8_t exp_period(uint8_t env)
{
    if (env > 93) return 1;
    if (env > 54) return 2;
    if (env > 26) return 4;
    if (env > 14) return 8;
    if (env > 6) return 16;
    return 30;
}

static void update_filter(sid *s)
{
    int32_t f;
    /* Cutoff in Hz from the 11-bit register: close to linear on the 8580;
     * the 6581's curve starts higher and is steeper. */
    if (s->model)
        f = 30 + (int32_t)s->fc * 12000 / 2047;
    else
        f = 200 + (int32_t)s->fc * 9800 / 2047;
    s->w0 = f * 6588 / 1000;                /* 2 pi f, Q20 per microsecond */
    s->q1024 = 1024 * 1500 / (1060 + 100 * (s->res_filt >> 4)); /* 1024 / Q */
}

void sid_reset(sid *s)
{
    uint8_t model = s->model, ntsc = s->ntsc;
    int i;
    memset(s, 0, sizeof *s);
    s->model = model;
    s->ntsc = ntsc;
    for (i = 0; i < 3; i++) {
        s->v[i].lfsr = 0x7ffff8;
        s->v[i].state = RELEASE;
        s->v[i].hold_zero = 1;
    }
    update_filter(s);
}

void sid_set_model(sid *s, int model, int ntsc)
{
    s->model = (uint8_t)(model != 0);
    s->ntsc = (uint8_t)(ntsc != 0);
    update_filter(s);
}

void sid_write(sid *s, uint8_t reg, uint8_t val)
{
    sid_voice *v;
    reg &= 0x1f;
    s->reg[reg] = val;
    if (reg < 21) {
        v = &s->v[reg / 7];
        switch (reg % 7) {
        case 0: v->freq = (uint16_t)((v->freq & 0xff00) | val); break;
        case 1: v->freq = (uint16_t)((v->freq & 0x00ff) | val << 8); break;
        case 2: v->pw = (uint16_t)((v->pw & 0xf00) | val); break;
        case 3: v->pw = (uint16_t)((v->pw & 0x0ff) | (val & 15) << 8); break;
        case 4:
            if ((val & 1) && !(v->ctrl & 1)) {
                v->state = ATTACK;
                v->hold_zero = 0;
            } else if (!(val & 1) && (v->ctrl & 1)) {
                v->state = RELEASE;
            }
            if (val & 8) {               /* test: hold the oscillator at 0 */
                v->acc = 0;
                v->lfsr = 0x7ffff8;
            }
            v->ctrl = val;
            break;
        case 5: v->ad = val; break;
        case 6: v->sr = val; break;
        }
        return;
    }
    switch (reg) {
    case 0x15: s->fc = (uint16_t)((s->fc & 0x7f8) | (val & 7)); update_filter(s); break;
    case 0x16: s->fc = (uint16_t)((s->fc & 7) | val << 3); update_filter(s); break;
    case 0x17: s->res_filt = val; update_filter(s); break;
    case 0x18: s->mode_vol = val; break;
    }
}

static uint16_t wave(const sid *s, int i)
{
    const sid_voice *v = &s->v[i];
    const sid_voice *src = &s->v[(i + 2) % 3];
    uint16_t out = 0xfff;
    uint8_t sel = v->ctrl >> 4;
    if (!sel)
        return 0x800;
    if (sel & 1) {                       /* triangle, with ring modulation */
        uint32_t msb = v->acc & 0x800000;
        if (v->ctrl & 4)
            msb ^= src->acc & 0x800000;
        out &= (uint16_t)(((msb ? ~v->acc : v->acc) >> 11) & 0xfff);
    }
    if (sel & 2)                         /* sawtooth */
        out &= (uint16_t)(v->acc >> 12);
    if (sel & 4)                         /* pulse */
        out &= ((v->ctrl & 8) || (v->acc >> 12) >= v->pw) ? 0xfff : 0;
    if (sel & 8) {                       /* noise */
        uint32_t r = v->lfsr;
        out &= (uint16_t)(((r & 0x400000) >> 11) | ((r & 0x100000) >> 10) | ((r & 0x010000) >> 7)
                          | ((r & 0x002000) >> 5) | ((r & 0x000800) >> 4) | ((r & 0x000080) >> 1)
                          | ((r & 0x000010) << 1) | ((r & 0x000004) << 2));
    }
    return out;
}

static void envelope(sid_voice *v)
{
    uint16_t period;
    uint8_t sustain;
    if (v->state == ATTACK)
        period = rate_period[v->ad >> 4];
    else if (v->state == DECAY_SUSTAIN)
        period = rate_period[v->ad & 15];
    else
        period = rate_period[v->sr & 15];
    if (++v->rate_count < period)
        return;
    v->rate_count = 0;
    if (v->state == ATTACK) {
        v->exp_count = 0;
        if (++v->env == 0xff)
            v->state = DECAY_SUSTAIN;
        return;
    }
    if (v->hold_zero || ++v->exp_count < exp_period(v->env))
        return;
    v->exp_count = 0;
    sustain = (uint8_t)((v->sr >> 4) * 17);
    if (v->state == DECAY_SUSTAIN ? v->env > sustain : v->env > 0)
        v->env--;
    if (v->env == 0)
        v->hold_zero = 1;
}

/* All three envelopes are held at zero and the filter has settled: the
 * output is silence until a register is written. Not while a noise
 * register or a sync has to be clocked as well. */
static int silent(const sid *s)
{
    int i;
    for (i = 0; i < 3; i++)
        if (s->v[i].env || !s->v[i].hold_zero || s->v[i].state == ATTACK || (s->v[i].ctrl & 0x82))
            return 0;
    return !s->vlp && !s->vbp && !s->vhp;
}

void sid_run(sid *s, uint32_t clocks)
{
    if (silent(s)) {
        int i;
        for (i = 0; i < 3; i++) {
            sid_voice *v = &s->v[i];
            /* the envelope's rate counter keeps going: what envelope()
             * would do clock by clock */
            uint32_t p = v->state == DECAY_SUSTAIN ? rate_period[v->ad & 15] : rate_period[v->sr & 15];
            uint32_t c = clocks;
            if (c && v->rate_count + 1u >= p) {
                c--;             /* at or past the period: back to 0 next clock */
                v->rate_count = 0;
            }
            v->rate_count = (uint16_t)((v->rate_count + c) % p);
            if (!(v->ctrl & 8)) {
                /* and the noise register steps each time bit 19 rises */
                int64_t a = (int64_t)v->acc - 0x80000, d = (int64_t)v->freq * clocks;
                int64_t steps = ((a + d) >> 20) - (a >> 20);
                while (steps-- > 0)
                    v->lfsr = ((v->lfsr << 1) | (((v->lfsr >> 22) ^ (v->lfsr >> 17)) & 1)) & 0x7fffff;
                v->acc = (uint32_t)((v->acc + d) & 0xffffff);
            }
        }
        s->acc_n += clocks;
        return;
    }
    while (clocks--) {
        int32_t vi = 0, vnf = 0, out;
        int i;
        for (i = 0; i < 3; i++) {
            sid_voice *v = &s->v[i];
            uint32_t prev = v->acc;
            v->msb_rose = 0;
            if (!(v->ctrl & 8)) {
                v->acc = (v->acc + v->freq) & 0xffffff;
                v->msb_rose = !(prev & 0x800000) && (v->acc & 0x800000);
                if (!(prev & 0x080000) && (v->acc & 0x080000))
                    v->lfsr = ((v->lfsr << 1) | (((v->lfsr >> 22) ^ (v->lfsr >> 17)) & 1)) & 0x7fffff;
            }
            envelope(v);
        }
        for (i = 0; i < 3; i++)          /* hard sync from the previous voice */
            if ((s->v[i].ctrl & 2) && s->v[(i + 2) % 3].msb_rose)
                s->v[i].acc = 0;
        for (i = 0; i < 3; i++) {
            int32_t o = ((int32_t)wave(s, i) - 0x800) * s->v[i].env >> 4;
            if (s->res_filt & (1 << i))
                vi += o;
            else if (i != 2 || !(s->mode_vol & 0x80))   /* 3OFF */
                vnf += o;
        }
        /* two-integrator loop: high-pass feeds band-pass feeds low-pass */
        s->vbp -= (int32_t)((int64_t)s->w0 * s->vhp >> 20);
        s->vlp -= (int32_t)((int64_t)s->w0 * s->vbp >> 20);
        s->vhp = (int32_t)((int64_t)s->vbp * s->q1024 >> 10) - s->vlp - vi;
        if (!vi && !vnf && s->vlp >= -2 && s->vlp <= 2 && s->vbp >= -2 && s->vbp <= 2
            && s->vhp >= -2 && s->vhp <= 2)
            s->vlp = s->vbp = s->vhp = 0;    /* let rounding settle to rest */
        out = vnf;
        if (s->mode_vol & 0x10) out += s->vlp;
        if (s->mode_vol & 0x20) out += s->vbp;
        if (s->mode_vol & 0x40) out += s->vhp;
        s->acc += (int64_t)out * (s->mode_vol & 15);
        s->acc_n++;
    }
}

uint8_t sid_read(sid *s, uint8_t reg)
{
    switch (reg & 0x1f) {
    case 0x19:
    case 0x1a: return 0xff;              /* no paddles */
    case 0x1b: return (uint8_t)(wave(s, 2) >> 4);
    case 0x1c: return s->v[2].env;
    default: return 0;
    }
}

int32_t sid_take(sid *s)
{
    int64_t v = s->acc_n ? s->acc / (int64_t)s->acc_n : 0;
    s->acc = 0;
    s->acc_n = 0;
    v /= 15 * 12;
    return (int32_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
