/*
 * The Oric's tape: a .tap file is the bytes the ROM writes, from the
 * 0x16 sync leader on. Each byte goes out as 14 bits, lowest data bit
 * first: a 1, a 0, eight data bits, an odd parity bit and three 1s. A 1
 * is one cycle of 2400 Hz, a 0 one cycle of 1200 Hz. As Oricutron does,
 * each leader is lengthened and a short run of 1s follows each header,
 * so the ROM has time to print the file name.
 *
 * With a known ROM the deck can also hand bytes straight to the ROM's
 * own find-sync and read-byte routines (addresses from Oricutron's ROM
 * patch files). Loaders of their own still get the real signal.
 */
#include "oric.h"

#include <stdlib.h>
#include <string.h>

#define HALF_1 208               /* cycles per half-cycle of a 1 */
#define HALF_0 416
#define EXTRA_LEADER 80          /* sync bytes added to each leader */
#define HEADER_GAP 32            /* 1 bits after each header */

typedef struct rom_traps {
    uint32_t crc;
    uint16_t sync, sync_end, readbyte, readbyte_end;
    uint16_t store_byte, store_zero;   /* 0: none */
    uint8_t set_carry;
} rom_traps;

static const rom_traps traps[] = {
    { 0xf18710b4u, 0xe696, 0xe6b9, 0xe630, 0xe65b, 0x002f, 0, 0 },          /* BASIC 1.0 */
    { 0xc3a92befu, 0xe735, 0xe759, 0xe6c9, 0xe6fb, 0x002f, 0x02b1, 1 },     /* BASIC 1.1 */
};

/* At the first sync byte of a leader that is followed by a header: note
 * where the header ends and lengthen the leader. */
static void setup_header(oric_tape *t)
{
    uint32_t p = t->pos, n = 0;
    while (p < t->len && t->data[p] == 0x16) {
        p++;
        n++;
    }
    if (n < 3 || p >= t->len || t->data[p] != 0x24 || p + 10 >= t->len)
        return;
    p += 10;
    while (p < t->len && t->data[p])
        p++;
    if (p >= t->len)
        return;
    t->header_end = p + 1;
    t->extra_leader = EXTRA_LEADER;
}

static void rewind_signal(oric_tape *t)
{
    t->bit = 0;
    t->half = 0;
    t->count = 0;
    t->gap = 0;
    t->extra_leader = 0;
    t->header_end = 0;
}

int oric_tape_insert(oric *m, const uint8_t *data, size_t len)
{
    uint8_t *copy;
    if (!len || len > 0x1000000 || !(copy = malloc(len)))
        return -1;
    memcpy(copy, data, len);
    oric_tape_eject(m);
    m->tape.data = copy;
    m->tape.len = (uint32_t)len;
    m->tape.level = 1;
    setup_header(&m->tape);
    return 0;
}

void oric_tape_eject(oric *m)
{
    free(m->tape.data);
    memset(&m->tape, 0, sizeof m->tape);
    m->tape.level = 1;
}

static void byte_done(oric_tape *t)
{
    if (t->extra_leader) {
        t->extra_leader--;
        return;
    }
    t->pos++;
    if (t->header_end && t->pos == t->header_end) {
        t->gap = HEADER_GAP;
        t->header_end = 0;
    }
    if (t->pos < t->len && t->data[t->pos] == 0x16 && t->data[t->pos - 1] != 0x16)
        setup_header(t);
}

/* The next bit to send: 1, 0, or -1 at the end of the tape. */
static int next_bit(oric_tape *t)
{
    uint8_t byte;
    int v;
    if (t->gap) {
        t->gap--;
        return 1;
    }
    if (t->pos >= t->len)
        return -1;
    byte = t->data[t->pos];
    switch (t->bit) {
    case 0: v = 1; break;
    case 1: v = 0; t->parity = 1; break;
    case 10: v = t->parity; break;
    case 11: case 12: case 13: v = 1; break;
    default:
        v = byte >> (t->bit - 2) & 1;
        t->parity ^= (uint8_t)v;
        break;
    }
    if (++t->bit == 14) {
        t->bit = 0;
        byte_done(t);
    }
    return v;
}

void oric_tape_tick(oric *m)
{
    oric_tape *t = &m->tape;
    if (!(via_port_b(&m->via) & 0x40))      /* motor relay off */
        return;
    if (t->count && --t->count)
        return;
    if (!t->half) {
        int b = next_bit(t);
        if (b < 0) {
            t->count = 0;
            return;
        }
        t->width = b ? HALF_1 : HALF_0;
        t->level = 0;
        t->half = 1;
    } else {
        t->level = 1;
        t->half = 0;
    }
    t->count = t->width;
    via_set_cb1(&m->via, t->level);
}

int oric_tape_trap(oric *m)
{
    oric_tape *t = &m->tape;
    const rom_traps *tr = NULL;
    size_t i;
    uint16_t pc = m->cpu.pc;
    for (i = 0; i < sizeof traps / sizeof traps[0]; i++)
        if (traps[i].crc == m->rom_crc)
            tr = &traps[i];
    if (!tr)
        return 0;
    if (pc == tr->sync) {
        uint32_t p = t->pos;
        while (p < t->len && t->data[p] != 0x16)
            p++;
        if (p >= t->len)
            return 0;
        t->pos = p;
        rewind_signal(t);
        m->cpu.pc = tr->sync_end;
        return 1;
    }
    if (pc == tr->readbyte) {
        uint8_t v;
        if (t->pos >= t->len)
            return 0;
        v = t->data[t->pos++];
        rewind_signal(t);
        m->cpu.a = v;
        m->cpu.p = (uint8_t)((m->cpu.p & ~(M6502_FZ | M6502_FC)) | (v ? 0 : M6502_FZ)
                             | (tr->set_carry ? M6502_FC : 0));
        if (tr->store_byte)
            m->ram[tr->store_byte] = v;
        if (tr->store_zero)
            m->ram[tr->store_zero] = 0;
        m->cpu.pc = tr->readbyte_end;
        return 1;
    }
    return 0;
}
