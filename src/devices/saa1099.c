#include "saa1099.h"

#include <string.h>

/* ---- envelope controllers ------------------------------------------------ */

/* Mode by register bits 1..3: phases, looping. Levels are computed in
 * env_level: 0 zero, 1 maximum, 2/3 decay, 4/5 triangle, 6/7 attack. */
static const uint8_t env_phases[8] = { 1, 1, 1, 1, 2, 2, 1, 1 };
static const uint8_t env_loops[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };

static uint8_t env_shape(uint8_t mode, uint8_t phase, uint8_t pos)
{
    switch (mode) {
    case 0: return 0;
    case 1: return 15;
    case 2: case 3: return phase ? 0 : (uint8_t)(15 - pos);
    case 4: case 5: return phase ? (uint8_t)(15 - pos) : pos;
    default: return phase ? 0 : pos;
    }
}

static void env_levels(saa_env *e)
{
    uint8_t l;
    if (e->ended && !e->looping)
        l = 0;            /* every one-shot envelope ends at zero */
    else
        l = env_shape(e->mode, e->phase, e->pos);
    if (e->res == 2)
        l &= 0x0e;
    e->left = l;
    e->right = e->invert_right ? (uint8_t)((e->res == 2 ? 14 : 15) - l) : l;
}

static void env_load(saa_env *e, uint8_t v)
{
    e->phase = 0;
    e->pos = 0;
    e->mode = (v >> 1) & 7;
    e->invert_right = v & 1;
    e->external = (v >> 5) & 1;
    e->phases = env_phases[e->mode];
    e->looping = env_loops[e->mode];
    e->res = (v & 0x10) ? 2 : 1;
    e->enabled = (v >> 7) & 1;
    e->ended = !e->enabled;
    env_levels(e);
}

static void env_control(saa_env *e, uint8_t v)
{
    int enabled = (v & 0x80) != 0, res;
    if (!enabled && !e->enabled)
        return;
    e->enabled = (uint8_t)enabled;
    if (!enabled) {
        e->ended = 1;
        env_levels(e);
        return;
    }
    /* Resolution changes at once, keeping the position (measured). */
    res = (v & 0x10) ? 2 : 1;
    if (e->res == 1 && res == 2)
        e->pos &= 0x0e;
    else if (e->res == 2 && res == 1)
        e->pos |= 0x01;
    e->res = (uint8_t)res;
    if (e->ended) {
        env_load(e, v);
        e->new_data = 0;
    } else {
        /* The rest waits for the end of the waveform. */
        env_levels(e);
        e->new_data = 1;
        e->next_data = v;
    }
}

static void env_tick(saa_env *e)
{
    int take_new = 0;
    if (!e->enabled) {
        e->ended = 1;
        e->phase = 0;
        e->pos = 0;
        return;
    }
    if (e->ended)
        return;
    e->pos = (uint8_t)(e->pos + e->res);
    if (e->pos >= 16) {
        e->phase++;
        if (e->phase == e->phases) {
            if (!e->looping) {
                e->ended = 1;
            } else {
                e->ended = 0;
                e->phase = 0;
                e->pos = (uint8_t)(e->pos - 16);
            }
            take_new = 1;
        } else {
            e->ended = 0;
            e->pos = (uint8_t)(e->pos - 16);
        }
    } else {
        e->ended = 0;
    }
    if (e->new_data && take_new) {
        e->new_data = 0;
        env_load(e, e->next_data);
    } else {
        env_levels(e);
    }
}

/* ---- generators ---------------------------------------------------------- */

/* Clocks between edges: the tone runs at clock/512 * 2^octave / (511 - n). */
static uint32_t tone_period(const saa_tone *t)
{
    return (uint32_t)(511 - t->offset) << (8 - t->octave);
}

static void tone_latch(saa_tone *t)
{
    if (!t->new_data)
        return;
    t->octave = t->next_octave;
    if (!t->ignore_offset) {
        t->offset = t->next_offset;
        t->new_data = 0;
    }
    t->ignore_offset = 0;
}

static void noise_step(saa_noise *n)
{
    /* 18-bit Galois LFSR, x^18 + x^11 + x^1 (measured on a SAA1099P). */
    n->lfsr = (n->lfsr & 1) ? (n->lfsr >> 1) ^ 0x20400 : n->lfsr >> 1;
}

static uint32_t noise_period(const saa_noise *n)
{
    return 256u << n->source;
}

/* ---- registers ----------------------------------------------------------- */

void saa_reset(saa1099 *s)
{
    int i;
    memset(s, 0, sizeof *s);
    for (i = 0; i < 6; i++) {
        s->tone[i].level = 1;
        s->tone[i].count = tone_period(&s->tone[i]);
    }
    for (i = 0; i < 2; i++) {
        s->noise[i].lfsr = 0x3ffff;
        s->noise[i].count = noise_period(&s->noise[i]);
        s->env[i].res = 1;
        s->env[i].ended = 1;
        env_load(&s->env[i], 0);
    }
}

void saa_write_address(saa1099 *s, uint8_t reg)
{
    s->addr = reg & 31;
    /* Writing the address of an envelope register clocks an envelope set
     * to the external clock. */
    if (s->addr == 24 || s->addr == 25) {
        saa_env *e = &s->env[s->addr - 24];
        if (e->external && e->enabled)
            env_tick(e);
    }
}

static void set_offset(saa1099 *s, int i, uint8_t v)
{
    saa_tone *t = &s->tone[i];
    if (s->sync) {
        t->new_data = 0;
        t->ignore_offset = 0;
        t->offset = t->next_offset = v;
        t->octave = t->next_octave;
        return;
    }
    t->next_offset = v;
    t->new_data = 1;
    if (t->next_octave == t->octave)
        t->ignore_offset = 1;
}

