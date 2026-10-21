#include "m6502.h"

#include <string.h>

/* ---- the bus, one cycle per access --------------------------------------- */

static void sample(m6502 *c)
{
    if (c->nmi_line && !c->nmi_prev)
        c->nmi_edge = 1;
    c->nmi_prev = c->nmi_line;
    c->poll_prev = c->poll;
    c->poll = (uint8_t)((c->irq_line && !(c->p & M6502_FI)) | c->nmi_edge << 1);
}

static uint8_t rd(m6502 *c, const m6502_bus *b, uint16_t addr)
{
    uint8_t v = b->read(b->ctx, addr);
    sample(c);
    return v;
}

static void wr(m6502 *c, const m6502_bus *b, uint16_t addr, uint8_t v)
{
    b->write(b->ctx, addr, v);
    sample(c);
}

#define RD(a) rd(c, b, (uint16_t)(a))
#define WR(a, v) wr(c, b, (uint16_t)(a), (uint8_t)(v))
#define FETCH() RD(c->pc++)
#define PUSH(v) WR(0x100 | c->s--, v)
#define PULL() RD(0x100 | ++c->s)

/* ---- ALU ------------------------------------------------------------------ */

static void nz(m6502 *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(M6502_FN | M6502_FZ)) | (v & 0x80) | (v ? 0 : M6502_FZ));
}

static void setc(m6502 *c, int on)
{
    c->p = (uint8_t)(on ? c->p | M6502_FC : c->p & ~M6502_FC);
}

static void setv(m6502 *c, int on)
{
    c->p = (uint8_t)(on ? c->p | M6502_FV : c->p & ~M6502_FV);
}

static void adc(m6502 *c, uint8_t v)
{
    unsigned a = c->a, cy = c->p & M6502_FC;
    unsigned bin = a + v + cy;
    if (c->p & M6502_FD) {
        /* NMOS decimal: Z from the binary sum, N and V from the high
         * nibble before its correction. */
        unsigned lo = (a & 0x0f) + (v & 0x0f) + cy, hi;
        if (lo > 9)
            lo += 6;
        hi = (a >> 4) + (v >> 4) + (lo > 0x0f);
        c->p = (uint8_t)(c->p & ~(M6502_FN | M6502_FZ));
        if (!(bin & 0xff))
            c->p |= M6502_FZ;
        c->p |= (uint8_t)((hi << 4) & 0x80);
        setv(c, ~(a ^ v) & (a ^ (hi << 4)) & 0x80);
        if (hi > 9)
            hi += 6;
        setc(c, hi > 0x0f);
        c->a = (uint8_t)(hi << 4 | (lo & 0x0f));
    } else {
        setv(c, ~(a ^ v) & (a ^ bin) & 0x80);
        setc(c, bin > 0xff);
        c->a = (uint8_t)bin;
        nz(c, c->a);
    }
}

static void sbc(m6502 *c, uint8_t v)
{
    unsigned a = c->a, borrow = !(c->p & M6502_FC);
    unsigned bin = a - v - borrow;
    /* NMOS decimal: all flags from the binary difference. */
    setv(c, (a ^ v) & (a ^ bin) & 0x80);
    setc(c, bin < 0x100);
    nz(c, (uint8_t)bin);
    if (c->p & M6502_FD) {
        unsigned lo = (a & 0x0f) - (v & 0x0f) - borrow;
        unsigned hi = (a >> 4) - (v >> 4);
        if (lo & 0x10) {
            lo -= 6;
            hi--;
        }
        if (hi & 0x10)
            hi -= 6;
        c->a = (uint8_t)(hi << 4 | (lo & 0x0f));
    } else {
        c->a = (uint8_t)bin;
    }
}

static void cmp(m6502 *c, uint8_t r, uint8_t v)
{
    setc(c, r >= v);
    nz(c, (uint8_t)(r - v));
}

static uint8_t asl(m6502 *c, uint8_t v) { setc(c, v & 0x80); v = (uint8_t)(v << 1); nz(c, v); return v; }
static uint8_t lsr(m6502 *c, uint8_t v) { setc(c, v & 1); v >>= 1; nz(c, v); return v; }
static uint8_t rol(m6502 *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v << 1 | (c->p & M6502_FC));
    setc(c, v & 0x80);
    nz(c, r);
    return r;
}
static uint8_t ror(m6502 *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v >> 1 | (c->p & M6502_FC) << 7);
    setc(c, v & 1);
    nz(c, r);
    return r;
}

