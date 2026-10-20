#include "tape.h"
#include "zx.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { B_DATA, B_TONE, B_PULSES, B_DIRECT, B_PAUSE, B_STOP, B_LOOP, B_LOOP_END, B_STOP48 };

typedef struct tblock {
    uint8_t kind;
    uint8_t std;            /* standard ROM timing: the trap may load it */
    uint16_t pilot, sync1, sync2, zero, one;
    uint32_t pilot_count;
    const uint8_t *data;
    uint32_t len;
    uint8_t last_bits;      /* bits used in the final byte (1..8) */
    uint32_t pause_ms;
    uint16_t direct_t;      /* T-states per sample (direct recording) */
} tblock;

enum { P_IDLE, P_PILOT, P_SYNC1, P_SYNC2, P_DATA, P_PAUSE, P_DONE };

struct zx_tape {
    uint8_t *buf;
    tblock *blk;
    int n, cap;
    int cur;
    int phase;
    uint32_t count;         /* pulses left in pilot/tone/pulses phase */
    uint32_t byte, bit;     /* data position */
    int half;               /* second pulse of a data bit */
    int loop_start, loop_left;
    int playing, fast, trap_used;
    uint64_t next_edge;
    uint8_t level;
};

/* ---- parsing ------------------------------------------------------------ */

static tblock *add(zx_tape *t)
{
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->blk = realloc(t->blk, sizeof(tblock) * (size_t)t->cap);
    }
    memset(&t->blk[t->n], 0, sizeof(tblock));
    return &t->blk[t->n++];
}

static void std_block(tblock *b, const uint8_t *data, uint32_t len, uint32_t pause)
{
    b->kind = B_DATA;
    b->std = 1;
    b->pilot = 2168;
    b->sync1 = 667;
    b->sync2 = 735;
    b->zero = 855;
    b->one = 1710;
    b->pilot_count = (len && data[0] < 0x80) ? 8063 : 3223;
    b->data = data;
    b->len = len;
    b->last_bits = 8;
    b->pause_ms = pause;
}

#define U16(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8)
#define U24(p) (U16(p) | (uint32_t)(p)[2] << 16)
#define U32(p) (U24(p) | (uint32_t)(p)[3] << 24)

static int parse_tap(zx_tape *t, const uint8_t *d, size_t len)
{
    size_t i = 0;
    while (i + 2 <= len) {
        uint32_t n = U16(d + i);
        i += 2;
        if (i + n > len)
            return -1;
        std_block(add(t), d + i, n, 1000);
        i += n;
    }
    return 0;
}

static int parse_tzx(zx_tape *t, const uint8_t *d, size_t len)
{
    size_t i = 10;
    tblock *b;
    if (len < 10 || memcmp(d, "ZXTape!\x1a", 8))
        return -1;
    while (i < len) {
        uint8_t id = d[i++];
        const uint8_t *p = d + i;
        size_t left = len - i, skip;
#define NEED(n) do { if (left < (size_t)(n)) return -1; } while (0)
        switch (id) {
        case 0x10:
            NEED(4);
            NEED(4 + U16(p + 2));
            std_block(add(t), p + 4, U16(p + 2), U16(p));
            skip = 4 + U16(p + 2);
            break;
        case 0x11:
            NEED(18);
            NEED(18 + U24(p + 15));
            b = add(t);
            b->kind = B_DATA;
            b->pilot = (uint16_t)U16(p);
            b->sync1 = (uint16_t)U16(p + 2);
            b->sync2 = (uint16_t)U16(p + 4);
            b->zero = (uint16_t)U16(p + 6);
            b->one = (uint16_t)U16(p + 8);
            b->pilot_count = U16(p + 10);
            b->last_bits = p[12];
            b->pause_ms = U16(p + 13);
            b->len = U24(p + 15);
            b->data = p + 18;
            skip = 18 + b->len;
            break;
        case 0x12:
            NEED(4);
            b = add(t);
            b->kind = B_TONE;
            b->pilot = (uint16_t)U16(p);
            b->pilot_count = U16(p + 2);
            skip = 4;
            break;
        case 0x13:
            NEED(1);
            NEED(1 + p[0] * 2);
            b = add(t);
            b->kind = B_PULSES;
            b->data = p + 1;
            b->len = p[0];
            skip = 1 + (size_t)p[0] * 2;
            break;
        case 0x14:
            NEED(10);
            NEED(10 + U24(p + 7));
            b = add(t);
            b->kind = B_DATA;
            b->zero = (uint16_t)U16(p);
            b->one = (uint16_t)U16(p + 2);
            b->last_bits = p[4];
            b->pause_ms = U16(p + 5);
            b->len = U24(p + 7);
            b->data = p + 10;
            skip = 10 + b->len;
            break;
        case 0x15:
            NEED(8);
            NEED(8 + U24(p + 5));
            b = add(t);
            b->kind = B_DIRECT;
            b->direct_t = (uint16_t)U16(p);
            b->pause_ms = U16(p + 2);
            b->last_bits = p[4];
            b->len = U24(p + 5);
            b->data = p + 8;
            skip = 8 + b->len;
            break;
        case 0x18: NEED(4); skip = 4 + U32(p); break;           /* CSW: not played */
        case 0x19: NEED(4); skip = 4 + U32(p); break;           /* generalized: TODO */
        case 0x20:
            NEED(2);
            b = add(t);
            b->kind = U16(p) ? B_PAUSE : B_STOP;
            b->pause_ms = U16(p);
            skip = 2;
            break;
        case 0x21: NEED(1); skip = 1 + p[0]; break;
        case 0x22: skip = 0; break;
        case 0x23: skip = 2; break;
        case 0x24:
            NEED(2);
            b = add(t);
            b->kind = B_LOOP;
            b->pilot_count = U16(p);
            skip = 2;
            break;
        case 0x25: add(t)->kind = B_LOOP_END; skip = 0; break;
        case 0x26: NEED(2); skip = 2 + U16(p) * 2; break;
        case 0x27: skip = 0; break;
        case 0x28: NEED(2); skip = 2 + U16(p); break;
        case 0x2a: NEED(4); add(t)->kind = B_STOP48; skip = 4 + U32(p); break;
        case 0x2b: NEED(4); skip = 4 + U32(p); break;
        case 0x30: NEED(1); skip = 1 + p[0]; break;
        case 0x31: NEED(2); skip = 2 + p[1]; break;
        case 0x32: NEED(2); skip = 2 + U16(p); break;
        case 0x33: NEED(1); skip = 1 + (size_t)p[0] * 3; break;
        case 0x35: NEED(20); skip = 20 + U32(p + 16); break;
        case 0x5a: skip = 9; break;
        default: NEED(4); skip = 4 + U32(p); break; /* extension rule */
        }
#undef NEED
        if (skip > left)
            return -1;
        i += skip;
    }
    return 0;
}

