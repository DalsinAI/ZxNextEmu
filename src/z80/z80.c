/*
 * Z80 / Z80N interpreter. Machine-cycle breakdown follows Fuse, so a
 * contended-memory bus sees exactly the accesses the real CPU makes.
 */
#include "z80.h"

#include <string.h>

enum { IDX_HL, IDX_IX, IDX_IY };

static uint8_t sz53[256], parity[256], sz53p[256];
static int tables_ready;

static void build_tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        int bits = 0, j;
        for (j = 0; j < 8; j++)
            bits += (i >> j) & 1;
        sz53[i] = (uint8_t)(i & (Z80_FS | Z80_FY | Z80_FX));
        if (i == 0)
            sz53[i] |= Z80_FZ;
        parity[i] = (bits & 1) ? 0 : Z80_FP;
        sz53p[i] = sz53[i] | parity[i];
    }
    tables_ready = 1;
}

void z80_reset(z80 *cpu)
{
    cpu->pc = 0;
    cpu->i = cpu->r = 0;
    cpu->iff1 = cpu->iff2 = 0;
    cpu->im = 0;
    cpu->halted = 0;
    cpu->ei_delay = 0;
    cpu->sp = 0xffff;
    cpu->a = cpu->f = 0xff;
    cpu->memptr = 0;
    cpu->q = cpu->prev_q = 0;
}

void z80_init(z80 *cpu, int z80n)
{
    if (!tables_ready)
        build_tables();
    memset(cpu, 0, sizeof *cpu);
    z80_reset(cpu);
    cpu->z80n = (uint8_t)(z80n != 0);
}

/* ---- register helpers ------------------------------------------------- */

#define AF(z) ((uint16_t)((z)->a << 8 | (z)->f))
#define BC(z) ((uint16_t)((z)->b << 8 | (z)->c))
#define DE(z) ((uint16_t)((z)->d << 8 | (z)->e))
#define HL(z) ((uint16_t)((z)->h << 8 | (z)->l))
#define IR(z) ((uint16_t)((z)->i << 8 | (z)->r))

static inline void set_af(z80 *z, uint16_t v) { z->a = (uint8_t)(v >> 8); z->f = (uint8_t)v; }
static inline void set_bc(z80 *z, uint16_t v) { z->b = (uint8_t)(v >> 8); z->c = (uint8_t)v; }
static inline void set_de(z80 *z, uint16_t v) { z->d = (uint8_t)(v >> 8); z->e = (uint8_t)v; }
static inline void set_hl(z80 *z, uint16_t v) { z->h = (uint8_t)(v >> 8); z->l = (uint8_t)v; }

static inline void setf(z80 *z, uint8_t f) { z->f = f; z->q = f; }
static inline void inc_r(z80 *z) { z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7f)); }

static inline uint16_t get_idx(const z80 *z, int idx)
{
    return idx == IDX_HL ? HL(z) : idx == IDX_IX ? z->ix : z->iy;
}

static inline void set_idx(z80 *z, int idx, uint16_t v)
{
    if (idx == IDX_HL)
        set_hl(z, v);
    else if (idx == IDX_IX)
        z->ix = v;
    else
        z->iy = v;
}

/* 8-bit register by opcode index; 6 ((HL)) is handled by the caller. */
static uint8_t reg8(const z80 *z, int r, int idx)
{
    switch (r) {
    case 0: return z->b;
    case 1: return z->c;
    case 2: return z->d;
    case 3: return z->e;
    case 4: return (uint8_t)(get_idx(z, idx) >> 8);
    case 5: return (uint8_t)get_idx(z, idx);
    default: return z->a;
    }
}

static void set_reg8(z80 *z, int r, int idx, uint8_t v)
{
    uint16_t x;
    switch (r) {
    case 0: z->b = v; break;
    case 1: z->c = v; break;
    case 2: z->d = v; break;
    case 3: z->e = v; break;
    case 4: x = get_idx(z, idx); set_idx(z, idx, (uint16_t)((x & 0x00ff) | v << 8)); break;
    case 5: x = get_idx(z, idx); set_idx(z, idx, (uint16_t)((x & 0xff00) | v)); break;
    default: z->a = v; break;
    }
}

/* rp table: BC, DE, HL/IX/IY, SP */
static uint16_t rp(const z80 *z, int p, int idx)
{
    switch (p) {
    case 0: return BC(z);
    case 1: return DE(z);
    case 2: return get_idx(z, idx);
    default: return z->sp;
    }
}

static void set_rp(z80 *z, int p, int idx, uint16_t v)
{
    switch (p) {
    case 0: set_bc(z, v); break;
    case 1: set_de(z, v); break;
    case 2: set_idx(z, idx, v); break;
    default: z->sp = v; break;
    }
}

static int cond(const z80 *z, int y)
{
    switch (y) {
    case 0: return !(z->f & Z80_FZ);
    case 1: return (z->f & Z80_FZ) != 0;
    case 2: return !(z->f & Z80_FC);
    case 3: return (z->f & Z80_FC) != 0;
    case 4: return !(z->f & Z80_FP);
    case 5: return (z->f & Z80_FP) != 0;
    case 6: return !(z->f & Z80_FS);
    default: return (z->f & Z80_FS) != 0;
    }
}

/* ---- bus helpers ------------------------------------------------------ */