static void set_octave(saa1099 *s, int i, uint8_t v)
{
    saa_tone *t = &s->tone[i];
    if (s->sync) {
        t->new_data = 0;
        t->ignore_offset = 0;
        t->octave = t->next_octave = v;
        t->offset = t->next_offset;
        return;
    }
    t->next_octave = v;
    t->new_data = 1;
    t->ignore_offset = 0;
}

void saa_write_data(saa1099 *s, uint8_t v)
{
    int r = s->addr, i;
    s->reg[r] = v;
    switch (r) {
    case 0: case 1: case 2: case 3: case 4: case 5:
        s->amp[r] = v;
        break;
    case 8: case 9: case 10: case 11: case 12: case 13:
        set_offset(s, r - 8, v);
        break;
    case 16: case 17: case 18:
        set_octave(s, (r - 16) * 2, v & 7);
        set_octave(s, (r - 16) * 2 + 1, (v >> 4) & 7);
        break;
    case 20:
        s->tone_mix = v & 0x3f;
        break;
    case 21:
        s->noise_mix = v & 0x3f;
        break;
    case 22:
        s->noise[0].source = v & 3;
        s->noise[1].source = (v >> 4) & 3;
        break;
    case 24: case 25:
        env_control(&s->env[r - 24], v);
        break;
    case 28:
        if (((v >> 1) & 1) != s->sync) {
            s->sync = (v >> 1) & 1;
            if (s->sync) {
                /* Everything is held at its start until sync is released. */
                for (i = 0; i < 6; i++) {
                    saa_tone *t = &s->tone[i];
                    t->level = 1;
                    t->octave = t->next_octave;
                    t->offset = t->next_offset;
                    t->new_data = t->ignore_offset = 0;
                }
            } else {
                for (i = 0; i < 6; i++)
                    s->tone[i].count = tone_period(&s->tone[i]);
                for (i = 0; i < 2; i++)
                    s->noise[i].count = noise_period(&s->noise[i]);
            }
        }
        s->enabled = v & 1;
        break;
    default:
        break;
    }
}

/* ---- output -------------------------------------------------------------- */

/* Envelope amplitude by (amplitude / 2, envelope level), measured. */
static const uint8_t pdm[8][16] = {
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 23 },
    { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30 },
    { 0, 3, 5, 8, 10, 13, 15, 18, 20, 23, 25, 28, 30, 33, 35, 38 },
    { 0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45 },
    { 0, 4, 7, 11, 14, 18, 21, 25, 28, 32, 35, 39, 42, 46, 49, 53 },
};

static void output(const saa1099 *s, int32_t *l, int32_t *r)
{
    int ch;
    *l = *r = 0;
    if (!s->enabled || s->sync)
        return;
    for (ch = 0; ch < 6; ch++) {
        int mix = ((s->tone_mix >> ch) & 1) | (((s->noise_mix >> ch) & 1) << 1);
        int tl = s->tone[ch].level, nl = (int)(s->noise[ch / 3].lfsr & 1), v;
        uint8_t a = s->amp[ch];
        const saa_env *e = &s->env[ch / 3];
        switch (mix) {
        case 1: v = tl * 2; break;
        case 2: v = nl * 2; break;
        case 3: v = tl * (2 - nl); break;
        default: v = 0; break;
        }
        if ((ch == 2 || ch == 5) && e->enabled) {
            *l += pdm[(a & 15) >> 1][e->left] * 4 * (2 - v);
            *r += pdm[a >> 5][e->right] * 4 * (2 - v);
        } else {
            *l += (a & 15) * v * 16;
            *r += (a >> 4) * v * 16;
        }
    }
}

static void tone_edge(saa1099 *s, int i)
{
    saa_tone *t = &s->tone[i];
    t->level ^= 1;
    if ((i == 0 || i == 3) && s->noise[i / 3].source == 3)
        noise_step(&s->noise[i / 3]);
    if ((i == 1 || i == 4) && !s->env[i / 3].external)
        env_tick(&s->env[i / 3]);
    tone_latch(t);
    t->count = tone_period(t);
}

void saa_run(saa1099 *s, uint32_t clocks)
{
    int32_t l, r;
    int i;
    if (s->sync) {
        s->acc_n += clocks;
        return;
    }
    output(s, &l, &r);
    while (clocks) {
        uint32_t step = clocks;
        for (i = 0; i < 6; i++)
            if (s->tone[i].count < step)
                step = s->tone[i].count;
        for (i = 0; i < 2; i++)
            if (s->noise[i].source != 3 && s->noise[i].count < step)
                step = s->noise[i].count;
        s->acc_l += (int64_t)l * step;
        s->acc_r += (int64_t)r * step;
        s->acc_n += step;
        clocks -= step;
        for (i = 0; i < 6; i++)
            s->tone[i].count -= step;
        for (i = 0; i < 2; i++)
            if (s->noise[i].source != 3)
                s->noise[i].count -= step;
        for (i = 0; i < 6; i++)
            if (!s->tone[i].count)
                tone_edge(s, i);
        for (i = 0; i < 2; i++) {
            if (s->noise[i].source != 3 && !s->noise[i].count) {
                noise_step(&s->noise[i]);
                s->noise[i].count = noise_period(&s->noise[i]);
            }
        }
        output(s, &l, &r);
    }
}

void saa_take(saa1099 *s, int32_t *left, int32_t *right)
{
    if (s->acc_n) {
        *left = (int32_t)(s->acc_l / s->acc_n);
        *right = (int32_t)(s->acc_r / s->acc_n);
    } else {
        output(s, left, right);
    }
    s->acc_l = s->acc_r = 0;
    s->acc_n = 0;
}