static void arr(m6502 *c, uint8_t v)
{
    uint8_t t = c->a & v, cy = c->p & M6502_FC;
    c->a = (uint8_t)(t >> 1 | cy << 7);
    if (c->p & M6502_FD) {
        /* NMOS ARR in decimal mode: N from the old carry, then the BCD
         * fix-ups on each nibble of the AND result. */
        c->p = (uint8_t)((c->p & ~(M6502_FN | M6502_FZ)) | (cy ? M6502_FN : 0) | (c->a ? 0 : M6502_FZ));
        setv(c, (t ^ c->a) & 0x40);
        if ((t & 0x0f) + (t & 1) > 5)
            c->a = (uint8_t)((c->a & 0xf0) | ((c->a + 6) & 0x0f));
        if ((t >> 4) + (t >> 4 & 1) > 5) {
            setc(c, 1);
            c->a = (uint8_t)(c->a + 0x60);
        } else {
            setc(c, 0);
        }
    } else {
        nz(c, c->a);
        setc(c, c->a & 0x40);
        setv(c, ((c->a >> 6) ^ (c->a >> 5)) & 1);
    }
}

/* ---- interrupts ------------------------------------------------------------ */

/* BRK, IRQ and NMI share a sequence; an NMI arriving before the vector is
 * read takes the vector over. */
static void interrupt(m6502 *c, const m6502_bus *b, int brk)
{
    uint16_t vec;
    RD(c->pc);
    if (brk)
        c->pc++;
    else
        RD(c->pc);
    PUSH(c->pc >> 8);
    PUSH(c->pc & 0xff);
    if (c->nmi_edge) {
        c->nmi_edge = 0;
        vec = 0xfffa;
    } else {
        vec = 0xfffe;
    }
    PUSH(c->p | M6502_FU | (brk ? M6502_FB : 0));
    c->p |= M6502_FI;
    c->pc = RD(vec);
    c->pc |= (uint16_t)(RD(vec + 1) << 8);
    /* The vector's first instruction always runs. */
    c->poll &= 2;
    c->poll_prev = c->nmi_edge << 1;
}

void m6502_init(m6502 *c)
{
    memset(c, 0, sizeof *c);
    c->s = 0xfd;
    c->p = M6502_FU | M6502_FI;
    c->reset_pending = 1;
}

void m6502_reset(m6502 *c)
{
    c->reset_pending = 1;
    c->jammed = 0;
}

static void do_reset(m6502 *c, const m6502_bus *b)
{
    /* Three pushes with writes suppressed: the stack pointer moves, the
     * bus sees reads. */
    RD(c->pc);
    RD(c->pc);
    RD(0x100 | c->s--);
    RD(0x100 | c->s--);
    RD(0x100 | c->s--);
    c->p |= M6502_FI;
    c->pc = RD(0xfffc);
    c->pc |= (uint16_t)(RD(0xfffd) << 8);
    c->reset_pending = 0;
    c->nmi_edge = 0;
    c->poll = c->poll_prev = 0;
}

/* ---- addressing ----------------------------------------------------------- */

/* Effective addresses. `wrmode` makes indexed modes always take the fix-up
 * cycle (stores and read-modify-write); reads take it on a page cross. The
 * dummy read is at the address before the high byte is fixed. */

static uint16_t ea_zp(m6502 *c, const m6502_bus *b) { return FETCH(); }

static uint16_t ea_zpi(m6502 *c, const m6502_bus *b, uint8_t i)
{
    uint8_t z = FETCH();
    RD(z);
    return (uint8_t)(z + i);
}

static uint16_t ea_abs(m6502 *c, const m6502_bus *b)
{
    uint16_t a = FETCH();
    return (uint16_t)(a | FETCH() << 8);
}

static uint16_t ea_absi(m6502 *c, const m6502_bus *b, uint8_t i, int wrmode)
{
    uint16_t base = ea_abs(c, b), a = (uint16_t)(base + i);
    if (wrmode || (a ^ base) & 0xff00)
        RD((base & 0xff00) | (a & 0xff));
    return a;
}