#define RD(a) bus->read(bus->ctx, (uint16_t)(a))
#define WR(a, v) bus->write(bus->ctx, (uint16_t)(a), (uint8_t)(v))
#define INTERNAL(a, n) bus->internal(bus->ctx, (uint16_t)(a), (n))

static inline uint8_t fetch_op(z80 *z, const z80_bus *bus)
{
    uint8_t op = bus->fetch(bus->ctx, z->pc);
    z->pc++;
    inc_r(z);
    return op;
}

static inline uint8_t imm8(z80 *z, const z80_bus *bus)
{
    uint8_t v = RD(z->pc);
    z->pc++;
    return v;
}

static inline uint16_t imm16(z80 *z, const z80_bus *bus)
{
    uint16_t lo = imm8(z, bus);
    uint16_t hi = imm8(z, bus);
    return (uint16_t)(hi << 8 | lo);
}

static inline uint16_t read16(const z80_bus *bus, uint16_t addr)
{
    uint16_t lo = RD(addr);
    uint16_t hi = RD(addr + 1);
    return (uint16_t)(hi << 8 | lo);
}

static inline void write16(const z80_bus *bus, uint16_t addr, uint16_t v)
{
    WR(addr, v);
    WR(addr + 1, v >> 8);
}

static inline void push16(z80 *z, const z80_bus *bus, uint16_t v)
{
    z->sp--;
    WR(z->sp, v >> 8);
    z->sp--;
    WR(z->sp, v);
}

static inline uint16_t pop16(z80 *z, const z80_bus *bus)
{
    uint16_t lo = RD(z->sp);
    uint16_t hi;
    z->sp++;
    hi = RD(z->sp);
    z->sp++;
    return (uint16_t)(hi << 8 | lo);
}

/* Effective address for (HL) / (IX+d) / (IY+d) in the plain opcode table. */
static uint16_t mem_operand(z80 *z, const z80_bus *bus, int idx)
{
    int8_t d;
    uint16_t a;
    if (idx == IDX_HL)
        return HL(z);
    d = (int8_t)RD(z->pc);
    INTERNAL(z->pc, 5);
    z->pc++;
    a = (uint16_t)(get_idx(z, idx) + d);
    z->memptr = a;
    return a;
}

/* ---- ALU -------------------------------------------------------------- */

static void add_a(z80 *z, uint8_t v, int carry)
{
    unsigned a = z->a, r16 = a + v + (unsigned)carry;
    uint8_t r = (uint8_t)r16;
    uint8_t f = sz53[r] | ((a ^ v ^ r) & Z80_FH);
    if (r16 > 0xff)
        f |= Z80_FC;
    if (!((a ^ v) & 0x80) && ((a ^ r) & 0x80))
        f |= Z80_FP;
    z->a = r;
    setf(z, f);
}

static uint8_t sub_flags(const z80 *z, uint8_t v, int carry, uint8_t *res)
{
    unsigned a = z->a, r16 = (a - v - (unsigned)carry) & 0x1ff;
    uint8_t r = (uint8_t)r16;
    uint8_t f = Z80_FN | sz53[r] | ((a ^ v ^ r) & Z80_FH);
    if (r16 > 0xff)
        f |= Z80_FC;
    if (((a ^ v) & 0x80) && ((a ^ r) & 0x80))
        f |= Z80_FP;
    *res = r;
    return f;
}

static void alu(z80 *z, int op, uint8_t v)
{
    uint8_t r, f;
    switch (op) {
    case 0: add_a(z, v, 0); break;
    case 1: add_a(z, v, z->f & Z80_FC); break;
    case 2: f = sub_flags(z, v, 0, &r); z->a = r; setf(z, f); break;
    case 3: f = sub_flags(z, v, z->f & Z80_FC, &r); z->a = r; setf(z, f); break;
    case 4: z->a &= v; setf(z, sz53p[z->a] | Z80_FH); break;
    case 5: z->a ^= v; setf(z, sz53p[z->a]); break;
    case 6: z->a |= v; setf(z, sz53p[z->a]); break;
    default:
        f = sub_flags(z, v, 0, &r);
        setf(z, (uint8_t)((f & ~(Z80_FX | Z80_FY)) | (v & (Z80_FX | Z80_FY))));
        break;
    }
}

static uint8_t inc8(z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    uint8_t f = (z->f & Z80_FC) | sz53[r];
    if (r == 0x80)
        f |= Z80_FP;
    if (!(r & 0x0f))
        f |= Z80_FH;
    setf(z, f);
    return r;
}

static uint8_t dec8(z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    uint8_t f = (z->f & Z80_FC) | Z80_FN | sz53[r];
    if (v == 0x80)
        f |= Z80_FP;
    if (!(v & 0x0f))
        f |= Z80_FH;
    setf(z, f);
    return r;
}

static uint16_t add16(z80 *z, uint16_t a, uint16_t b)
{
    uint32_t r32 = (uint32_t)a + b;
    uint16_t r = (uint16_t)r32;
    uint8_t f = (z->f & (Z80_FS | Z80_FZ | Z80_FP)) | ((r >> 8) & (Z80_FX | Z80_FY));
    f |= ((a ^ b ^ r) >> 8) & Z80_FH;
    if (r32 > 0xffff)
        f |= Z80_FC;
    setf(z, f);
    z->memptr = (uint16_t)(a + 1);
    return r;
}