zx_tape *zx_tape_open(const uint8_t *data, size_t len, int is_tzx)
{
    zx_tape *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->buf = malloc(len ? len : 1);
    memcpy(t->buf, data, len);
    if ((is_tzx ? parse_tzx(t, t->buf, len) : parse_tap(t, t->buf, len)) || !t->n) {
        zx_tape_free(t);
        return NULL;
    }
    t->fast = 1;
    t->phase = P_IDLE;
    return t;
}

void zx_tape_free(zx_tape *t)
{
    if (!t)
        return;
    free(t->blk);
    free(t->buf);
    free(t);
}

void zx_tape_set_fast(zx_tape *t, int fast) { t->fast = fast; }
int zx_tape_playing(const zx_tape *t) { return t->playing; }
void zx_tape_stop(zx_tape *t) { t->playing = 0; }

void zx_tape_play(zx_tape *t, uint64_t now)
{
    if (t->cur >= t->n)
        return;
    t->playing = 1;
    t->next_edge = now;
}

void zx_tape_custom_loader(zx_tape *t)
{
    if (t->fast && t->trap_used && !t->playing && t->cur < t->n) {
        t->playing = 1;
        t->next_edge = 0; /* catch up immediately */
    }
}

/* ---- pulse generation --------------------------------------------------- */

#define MS_T 3500 /* T-states per millisecond at 3.5 MHz */

static void begin_block(zx_tape *t)
{
    tblock *b;
    for (;;) {
        if (t->cur >= t->n) {
            t->phase = P_DONE;
            t->playing = 0;
            return;
        }
        b = &t->blk[t->cur];
        t->byte = t->bit = 0;
        t->half = 0;
        switch (b->kind) {
        case B_DATA:
            t->count = b->pilot_count;
            t->phase = b->pilot_count ? P_PILOT : (b->sync1 ? P_SYNC1 : P_DATA);
            return;
        case B_TONE:
            t->count = b->pilot_count;
            t->phase = P_PILOT;
            return;
        case B_PULSES:
            t->count = 0;
            t->phase = P_PILOT;
            return;
        case B_DIRECT:
            t->phase = P_DATA;
            return;
        case B_PAUSE:
            t->phase = P_PAUSE;
            return;
        case B_STOP:
        case B_STOP48:
            t->cur++;
            t->phase = P_IDLE;
            t->playing = 0;
            return;
        case B_LOOP:
            t->loop_start = t->cur + 1;
            t->loop_left = (int)b->pilot_count;
            t->cur++;
            break;
        default: /* B_LOOP_END */
            if (--t->loop_left > 0)
                t->cur = t->loop_start;
            else
                t->cur++;
            break;
        }
    }
}

