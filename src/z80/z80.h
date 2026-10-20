/*
 * Z80 / Z80N CPU core.
 *
 * Timing model: the CPU never counts T-states itself. Every machine cycle
 * (opcode fetch, memory read/write, I/O, internal cycles) is reported to the
 * bus, in the order the real CPU performs it, and the bus advances the clock
 * by the cycle's length plus any wait/contention states the machine inserts.
 * That keeps ULA contention, +3 MREQ-only contention and the Next's turbo
 * speeds entirely in the machine model.
 *
 * Undocumented behaviour is implemented: X/Y flags, MEMPTR (WZ), Q for
 * SCF/CCF, and block-instruction flags while repeating.
 */
#ifndef ZXN_Z80_H
#define ZXN_Z80_H

#include <stdint.h>

#define Z80_FC 0x01
#define Z80_FN 0x02
#define Z80_FP 0x04
#define Z80_FX 0x08
#define Z80_FH 0x10
#define Z80_FY 0x20
#define Z80_FZ 0x40
#define Z80_FS 0x80

typedef struct z80_bus {
    void *ctx;
    /* Opcode fetch (M1), 4 T-states. */
    uint8_t (*fetch)(void *ctx, uint16_t addr);
    /* Memory read / write, 3 T-states each. */
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t val);
    /* I/O read / write, 4 T-states each (wait state included). */
    uint8_t (*io_read)(void *ctx, uint16_t port);
    void (*io_write)(void *ctx, uint16_t port, uint8_t val);
    /* `cycles` T-states with `addr` on the address bus but no MREQ.
     * Contended one T-state at a time on 48K/128K. */
    void (*internal)(void *ctx, uint16_t addr, unsigned cycles);
    /* Plain uncontended T-states (interrupt acknowledge). */
    void (*tick)(void *ctx, unsigned cycles);
    /* Optional (may be NULL). */
    uint8_t (*int_ack)(void *ctx);
    void (*nextreg_write)(void *ctx, uint8_t reg, uint8_t val);
    void (*reti)(void *ctx);
} z80_bus;

typedef struct z80 {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a_, f_, b_, c_, d_, e_, h_, l_;
    uint16_t ix, iy, sp, pc;
    uint8_t i, r;
    uint8_t iff1, iff2, im, halted;
    uint16_t memptr;
    /* Flags written by the last instruction, 0 if it left F alone. */
    uint8_t q, prev_q;
    /* Set by EI: no interrupt is taken until after the next instruction. */
    uint8_t ei_delay;
    /* Enables the ZX Spectrum Next extended instruction set. */
    uint8_t z80n;
} z80;

void z80_init(z80 *cpu, int z80n);
void z80_reset(z80 *cpu);
/* Execute one complete instruction, prefixes included. */
void z80_step(z80 *cpu, const z80_bus *bus);
/* Accept a maskable interrupt if enabled; returns 1 if taken. */
int z80_interrupt(z80 *cpu, const z80_bus *bus);
void z80_nmi(z80 *cpu, const z80_bus *bus);

static inline int z80_int_enabled(const z80 *cpu) { return cpu->iff1 && !cpu->ei_delay; }

#endif