static void adc16(z80 *z, uint16_t v)
{
    uint16_t hl = HL(z);
    uint32_t r32 = (uint32_t)hl + v + (z->f & Z80_FC);
    uint16_t r = (uint16_t)r32;
    uint8_t f = ((r >> 8) & (Z80_FS | Z80_FX | Z80_FY)) | (((hl ^ v ^ r) >> 8) & Z80_FH);
    if (r32 > 0xffff)
        f |= Z80_FC;
    if (!((hl ^ v) & 0x8000) && ((hl ^ r) & 0x8000))
        f |= Z80_FP;
    if (!r)
        f |= Z80_FZ;
    z->memptr = (uint16_t)(hl + 1);
    set_hl(z, r);
    setf(z, f);
}

static void sbc16(z80 *z, uint16_t v)
{
    uint16_t hl = HL(z);
    uint32_t r32 = ((uint32_t)hl - v - (z->f & Z80_FC)) & 0x1ffff;
    uint16_t r = (uint16_t)r32;
    uint8_t f = Z80_FN | ((r >> 8) & (Z80_FS | Z80_FX | Z80_FY)) | (((hl ^ v ^ r) >> 8) & Z80_FH);
    if (r32 > 0xffff)
        f |= Z80_FC;
    if (((hl ^ v) & 0x8000) && ((hl ^ r) & 0x8000))
        f |= Z80_FP;
    if (!r)
        f |= Z80_FZ;
    z->memptr = (uint16_t)(hl + 1);
    set_hl(z, r);
    setf(z, f);
}

/* CB-prefixed rotate/shift group. */
static uint8_t rot(z80 *z, int op, uint8_t v)
{
    uint8_t r, c;
    switch (op) {
    case 0: r = (uint8_t)(v << 1 | v >> 7); c = v >> 7; break;            /* RLC */
    case 1: r = (uint8_t)(v >> 1 | v << 7); c = v & 1; break;             /* RRC */
    case 2: r = (uint8_t)(v << 1 | (z->f & Z80_FC)); c = v >> 7; break;  /* RL */
    case 3: r = (uint8_t)(v >> 1 | (z->f & Z80_FC) << 7); c = v & 1; break; /* RR */
    case 4: r = (uint8_t)(v << 1); c = v >> 7; break;                     /* SLA */
    case 5: r = (uint8_t)(v >> 1 | (v & 0x80)); c = v & 1; break;         /* SRA */
    case 6: r = (uint8_t)(v << 1 | 1); c = v >> 7; break;                 /* SLL */
    default: r = (uint8_t)(v >> 1); c = v & 1; break;                     /* SRL */
    }
    setf(z, sz53p[r] | c);
    return r;
}

static void bit(z80 *z, int n, uint8_t v, uint8_t xy)
{
    uint8_t f = (z->f & Z80_FC) | Z80_FH | (xy & (Z80_FX | Z80_FY));
    uint8_t b = v & (1 << n);
    if (!b)
        f |= Z80_FZ | Z80_FP;
    if (n == 7 && b)
        f |= Z80_FS;
    setf(z, f);
}

static void daa(z80 *z)
{
    uint8_t a = z->a, add = 0, carry = z->f & Z80_FC, r;
    if ((z->f & Z80_FH) || (a & 0x0f) > 9)
        add = 6;
    if (carry || a > 0x99)
        add |= 0x60;
    if (a > 0x99)
        carry = Z80_FC;
    if (z->f & Z80_FN) {
        z->f = sub_flags(z, add, 0, &r);
        z->a = r;
    } else {
        add_a(z, add, 0);
    }
    setf(z, (uint8_t)((z->f & ~(Z80_FC | Z80_FP)) | carry | parity[z->a]));
}

/* ---- interrupts ------------------------------------------------------- */

int z80_interrupt(z80 *z, const z80_bus *bus)
{
    uint8_t data;
    if (!z80_int_enabled(z))
        return 0;
    z->halted = 0;
    z->iff1 = z->iff2 = 0;
    inc_r(z);
    z->q = 0;
    data = bus->int_ack ? bus->int_ack(bus->ctx) : 0xff;
    bus->tick(bus->ctx, 7);
    push16(z, bus, z->pc);
    if (z->im == 2)
        z->pc = read16(bus, (uint16_t)(z->i << 8 | data));
    else
        z->pc = 0x0038; /* IM 0 with 0xFF on the bus is RST 38 */
    z->memptr = z->pc;
    return 1;
}

void z80_nmi(z80 *z, const z80_bus *bus)
{
    z->halted = 0;
    z->iff1 = 0;
    inc_r(z);
    z->q = 0;
    bus->tick(bus->ctx, 5);
    push16(z, bus, z->pc);
    z->pc = 0x0066;
    z->memptr = z->pc;
}

/* ---- execution -------------------------------------------------------- */

static void exec_main(z80 *z, const z80_bus *bus, uint8_t op, int idx);
static void exec_cb(z80 *z, const z80_bus *bus, uint8_t op);
static void exec_ddcb(z80 *z, const z80_bus *bus, int idx);
static void exec_ed(z80 *z, const z80_bus *bus, uint8_t op);