static uint16_t ea_izx(m6502 *c, const m6502_bus *b)
{
    uint8_t z = FETCH();
    uint16_t a;
    RD(z);
    z = (uint8_t)(z + c->x);
    a = RD(z);
    return (uint16_t)(a | RD((uint8_t)(z + 1)) << 8);
}

static uint16_t ea_izy(m6502 *c, const m6502_bus *b, int wrmode)
{
    uint8_t z = FETCH();
    uint16_t base = RD(z), a;
    base |= (uint16_t)(RD((uint8_t)(z + 1)) << 8);
    a = (uint16_t)(base + c->y);
    if (wrmode || (a ^ base) & 0xff00)
        RD((base & 0xff00) | (a & 0xff));
    return a;
}

/* SHA, SHX, SHY, TAS: store v & (base high byte + 1); on a page cross
 * the stored value also replaces the address's high byte. */
static void store_hi(m6502 *c, const m6502_bus *b, uint16_t base, uint8_t i, uint8_t v)
{
    uint16_t a = (uint16_t)(base + i);
    RD((base & 0xff00) | (a & 0xff));
    v &= (uint8_t)((base >> 8) + 1);
    if ((a ^ base) & 0xff00)
        a = (uint16_t)((a & 0xff) | v << 8);
    WR(a, v);
}

static void branch(m6502 *c, const m6502_bus *b, int take, uint8_t poll1)
{
    int8_t off = (int8_t)FETCH();
    uint16_t to;
    if (!take)
        return;
    RD(c->pc);
    to = (uint16_t)(c->pc + off);
    if ((to ^ c->pc) & 0xff00) {
        RD((c->pc & 0xff00) | (to & 0xff));
    } else {
        /* No interrupt poll on this extra cycle. */
        c->poll_prev = poll1 | c->nmi_edge << 1;
    }
    c->pc = to;
}

/* ---- instructions --------------------------------------------------------- */

