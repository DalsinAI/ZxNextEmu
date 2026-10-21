/*
 * MOS 6502 (NMOS) CPU core.
 *
 * Timing model: every clock cycle is one bus access, read or write, and
 * each is reported to the bus in the order the real chip makes it,
 * dummy reads and the double write of read-modify-write instructions
 * included. The bus advances the machine by one cycle per call, so VIA
 * timers, video and sound stay exact to the cycle.
 *
 * Interrupts are polled the way the chip does it: the IRQ and NMI lines
 * are sampled at the end of each cycle, and an instruction's last sample
 * but one decides whether an interrupt follows it (so CLI, SEI and PLP
 * act one instruction late, and a taken branch that stays on its page
 * does not poll on its extra cycle). All the undocumented opcodes are
 * implemented; ANE and LXA use the magic constant 0xEE.
 */
#ifndef ZXN_M6502_H
#define ZXN_M6502_H

#include <stdint.h>

#define M6502_FC 0x01
#define M6502_FZ 0x02
#define M6502_FI 0x04
#define M6502_FD 0x08
#define M6502_FB 0x10
#define M6502_FU 0x20
#define M6502_FV 0x40
#define M6502_FN 0x80

typedef struct m6502_bus {
    void *ctx;
    /* One cycle each. */
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t val);
} m6502_bus;

typedef struct m6502 {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    /* Input lines, set by the machine (from inside its bus callbacks too). */
    uint8_t irq_line, nmi_line;
    /* Internal: interrupt poll state, sampled at the end of each cycle. */
    uint8_t nmi_prev, nmi_edge;      /* NMI is edge-triggered */
    uint8_t poll, poll_prev;         /* bit 0 IRQ wanted, bit 1 NMI wanted */
    uint8_t jammed;                  /* a JAM opcode stopped the CPU */
    uint8_t reset_pending;
} m6502;

void m6502_init(m6502 *cpu);
/* The RESET line: the next m6502_step runs the 7-cycle reset sequence. */
void m6502_reset(m6502 *cpu);
/* Execute one instruction, or take an interrupt it has decided on. */
void m6502_step(m6502 *cpu, const m6502_bus *bus);

#endif