void z80_step(z80 *z, const z80_bus *bus)
{
    uint8_t op;
    int idx = IDX_HL;

    z->ei_delay = 0;
    z->prev_q = z->q;
    z->q = 0;

    if (z->halted) {
        /* HALT executes NOPs: PC already points past the HALT and the
         * CPU keeps fetching (and discarding) the byte there. */
        bus->fetch(bus->ctx, z->pc);
        inc_r(z);
        return;
    }

    op = fetch_op(z, bus);
    while (op == 0xdd || op == 0xfd) {
        idx = op == 0xdd ? IDX_IX : IDX_IY;
        z->prev_q = 0; /* a prefix counts as an instruction that left F alone */
        op = fetch_op(z, bus);
    }
    if (op == 0xcb) {
        if (idx == IDX_HL)
            exec_cb(z, bus, fetch_op(z, bus));
        else
            exec_ddcb(z, bus, idx);
    } else if (op == 0xed) {
        exec_ed(z, bus, fetch_op(z, bus));
    } else {
        exec_main(z, bus, op, idx);
    }
}

static void swap8(uint8_t *a, uint8_t *b)
{
    uint8_t t = *a;
    *a = *b;
    *b = t;
}

static void exec_main(z80 *z, const z80_bus *bus, uint8_t op, int idx)
{
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    uint16_t addr, v16, nn;
    uint8_t v, n;
    int8_t d;

    switch (x) {
    case 0:
        switch (zz) {
        case 0:
            switch (y) {
            case 0: break; /* NOP */
            case 1: swap8(&z->a, &z->a_); swap8(&z->f, &z->f_); break;
            case 2: /* DJNZ */
                INTERNAL(IR(z), 1);
                d = (int8_t)RD(z->pc);
                z->b--;
                if (z->b) {
                    INTERNAL(z->pc, 5);
                    z->pc = (uint16_t)(z->pc + 1 + d);
                    z->memptr = z->pc;
                } else {
                    z->pc++;
                }
                break;
            default: /* JR, JR cc */
                d = (int8_t)RD(z->pc);
                if (y == 3 || cond(z, y - 4)) {
                    INTERNAL(z->pc, 5);
                    z->pc = (uint16_t)(z->pc + 1 + d);
                    z->memptr = z->pc;
                } else {
                    z->pc++;
                }
                break;
            }
            break;
        case 1:
            if (!q) {
                set_rp(z, p, idx, imm16(z, bus));
            } else {
                INTERNAL(IR(z), 7);
                set_idx(z, idx, add16(z, get_idx(z, idx), rp(z, p, idx)));
            }
            break;
        case 2:
            if (!q) {
                switch (p) {
                case 0:
                case 1:
                    addr = p ? DE(z) : BC(z);
                    WR(addr, z->a);
                    z->memptr = (uint16_t)(z->a << 8 | ((addr + 1) & 0xff));
                    break;
                case 2:
                    addr = imm16(z, bus);
                    write16(bus, addr, get_idx(z, idx));
                    z->memptr = (uint16_t)(addr + 1);
                    break;
                default:
                    addr = imm16(z, bus);
                    WR(addr, z->a);
                    z->memptr = (uint16_t)(z->a << 8 | ((addr + 1) & 0xff));
                    break;
                }
            } else {
                switch (p) {
                case 0:
                case 1:
                    addr = p ? DE(z) : BC(z);
                    z->a = RD(addr);
                    z->memptr = (uint16_t)(addr + 1);
                    break;
                case 2:
                    addr = imm16(z, bus);
                    set_idx(z, idx, read16(bus, addr));
                    z->memptr = (uint16_t)(addr + 1);
                    break;
                default:
                    addr = imm16(z, bus);
                    z->a = RD(addr);
                    z->memptr = (uint16_t)(addr + 1);
                    break;
                }
            }
            break;
        case 3:
            INTERNAL(IR(z), 2);
            v16 = rp(z, p, idx);
            set_rp(z, p, idx, (uint16_t)(q ? v16 - 1 : v16 + 1));
            break;
        case 4:
        case 5:
            if (y == 6) {
                addr = mem_operand(z, bus, idx);
                v = RD(addr);
                INTERNAL(addr, 1);
                WR(addr, zz == 4 ? inc8(z, v) : dec8(z, v));
            } else {
                v = reg8(z, y, idx);
                set_reg8(z, y, idx, zz == 4 ? inc8(z, v) : dec8(z, v));
            }
            break;
        case 6:
            if (y == 6) {
                if (idx == IDX_HL) {
                    addr = HL(z);
                } else {
                    d = (int8_t)imm8(z, bus);
                    addr = (uint16_t)(get_idx(z, idx) + d);
                    z->memptr = addr;
                }
                n = RD(z->pc);
                if (idx != IDX_HL)
                    INTERNAL(z->pc, 2);
                z->pc++;
                WR(addr, n);
            } else {
                set_reg8(z, y, idx, imm8(z, bus));
            }
            break;
        default:
            switch (y) {
            case 0: /* RLCA */
                z->a = (uint8_t)(z->a << 1 | z->a >> 7);
                setf(z, (z->f & (Z80_FS | Z80_FZ | Z80_FP)) | (z->a & (Z80_FX | Z80_FY | Z80_FC)));
                break;
            case 1: /* RRCA */
                v = z->a & 1;
                z->a = (uint8_t)(z->a >> 1 | z->a << 7);
                setf(z, (z->f & (Z80_FS | Z80_FZ | Z80_FP)) | (z->a & (Z80_FX | Z80_FY)) | v);
                break;
            case 2: /* RLA */
                v = z->a >> 7;
                z->a = (uint8_t)(z->a << 1 | (z->f & Z80_FC));
                setf(z, (z->f & (Z80_FS | Z80_FZ | Z80_FP)) | (z->a & (Z80_FX | Z80_FY)) | v);
                break;
            case 3: /* RRA */
                v = z->a & 1;
                z->a = (uint8_t)(z->a >> 1 | (z->f & Z80_FC) << 7);
                setf(z, (z->f & (Z80_FS | Z80_FZ | Z80_FP)) | (z->a & (Z80_FX | Z80_FY)) | v);
                break;
            case 4: daa(z); break;
            case 5: /* CPL */
                z->a = (uint8_t)~z->a;
                setf(z, (z->f & (Z80_FS | Z80_FZ | Z80_FP | Z80_FC)) | (z->a & (Z80_FX | Z80_FY)) | Z80_FH | Z80_FN);
                break;
            case 6: /* SCF */
                v = ((z->prev_q ^ z->f) | z->a) & (Z80_FX | Z80_FY);
                setf(z, (z->f & (Z80_FS | Z80_FZ | Z80_FP)) | v | Z80_FC);
                break;
            default: /* CCF */
                v = ((z->prev_q ^ z->f) | z->a) & (Z80_FX | Z80_FY);
                setf(z, (z->f & (Z80_FS | Z80_FZ | Z80_FP)) | v | ((z->f & Z80_FC) ? Z80_FH : Z80_FC));
                break;
            }
            break;
        }
        break;

    case 1:
        if (op == 0x76) {
            z->halted = 1;
        } else if (y == 6) {
            addr = mem_operand(z, bus, idx);
            WR(addr, reg8(z, zz, IDX_HL));
        } else if (zz == 6) {
            addr = mem_operand(z, bus, idx);
            set_reg8(z, y, IDX_HL, RD(addr));
        } else {
            set_reg8(z, y, idx, reg8(z, zz, idx));
        }
        break;

    case 2:
        if (zz == 6) {
            addr = mem_operand(z, bus, idx);
            v = RD(addr);
        } else {
            v = reg8(z, zz, idx);
        }
        alu(z, y, v);
        break;

    default:
        switch (zz) {
        case 0: /* RET cc */
            INTERNAL(IR(z), 1);
            if (cond(z, y)) {
                z->pc = pop16(z, bus);
                z->memptr = z->pc;
            }
            break;
        case 1:
            if (!q) {
                v16 = pop16(z, bus);
                if (p == 3)
                    set_af(z, v16);
                else
                    set_rp(z, p, idx, v16);
            } else {
                switch (p) {
                case 0: z->pc = pop16(z, bus); z->memptr = z->pc; break;
                case 1:
                    swap8(&z->b, &z->b_); swap8(&z->c, &z->c_);
                    swap8(&z->d, &z->d_); swap8(&z->e, &z->e_);
                    swap8(&z->h, &z->h_); swap8(&z->l, &z->l_);
                    break;
                case 2: z->pc = get_idx(z, idx); break;
                default: INTERNAL(IR(z), 2); z->sp = get_idx(z, idx); break;
                }
            }
            break;
        case 2: /* JP cc,nn */
            nn = imm16(z, bus);
            z->memptr = nn;
            if (cond(z, y))
                z->pc = nn;
            break;
        case 3:
            switch (y) {
            case 0: nn = imm16(z, bus); z->memptr = nn; z->pc = nn; break;
            case 2: /* OUT (n),A */
                n = imm8(z, bus);
                bus->io_write(bus->ctx, (uint16_t)(z->a << 8 | n), z->a);
                z->memptr = (uint16_t)(z->a << 8 | ((n + 1) & 0xff));
                break;
            case 3: /* IN A,(n) */
                n = imm8(z, bus);
                addr = (uint16_t)(z->a << 8 | n);
                z->a = bus->io_read(bus->ctx, addr);
                z->memptr = (uint16_t)(addr + 1);
                break;
            case 4: /* EX (SP),HL */
                addr = z->sp;
                v16 = read16(bus, addr);
                INTERNAL(addr + 1, 1);
                nn = get_idx(z, idx);
                WR(addr + 1, nn >> 8);
                WR(addr, nn);
                INTERNAL(addr, 2);
                set_idx(z, idx, v16);
                z->memptr = v16;
                break;
            case 5: swap8(&z->d, &z->h); swap8(&z->e, &z->l); break;
            case 6: z->iff1 = z->iff2 = 0; break;
            case 7: z->iff1 = z->iff2 = 1; z->ei_delay = 1; break;
            default: break; /* CB: handled in z80_step */
            }
            break;
        case 4: /* CALL cc,nn */
            nn = imm16(z, bus);
            z->memptr = nn;
            if (cond(z, y)) {
                INTERNAL(z->pc - 1, 1);
                push16(z, bus, z->pc);
                z->pc = nn;
            }
            break;
        case 5:
            if (!q) {
                INTERNAL(IR(z), 1);
                push16(z, bus, p == 3 ? AF(z) : rp(z, p, idx));
            } else { /* CALL nn; DD/ED/FD are consumed in z80_step */
                nn = imm16(z, bus);
                z->memptr = nn;
                INTERNAL(z->pc - 1, 1);
                push16(z, bus, z->pc);
                z->pc = nn;
            }
            break;
        case 6: alu(z, y, imm8(z, bus)); break;
        default: /* RST */
            INTERNAL(IR(z), 1);
            push16(z, bus, z->pc);
            z->pc = (uint16_t)(y * 8);
            z->memptr = z->pc;
            break;
        }
        break;
    }
}