/* Produce the next pulse: returns its length in T-states and sets the level. */
static uint32_t next_pulse(zx_tape *t)
{
    tblock *b;
    uint8_t bitv;
    uint32_t bits_total;

    for (;;) {
        if (t->phase == P_IDLE || t->phase == P_DONE)
            begin_block(t);
        if (t->phase == P_DONE || !t->playing)
            return 0;
        b = &t->blk[t->cur];
        switch (t->phase) {
        case P_PILOT:
            if (b->kind == B_PULSES) {
                if (t->count < b->len) {
                    const uint8_t *pp = b->data + t->count * 2;
                    t->count++;
                    t->level ^= 1;
                    return U16(pp);
                }
            } else if (t->count) {
                t->count--;
                t->level ^= 1;
                return b->pilot;
            }
            if (b->kind != B_DATA) {
                t->cur++;
                t->phase = P_IDLE;
                continue;
            }
            t->phase = b->sync1 ? P_SYNC1 : P_DATA;
            continue;
        case P_SYNC1:
            t->phase = P_SYNC2;
            t->level ^= 1;
            return b->sync1;
        case P_SYNC2:
            t->phase = P_DATA;
            t->level ^= 1;
            return b->sync2;
        case P_DATA:
            bits_total = b->len ? (b->len - 1) * 8 + b->last_bits : 0;
            if (t->byte * 8 + t->bit >= bits_total) {
                t->phase = P_PAUSE;
                continue;
            }
            bitv = (uint8_t)((b->data[t->byte] >> (7 - t->bit)) & 1);
            if (b->kind == B_DIRECT) {
                t->level = bitv;
                if (++t->bit == 8) { t->bit = 0; t->byte++; }
                return b->direct_t;
            }
            t->level ^= 1;
            if (t->half) {
                t->half = 0;
                if (++t->bit == 8) { t->bit = 0; t->byte++; }
            } else {
                t->half = 1;
            }
            return bitv ? b->one : b->zero;
        case P_PAUSE:
            t->cur++;
            t->phase = P_IDLE;
            if (b->pause_ms) {
                t->level = 0;
                return b->pause_ms * MS_T;
            }
            continue;
        default:
            return 0;
        }
    }
}

void zx_tape_advance(zx_tape *t, struct zx *m, uint64_t now)
{
    if (t->next_edge == 0)
        t->next_edge = now;
    while (t->playing && t->next_edge <= now) {
        uint32_t len = next_pulse(t);
        if (!len)
            break;
        t->next_edge += len;
        m->ear_in = t->level;
    }
}

/* ---- ROM trap ------------------------------------------------------------ */

int zx_tape_trap(zx_tape *t, struct zx *m)
{
    tblock *b;
    z80 *c = &m->cpu;
    uint32_t want, avail, n, i;
    uint8_t parity;
    int ok;

    if (!t->fast || t->playing)
        return 0;
    while (t->cur < t->n && t->blk[t->cur].kind != B_DATA) {
        if (t->blk[t->cur].kind == B_STOP || t->blk[t->cur].kind == B_STOP48)
            return 0;
        t->cur++;
    }
    if (t->cur >= t->n)
        return 0;
    b = &t->blk[t->cur];
    if (!b->std || b->len < 1)
        return 0;
    t->cur++;
    t->phase = P_IDLE;
    t->trap_used = 1;

    want = (uint32_t)(c->d << 8 | c->e);
    avail = b->len - 1;          /* flag byte excluded; includes checksum */
    ok = b->data[0] == c->a;
    if (ok && (c->f & Z80_FC)) { /* LOAD (carry clear means VERIFY) */
        n = want < avail ? want : avail;
        for (i = 0; i < n; i++)
            zx_poke(m, (uint16_t)(c->ix + i), b->data[1 + i]);
        parity = 0;
        for (i = 0; i < b->len; i++)
            parity ^= b->data[i];
        ok = want < avail && parity == 0;
        c->ix = (uint16_t)(c->ix + n);
        want -= n;
        c->d = (uint8_t)(want >> 8);
        c->e = (uint8_t)want;
    }
    c->f = ok ? (uint8_t)(c->f | Z80_FC) : (uint8_t)(c->f & ~Z80_FC);
    /* RET */
    c->pc = (uint16_t)(zx_peek(m, c->sp) | zx_peek(m, (uint16_t)(c->sp + 1)) << 8);
    c->sp = (uint16_t)(c->sp + 2);
    return 1;
}

/* ---- state --------------------------------------------------------------- */

/* The play position: everything from `cur` on. The blocks themselves come
 * from the tape file, so a state only loads into the same tape. */
#define TAPE_STATE_AT offsetof(zx_tape, cur)

size_t zx_tape_state_size(void)
{
    return sizeof(int) + sizeof(zx_tape) - TAPE_STATE_AT;
}

void zx_tape_state_save(const zx_tape *t, uint8_t *out)
{
    memcpy(out, &t->n, sizeof(int));
    memcpy(out + sizeof(int), (const uint8_t *)t + TAPE_STATE_AT, sizeof(zx_tape) - TAPE_STATE_AT);
}

int zx_tape_state_load(zx_tape *t, const uint8_t *in)
{
    int n;
    memcpy(&n, in, sizeof n);
    if (n != t->n)
        return -1;
    memcpy((uint8_t *)t + TAPE_STATE_AT, in + sizeof(int), sizeof(zx_tape) - TAPE_STATE_AT);
    return 0;
}