void m6502_step(m6502 *c, const m6502_bus *b)
{
    uint8_t op, v, poll1;
    uint16_t a;

    if (c->reset_pending) {
        do_reset(c, b);
        return;
    }
    if (c->jammed) {
        /* The address bus as measured on a jammed NMOS part. */
        RD(c->jammed == 2 || c->jammed == 3 ? 0xfffe : 0xffff);
        if (c->jammed < 4)
            c->jammed++;
        return;
    }
    if (c->poll_prev) {
        interrupt(c, b, 0);
        return;
    }

    op = FETCH();
    poll1 = c->poll & 1;

/* Read, store and read-modify-write forms over an address expression. */
#define R(ea, body) do { a = (ea); v = RD(a); body; } while (0)
#define W(ea, val) do { a = (ea); WR(a, val); } while (0)
#define M(ea, body) do { a = (ea); v = RD(a); WR(a, v); body; WR(a, v); } while (0)
#define IMPL() RD(c->pc)

    switch (op) {
    /* loads */
    case 0xa9: v = FETCH(); c->a = v; nz(c, v); break;
    case 0xa5: R(ea_zp(c, b), c->a = v; nz(c, v)); break;
    case 0xb5: R(ea_zpi(c, b, c->x), c->a = v; nz(c, v)); break;
    case 0xad: R(ea_abs(c, b), c->a = v; nz(c, v)); break;
    case 0xbd: R(ea_absi(c, b, c->x, 0), c->a = v; nz(c, v)); break;
    case 0xb9: R(ea_absi(c, b, c->y, 0), c->a = v; nz(c, v)); break;
    case 0xa1: R(ea_izx(c, b), c->a = v; nz(c, v)); break;
    case 0xb1: R(ea_izy(c, b, 0), c->a = v; nz(c, v)); break;
    case 0xa2: v = FETCH(); c->x = v; nz(c, v); break;
    case 0xa6: R(ea_zp(c, b), c->x = v; nz(c, v)); break;
    case 0xb6: R(ea_zpi(c, b, c->y), c->x = v; nz(c, v)); break;
    case 0xae: R(ea_abs(c, b), c->x = v; nz(c, v)); break;
    case 0xbe: R(ea_absi(c, b, c->y, 0), c->x = v; nz(c, v)); break;
    case 0xa0: v = FETCH(); c->y = v; nz(c, v); break;
    case 0xa4: R(ea_zp(c, b), c->y = v; nz(c, v)); break;
    case 0xb4: R(ea_zpi(c, b, c->x), c->y = v; nz(c, v)); break;
    case 0xac: R(ea_abs(c, b), c->y = v; nz(c, v)); break;
    case 0xbc: R(ea_absi(c, b, c->x, 0), c->y = v; nz(c, v)); break;

    /* stores */
    case 0x85: W(ea_zp(c, b), c->a); break;
    case 0x95: W(ea_zpi(c, b, c->x), c->a); break;
    case 0x8d: W(ea_abs(c, b), c->a); break;
    case 0x9d: W(ea_absi(c, b, c->x, 1), c->a); break;
    case 0x99: W(ea_absi(c, b, c->y, 1), c->a); break;
    case 0x81: W(ea_izx(c, b), c->a); break;
    case 0x91: W(ea_izy(c, b, 1), c->a); break;
    case 0x86: W(ea_zp(c, b), c->x); break;
    case 0x96: W(ea_zpi(c, b, c->y), c->x); break;
    case 0x8e: W(ea_abs(c, b), c->x); break;
    case 0x84: W(ea_zp(c, b), c->y); break;
    case 0x94: W(ea_zpi(c, b, c->x), c->y); break;
    case 0x8c: W(ea_abs(c, b), c->y); break;

    /* logic and arithmetic */
#define ALU(o, body) \
    case o + 0x09: v = FETCH(); body; break; \
    case o + 0x05: R(ea_zp(c, b), body); break; \
    case o + 0x15: R(ea_zpi(c, b, c->x), body); break; \
    case o + 0x0d: R(ea_abs(c, b), body); break; \
    case o + 0x1d: R(ea_absi(c, b, c->x, 0), body); break; \
    case o + 0x19: R(ea_absi(c, b, c->y, 0), body); break; \
    case o + 0x01: R(ea_izx(c, b), body); break; \
    case o + 0x11: R(ea_izy(c, b, 0), body); break;
    ALU(0x00, c->a |= v; nz(c, c->a))
    ALU(0x20, c->a &= v; nz(c, c->a))
    ALU(0x40, c->a ^= v; nz(c, c->a))
    ALU(0x60, adc(c, v))
    ALU(0xc0, cmp(c, c->a, v))
    ALU(0xe0, sbc(c, v))
    case 0xeb: v = FETCH(); sbc(c, v); break;
    case 0xe0: v = FETCH(); cmp(c, c->x, v); break;
    case 0xe4: R(ea_zp(c, b), cmp(c, c->x, v)); break;
    case 0xec: R(ea_abs(c, b), cmp(c, c->x, v)); break;
    case 0xc0: v = FETCH(); cmp(c, c->y, v); break;
    case 0xc4: R(ea_zp(c, b), cmp(c, c->y, v)); break;
    case 0xcc: R(ea_abs(c, b), cmp(c, c->y, v)); break;
    case 0x24: case 0x2c:
        R(op == 0x24 ? ea_zp(c, b) : ea_abs(c, b),
          c->p = (uint8_t)((c->p & ~(M6502_FN | M6502_FV | M6502_FZ)) | (v & 0xc0) | ((c->a & v) ? 0 : M6502_FZ)));
        break;

    /* shifts and increments */
#define RMW(o, body) \
    case o + 0x06: M(ea_zp(c, b), body); break; \
    case o + 0x16: M(ea_zpi(c, b, c->x), body); break; \
    case o + 0x0e: M(ea_abs(c, b), body); break; \
    case o + 0x1e: M(ea_absi(c, b, c->x, 1), body); break;
    RMW(0x00, v = asl(c, v))
    RMW(0x20, v = rol(c, v))
    RMW(0x40, v = lsr(c, v))
    RMW(0x60, v = ror(c, v))
    RMW(0xc0, v--; nz(c, v))
    RMW(0xe0, v++; nz(c, v))
    case 0x0a: IMPL(); c->a = asl(c, c->a); break;
    case 0x2a: IMPL(); c->a = rol(c, c->a); break;
    case 0x4a: IMPL(); c->a = lsr(c, c->a); break;
    case 0x6a: IMPL(); c->a = ror(c, c->a); break;
    case 0xca: IMPL(); c->x--; nz(c, c->x); break;
    case 0x88: IMPL(); c->y--; nz(c, c->y); break;
    case 0xe8: IMPL(); c->x++; nz(c, c->x); break;
    case 0xc8: IMPL(); c->y++; nz(c, c->y); break;

    /* the undocumented read-modify-write group */
#define URMW(o, body) \
    case o + 0x07: M(ea_zp(c, b), body); break; \
    case o + 0x17: M(ea_zpi(c, b, c->x), body); break; \
    case o + 0x0f: M(ea_abs(c, b), body); break; \
    case o + 0x1f: M(ea_absi(c, b, c->x, 1), body); break; \
    case o + 0x1b: M(ea_absi(c, b, c->y, 1), body); break; \
    case o + 0x03: M(ea_izx(c, b), body); break; \
    case o + 0x13: M(ea_izy(c, b, 1), body); break;
    URMW(0x00, v = asl(c, v); c->a |= v; nz(c, c->a))          /* SLO */
    URMW(0x20, v = rol(c, v); c->a &= v; nz(c, c->a))          /* RLA */
    URMW(0x40, v = lsr(c, v); c->a ^= v; nz(c, c->a))          /* SRE */
    URMW(0x60, v = ror(c, v); adc(c, v))                       /* RRA */
    URMW(0xc0, v--; cmp(c, c->a, v))                           /* DCP */
    URMW(0xe0, v++; sbc(c, v))                                 /* ISC */

    /* other undocumented opcodes */
    case 0x87: W(ea_zp(c, b), c->a & c->x); break;             /* SAX */
    case 0x97: W(ea_zpi(c, b, c->y), c->a & c->x); break;
    case 0x8f: W(ea_abs(c, b), c->a & c->x); break;
    case 0x83: W(ea_izx(c, b), c->a & c->x); break;
    case 0xa7: R(ea_zp(c, b), c->a = c->x = v; nz(c, v)); break;   /* LAX */
    case 0xb7: R(ea_zpi(c, b, c->y), c->a = c->x = v; nz(c, v)); break;
    case 0xaf: R(ea_abs(c, b), c->a = c->x = v; nz(c, v)); break;
    case 0xbf: R(ea_absi(c, b, c->y, 0), c->a = c->x = v; nz(c, v)); break;
    case 0xa3: R(ea_izx(c, b), c->a = c->x = v; nz(c, v)); break;
    case 0xb3: R(ea_izy(c, b, 0), c->a = c->x = v; nz(c, v)); break;
    case 0x0b: case 0x2b:                                      /* ANC */
        v = FETCH(); c->a &= v; nz(c, c->a); setc(c, c->a & 0x80); break;
    case 0x4b: v = FETCH(); c->a = lsr(c, c->a & v); break;    /* ALR */
    case 0x6b: v = FETCH(); arr(c, v); break;
    case 0x8b: v = FETCH(); c->a = (uint8_t)((c->a | 0xee) & c->x & v); nz(c, c->a); break;  /* ANE */
    case 0xab: v = FETCH(); c->a = c->x = (uint8_t)((c->a | 0xee) & v); nz(c, c->a); break;  /* LXA */
    case 0xcb:                                                 /* SBX */
        v = FETCH();
        a = (uint16_t)((c->a & c->x) - v);
        setc(c, a < 0x100);
        c->x = (uint8_t)a;
        nz(c, c->x);
        break;
    case 0xbb: R(ea_absi(c, b, c->y, 0), c->a = c->x = c->s = v & c->s; nz(c, c->a)); break;  /* LAS */
    case 0x93: {                                               /* SHA (zp),Y */
        uint8_t z = FETCH();
        a = RD(z);
        a |= (uint16_t)(RD((uint8_t)(z + 1)) << 8);
        store_hi(c, b, a, c->y, c->a & c->x);
        break;
    }
    case 0x9f: a = ea_abs(c, b); store_hi(c, b, a, c->y, c->a & c->x); break;  /* SHA abs,Y */
    case 0x9e: a = ea_abs(c, b); store_hi(c, b, a, c->y, c->x); break;         /* SHX */
    case 0x9c: a = ea_abs(c, b); store_hi(c, b, a, c->x, c->y); break;         /* SHY */
    case 0x9b: a = ea_abs(c, b); c->s = c->a & c->x; store_hi(c, b, a, c->y, c->s); break;  /* TAS */

    /* NOPs, documented and not */
    case 0xea: case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
        IMPL(); break;
    case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2:
        FETCH(); break;
    case 0x04: case 0x44: case 0x64: R(ea_zp(c, b), (void)v); break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4:
        R(ea_zpi(c, b, c->x), (void)v); break;
    case 0x0c: R(ea_abs(c, b), (void)v); break;
    case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc:
        R(ea_absi(c, b, c->x, 0), (void)v); break;
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
    case 0x62: case 0x72: case 0x92: case 0xb2: case 0xd2: case 0xf2:
        RD(c->pc);
        c->jammed = 1;
        break;

    /* transfers and flags */
    case 0xaa: IMPL(); c->x = c->a; nz(c, c->x); break;
    case 0x8a: IMPL(); c->a = c->x; nz(c, c->a); break;
    case 0xa8: IMPL(); c->y = c->a; nz(c, c->y); break;
    case 0x98: IMPL(); c->a = c->y; nz(c, c->a); break;
    case 0xba: IMPL(); c->x = c->s; nz(c, c->x); break;
    case 0x9a: IMPL(); c->s = c->x; break;
    case 0x18: IMPL(); c->p &= (uint8_t)~M6502_FC; break;
    case 0x38: IMPL(); c->p |= M6502_FC; break;
    case 0x58: IMPL(); c->p &= (uint8_t)~M6502_FI; break;
    case 0x78: IMPL(); c->p |= M6502_FI; break;
    case 0xb8: IMPL(); c->p &= (uint8_t)~M6502_FV; break;
    case 0xd8: IMPL(); c->p &= (uint8_t)~M6502_FD; break;
    case 0xf8: IMPL(); c->p |= M6502_FD; break;

    /* stack */
    case 0x48: IMPL(); PUSH(c->a); break;
    case 0x08: IMPL(); PUSH(c->p | M6502_FB | M6502_FU); break;
    case 0x68: IMPL(); RD(0x100 | c->s); c->a = PULL(); nz(c, c->a); break;
    case 0x28:
        IMPL();
        RD(0x100 | c->s);
        v = PULL();
        c->p = (uint8_t)((v & ~M6502_FB) | M6502_FU);
        break;

    /* jumps and calls */
    case 0x4c: c->pc = ea_abs(c, b); break;
    case 0x6c: {
        uint16_t p = ea_abs(c, b);
        a = RD(p);
        /* The pointer's high byte comes from the same page. */
        a |= (uint16_t)(RD((p & 0xff00) | ((p + 1) & 0xff)) << 8);
        c->pc = a;
        break;
    }
    case 0x20:
        a = FETCH();
        RD(0x100 | c->s);
        PUSH(c->pc >> 8);
        PUSH(c->pc & 0xff);
        a |= (uint16_t)(RD(c->pc) << 8);
        c->pc = a;
        break;
    case 0x60:
        IMPL();
        RD(0x100 | c->s);
        a = PULL();
        a |= (uint16_t)(PULL() << 8);
        c->pc = a;
        RD(c->pc++);
        break;
    case 0x40:
        IMPL();
        RD(0x100 | c->s);
        v = PULL();
        c->p = (uint8_t)((v & ~M6502_FB) | M6502_FU);
        a = PULL();
        a |= (uint16_t)(PULL() << 8);
        c->pc = a;
        break;
    case 0x00:
        interrupt(c, b, 1);
        return;

    /* branches */
    case 0x10: branch(c, b, !(c->p & M6502_FN), poll1); break;
    case 0x30: branch(c, b, c->p & M6502_FN, poll1); break;
    case 0x50: branch(c, b, !(c->p & M6502_FV), poll1); break;
    case 0x70: branch(c, b, c->p & M6502_FV, poll1); break;
    case 0x90: branch(c, b, !(c->p & M6502_FC), poll1); break;
    case 0xb0: branch(c, b, c->p & M6502_FC, poll1); break;
    case 0xd0: branch(c, b, !(c->p & M6502_FZ), poll1); break;
    case 0xf0: branch(c, b, c->p & M6502_FZ, poll1); break;
    }
#undef R
#undef W
#undef M
#undef IMPL
}