static void exec_cb(z80 *z, const z80_bus *bus, uint8_t op)
{
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7;
    uint16_t addr;
    uint8_t v;

    if (zz == 6) {
        addr = HL(z);
        v = RD(addr);
        INTERNAL(addr, 1);
        switch (x) {
        case 0: WR(addr, rot(z, y, v)); break;
        case 1: bit(z, y, v, (uint8_t)(z->memptr >> 8)); break;
        case 2: WR(addr, v & ~(1 << y)); break;
        default: WR(addr, v | (1 << y)); break;
        }
    } else {
        v = reg8(z, zz, IDX_HL);
        switch (x) {
        case 0: set_reg8(z, zz, IDX_HL, rot(z, y, v)); break;
        case 1: bit(z, y, v, v); break;
        case 2: set_reg8(z, zz, IDX_HL, (uint8_t)(v & ~(1 << y))); break;
        default: set_reg8(z, zz, IDX_HL, (uint8_t)(v | (1 << y))); break;
        }
    }
}

static void exec_ddcb(z80 *z, const z80_bus *bus, int idx)
{
    int8_t d = (int8_t)imm8(z, bus);
    uint8_t op = RD(z->pc), v, r;
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7;
    uint16_t addr;

    INTERNAL(z->pc, 2);
    z->pc++;
    addr = (uint16_t)(get_idx(z, idx) + d);
    z->memptr = addr;
    v = RD(addr);
    INTERNAL(addr, 1);
    switch (x) {
    case 0: r = rot(z, y, v); break;
    case 1: bit(z, y, v, (uint8_t)(addr >> 8)); return;
    case 2: r = (uint8_t)(v & ~(1 << y)); break;
    default: r = (uint8_t)(v | (1 << y)); break;
    }
    WR(addr, r);
    if (zz != 6) /* undocumented: result also lands in a register */
        set_reg8(z, zz, IDX_HL, r);
}

/* Undocumented flag adjustment for INIR/INDR/OTIR/OTDR while repeating. */
static void io_block_repeat_flags(z80 *z, uint8_t v)
{
    uint8_t f = (uint8_t)((z->f & ~(Z80_FX | Z80_FY)) | ((z->pc >> 8) & (Z80_FX | Z80_FY)));
    if (f & Z80_FC) {
        f &= (uint8_t)~Z80_FH;
        if (v & 0x80) {
            f ^= parity[(z->b - 1) & 7] ^ Z80_FP;
            if (!(z->b & 0x0f))
                f |= Z80_FH;
        } else {
            f ^= parity[(z->b + 1) & 7] ^ Z80_FP;
            if ((z->b & 0x0f) == 0x0f)
                f |= Z80_FH;
        }
    } else {
        f ^= parity[z->b & 7] ^ Z80_FP;
    }
    setf(z, f);
}

static void io_block_flags(z80 *z, uint8_t v, unsigned k)
{
    uint8_t f = sz53[z->b];
    if (v & 0x80)
        f |= Z80_FN;
    if (k > 0xff)
        f |= Z80_FH | Z80_FC;
    f |= parity[((k & 7) ^ z->b) & 0xff];
    setf(z, f);
}

static void block(z80 *z, const z80_bus *bus, int y, int zz)
{
    int dec = y & 1, repeat = y >= 6;
    int16_t stp = dec ? -1 : 1;
    uint16_t hl = HL(z), de = DE(z), port;
    uint8_t v, r, f, h, c;

    switch (zz) {
    case 0: /* LDI/LDD/LDIR/LDDR */
        v = RD(hl);
        WR(de, v);
        INTERNAL(de, 2);
        set_bc(z, (uint16_t)(BC(z) - 1));
        set_hl(z, (uint16_t)(hl + stp));
        set_de(z, (uint16_t)(de + stp));
        r = (uint8_t)(v + z->a);
        f = (z->f & (Z80_FS | Z80_FZ | Z80_FC)) | (r & Z80_FX) | ((r & 0x02) << 4);
        if (BC(z))
            f |= Z80_FP;
        setf(z, f);
        if (repeat && BC(z)) {
            INTERNAL(de, 5);
            z->pc -= 2;
            z->memptr = (uint16_t)(z->pc + 1);
            setf(z, (uint8_t)((z->f & ~(Z80_FX | Z80_FY)) | ((z->pc >> 8) & (Z80_FX | Z80_FY))));
        }
        break;
    case 1: /* CPI/CPD/CPIR/CPDR */
        v = RD(hl);
        INTERNAL(hl, 5);
        r = (uint8_t)(z->a - v);
        set_hl(z, (uint16_t)(hl + stp));
        set_bc(z, (uint16_t)(BC(z) - 1));
        h = (z->a ^ v ^ r) & Z80_FH;
        f = (z->f & Z80_FC) | Z80_FN | h | (sz53[r] & (Z80_FS | Z80_FZ));
        if (h)
            r--;
        f |= (r & Z80_FX) | ((r & 0x02) << 4);
        if (h)
            r++;
        if (BC(z))
            f |= Z80_FP;
        setf(z, f);
        z->memptr = (uint16_t)(z->memptr + stp);
        if (repeat && BC(z) && r) {
            INTERNAL(hl, 5);
            z->pc -= 2;
            z->memptr = (uint16_t)(z->pc + 1);
            setf(z, (uint8_t)((z->f & ~(Z80_FX | Z80_FY)) | ((z->pc >> 8) & (Z80_FX | Z80_FY))));
        }
        break;
    case 2: /* INI/IND/INIR/INDR */
        INTERNAL(IR(z), 1);
        port = BC(z);
        v = bus->io_read(bus->ctx, port);
        WR(hl, v);
        z->memptr = (uint16_t)(port + stp);
        z->b--;
        set_hl(z, (uint16_t)(hl + stp));
        c = (uint8_t)(z->c + stp);
        io_block_flags(z, v, (unsigned)v + c);
        if (repeat && z->b) {
            INTERNAL(hl, 5);
            z->pc -= 2;
            z->memptr = (uint16_t)(z->pc + 1);
            io_block_repeat_flags(z, v);
        }
        break;
    default: /* OUTI/OUTD/OTIR/OTDR */
        INTERNAL(IR(z), 1);
        v = RD(hl);
        z->b--;
        port = BC(z);
        z->memptr = (uint16_t)(port + stp);
        bus->io_write(bus->ctx, port, v);
        set_hl(z, (uint16_t)(hl + stp));
        io_block_flags(z, v, (unsigned)v + z->l);
        if (repeat && z->b) {
            INTERNAL(port, 5);
            z->pc -= 2;
            z->memptr = (uint16_t)(z->pc + 1);
            io_block_repeat_flags(z, v);
        }
        break;
    }
}

/* Z80N extended opcodes. Returns 0 if `op` is not one of them. */
static int exec_z80n(z80 *z, const z80_bus *bus, uint8_t op)
{
    uint16_t de = DE(z), hl = HL(z), nn, src;
    uint8_t v, n, reg;

    switch (op) {
    case 0x23: z->a = (uint8_t)(z->a << 4 | z->a >> 4); break; /* SWAPNIB */
    case 0x24: /* MIRROR A */
        v = z->a;
        v = (uint8_t)((v & 0xf0) >> 4 | (v & 0x0f) << 4);
        v = (uint8_t)((v & 0xcc) >> 2 | (v & 0x33) << 2);
        v = (uint8_t)((v & 0xaa) >> 1 | (v & 0x55) << 1);
        z->a = v;
        break;
    case 0x27: /* TEST n */
        n = imm8(z, bus);
        setf(z, sz53p[z->a & n] | Z80_FH);
        break;
    case 0x28: n = z->b & 0x1f; set_de(z, n > 15 ? 0 : (uint16_t)(de << n)); break; /* BSLA */
    case 0x29: /* BSRA */
        n = z->b & 0x1f;
        if (n > 15)
            n = 15;
        set_de(z, (uint16_t)((de >> n) | ((de & 0x8000) ? (uint16_t)(0xffff << (15 - n)) : 0)));
        break;
    case 0x2a: n = z->b & 0x1f; set_de(z, n > 15 ? 0 : (uint16_t)(de >> n)); break; /* BSRL */
    case 0x2b: n = z->b & 0x1f; set_de(z, n > 15 ? 0xffff : (uint16_t)~((uint16_t)~de >> n)); break; /* BSRF */
    case 0x2c: n = z->b & 0x0f; set_de(z, (uint16_t)(de << n | de >> ((16 - n) & 15))); break; /* BRLC */
    case 0x30: set_de(z, (uint16_t)(z->d * z->e)); break; /* MUL D,E */
    case 0x31: set_hl(z, (uint16_t)(hl + z->a)); break;
    case 0x32: set_de(z, (uint16_t)(de + z->a)); break;
    case 0x33: set_bc(z, (uint16_t)(BC(z) + z->a)); break;
    case 0x34: case 0x35: case 0x36: /* ADD rr,nn */
        nn = imm16(z, bus);
        INTERNAL(z->pc - 1, 2);
        if (op == 0x34)
            set_hl(z, (uint16_t)(hl + nn));
        else if (op == 0x35)
            set_de(z, (uint16_t)(de + nn));
        else
            set_bc(z, (uint16_t)(BC(z) + nn));
        break;
    case 0x8a: /* PUSH nn, operand stored big-endian */
        nn = (uint16_t)(imm8(z, bus) << 8);
        nn |= imm8(z, bus);
        INTERNAL(IR(z), 3);
        push16(z, bus, nn);
        break;
    case 0x90: /* OUTINB */
        INTERNAL(IR(z), 1);
        v = RD(hl);
        bus->io_write(bus->ctx, BC(z), v);
        set_hl(z, (uint16_t)(hl + 1));
        break;
    case 0x91: /* NEXTREG n,n */
        reg = imm8(z, bus);
        v = imm8(z, bus);
        INTERNAL(z->pc - 1, 6);
        if (bus->nextreg_write)
            bus->nextreg_write(bus->ctx, reg, v);
        break;
    case 0x92: /* NEXTREG n,A */
        reg = imm8(z, bus);
        INTERNAL(z->pc - 1, 6);
        if (bus->nextreg_write)
            bus->nextreg_write(bus->ctx, reg, z->a);
        break;
    case 0x93: /* PIXELDN */
        if ((hl & 0x0700) != 0x0700)
            hl = (uint16_t)(hl + 0x100);
        else if ((hl & 0xe0) != 0xe0)
            hl = (uint16_t)((hl & 0xf8ff) + 0x20);
        else
            hl = (uint16_t)((hl & 0xf81f) + 0x800);
        set_hl(z, hl);
        break;
    case 0x94: /* PIXELAD */
        set_hl(z, (uint16_t)(0x4000 + ((z->d & 0xc0) << 5) + ((z->d & 0x07) << 8) +
                             ((z->d & 0x38) << 2) + (z->e >> 3)));
        break;
    case 0x95: z->a = (uint8_t)(0x80 >> (z->e & 7)); break; /* SETAE */
    case 0x98: /* JP (C) */
        v = bus->io_read(bus->ctx, BC(z));
        INTERNAL(IR(z), 1);
        z->pc = (uint16_t)((z->pc & 0xc000) + (v << 6));
        break;
    case 0xa4: case 0xac: case 0xb4: case 0xbc: /* LDIX, LDDX, LDIRX, LDDRX */
        v = RD(hl);
        if (v != z->a)
            WR(de, v);
        else
            bus->tick(bus->ctx, 3);
        INTERNAL(de, 2);
        set_hl(z, (uint16_t)((op & 0x08) ? hl - 1 : hl + 1));
        set_de(z, (uint16_t)(de + 1));
        set_bc(z, (uint16_t)(BC(z) - 1));
        if ((op & 0x10) && BC(z)) {
            INTERNAL(de, 5);
            z->pc -= 2;
        }
        break;
    case 0xa5: /* LDWS */
        WR(de, RD(hl));
        z->l++;
        z->d = inc8(z, z->d);
        break;
    case 0xb7: /* LDPIRX */
        src = (uint16_t)((hl & 0xfff8) | (z->e & 7));
        v = RD(src);
        if (v != z->a)
            WR(de, v);
        else
            bus->tick(bus->ctx, 3);
        INTERNAL(de, 2);
        set_de(z, (uint16_t)(de + 1));
        set_bc(z, (uint16_t)(BC(z) - 1));
        if (BC(z)) {
            INTERNAL(de, 5);
            z->pc -= 2;
        }
        break;
    default:
        return 0;
    }
    return 1;
}

static void exec_ed(z80 *z, const z80_bus *bus, uint8_t op)
{
    int x = op >> 6, y = (op >> 3) & 7, zz = op & 7, p = y >> 1, q = y & 1;
    uint16_t port, addr, hl;
    uint8_t v, r;

    if (z->z80n && exec_z80n(z, bus, op))
        return;

    if (x == 2) {
        if (zz <= 3 && y >= 4)
            block(z, bus, y, zz);
        return;
    }
    if (x != 1)
        return; /* invalid ED: 8 T-state NOP */

    switch (zz) {
    case 0: /* IN r,(C) */
        port = BC(z);
        v = bus->io_read(bus->ctx, port);
        z->memptr = (uint16_t)(port + 1);
        if (y != 6)
            set_reg8(z, y, IDX_HL, v);
        setf(z, (z->f & Z80_FC) | sz53p[v]);
        break;
    case 1: /* OUT (C),r */
        port = BC(z);
        bus->io_write(bus->ctx, port, y == 6 ? 0 : reg8(z, y, IDX_HL));
        z->memptr = (uint16_t)(port + 1);
        break;
    case 2:
        INTERNAL(IR(z), 7);
        if (!q)
            sbc16(z, rp(z, p, IDX_HL));
        else
            adc16(z, rp(z, p, IDX_HL));
        break;
    case 3:
        addr = imm16(z, bus);
        if (!q)
            write16(bus, addr, rp(z, p, IDX_HL));
        else
            set_rp(z, p, IDX_HL, read16(bus, addr));
        z->memptr = (uint16_t)(addr + 1);
        break;
    case 4: /* NEG */
        v = z->a;
        z->a = 0;
        setf(z, sub_flags(z, v, 0, &r));
        z->a = r;
        break;
    case 5: /* RETN / RETI */
        z->iff1 = z->iff2;
        z->pc = pop16(z, bus);
        z->memptr = z->pc;
        if (y == 1) {
            if (bus->reti)
                bus->reti(bus->ctx);
        } else if (bus->retn) {
            bus->retn(bus->ctx);
        }
        break;
    case 6:
        z->im = (uint8_t)((y & 3) < 2 ? 0 : (y & 3) - 1);
        break;
    default:
        switch (y) {
        case 0: INTERNAL(IR(z), 1); z->i = z->a; break;
        case 1: INTERNAL(IR(z), 1); z->r = z->a; break;
        case 2:
        case 3:
            INTERNAL(IR(z), 1);
            z->a = y == 2 ? z->i : z->r;
            setf(z, (z->f & Z80_FC) | sz53[z->a] | (z->iff2 ? Z80_FP : 0));
            break;
        case 4: /* RRD */
        case 5: /* RLD */
            hl = HL(z);
            v = RD(hl);
            INTERNAL(hl, 4);
            if (y == 4) {
                WR(hl, (z->a << 4) | (v >> 4));
                z->a = (uint8_t)((z->a & 0xf0) | (v & 0x0f));
            } else {
                WR(hl, (v << 4) | (z->a & 0x0f));
                z->a = (uint8_t)((z->a & 0xf0) | (v >> 4));
            }
            setf(z, (z->f & Z80_FC) | sz53p[z->a]);
            z->memptr = (uint16_t)(hl + 1);
            break;
        default: break;
        }
        break;
    }
}
