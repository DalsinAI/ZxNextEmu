/*
 * General Sound (gs.h).
 */
#include "gs.h"

#include <string.h>

static void advance(gs *g, unsigned n)
{
    g->clock += n;
    g->acc_l += (int64_t)g->level_l * n;
    g->acc_r += (int64_t)g->level_r * n;
    g->acc_t += n;
}

static void levels(gs *g)
{
    g->level_l = ((int32_t)g->dac[0] - 128) * g->vol[0] + ((int32_t)g->dac[1] - 128) * g->vol[1];
    g->level_r = ((int32_t)g->dac[2] - 128) * g->vol[2] + ((int32_t)g->dac[3] - 128) * g->vol[3];
}

static uint8_t mem(gs *g, uint16_t a)
{
    uint8_t v;
    if (a < 0x4000)
        return g->rom[a];
    if (a < 0x8000) {
        v = g->ram[a - 0x4000];
        if (a >= 0x6000) {           /* the DAC latches */
            g->dac[(a >> 8) & 3] = v;
            levels(g);
        }
        return v;
    }
    if (!g->page)
        return g->rom[a - 0x8000];
    return g->ram[(uint32_t)g->page * 0x8000 + (a - 0x8000)];
}

static uint8_t bus_fetch(void *ctx, uint16_t a) { advance(ctx, 4); return mem(ctx, a); }
static uint8_t bus_read(void *ctx, uint16_t a) { advance(ctx, 3); return mem(ctx, a); }

static void bus_write(void *ctx, uint16_t a, uint8_t v)
{
    gs *g = ctx;
    advance(g, 3);
    if (a >= 0x4000 && a < 0x8000)
        g->ram[a - 0x4000] = v;
    else if (a >= 0x8000 && g->page)
        g->ram[(uint32_t)g->page * 0x8000 + (a - 0x8000)] = v;
}

static uint8_t bus_in(void *ctx, uint16_t port)
{
    gs *g = ctx;
    advance(g, 4);
    switch (port & 0x0f) {
    case 0x01: return g->cmd;
    case 0x02: g->stat &= 0x7f; return g->to_gs;
    case 0x03: g->stat |= 0x80; g->from_gs = 0xff; return 0xff;
    case 0x04: return g->stat;
    case 0x05: g->stat &= 0xfe; return 0xff;
    case 0x0a: g->stat = (uint8_t)((g->stat & 0x7f) | (g->page << 7)); return 0xff;
    case 0x0b: g->stat = (uint8_t)((g->stat & 0xfe) | ((g->vol[0] >> 5) & 1)); return 0xff;
    default: return 0xff;
    }
}

static void bus_out(void *ctx, uint16_t port, uint8_t v)
{
    gs *g = ctx;
    advance(g, 4);
    switch (port & 0x0f) {
    case 0x00: g->page = v & (GS_PAGES - 1); break;
    case 0x02: g->stat &= 0x7f; break;
    case 0x03: g->stat |= 0x80; g->from_gs = v; break;
    case 0x05: g->stat &= 0xfe; break;
    case 0x06: case 0x07: case 0x08: case 0x09:
        g->vol[(port & 0x0f) - 6] = v & 0x3f;
        levels(g);
        break;
    case 0x0a: g->stat = (uint8_t)((g->stat & 0x7f) | (g->page << 7)); break;
    case 0x0b: g->stat = (uint8_t)((g->stat & 0xfe) | ((g->vol[0] >> 5) & 1)); break;
    }
}

static void bus_internal(void *ctx, uint16_t a, unsigned n) { (void)a; advance(ctx, n); }
static void bus_tick(void *ctx, unsigned n) { advance(ctx, n); }

static const z80_bus gs_bus = {
    NULL, bus_fetch, bus_read, bus_write, bus_in, bus_out, bus_internal, bus_tick,
    NULL, NULL, NULL, NULL
};

void gs_init(gs *g, const uint8_t *rom)
{
    memset(g, 0, sizeof *g);
    memcpy(g->rom, rom, GS_ROM);
    z80_init(&g->cpu, 0);
    gs_reset(g);
}

void gs_reset(gs *g)
{
    z80_reset(&g->cpu);
    g->page = 0;
    g->cmd = g->to_gs = g->from_gs = 0;
    g->stat = 0;
    memset(g->vol, 0, sizeof g->vol);
    memset(g->dac, 0x80, sizeof g->dac);
    levels(g);
    g->next_int = g->clock + GS_INT_T;
}

void gs_run(gs *g, uint64_t clocks)
{
    z80_bus bus = gs_bus;
    bus.ctx = g;
    g->target += clocks;
    if (g->target < g->clock)        /* the last instruction ran past it */
        return;
    while (g->clock < g->target) {
        if (g->clock >= g->next_int) {
            g->next_int += GS_INT_T;
            if (z80_int_enabled(&g->cpu)) {
                z80_interrupt(&g->cpu, &bus);
                continue;
            }
        }
        if (g->cpu.halted) {
            /* HALT: nothing happens until the next interrupt */
            uint64_t to = g->next_int < g->target ? g->next_int : g->target;
            unsigned n = (unsigned)((to - g->clock + 3) / 4) * 4;
            g->cpu.r = (uint8_t)((g->cpu.r & 0x80) | ((g->cpu.r + n / 4) & 0x7f));
            advance(g, n ? n : 4);
            continue;
        }
        z80_step(&g->cpu, &bus);
    }
}

uint8_t gs_read(gs *g, uint8_t port)
{
    if (port == 0xbb)
        return g->stat | 0x7e;
    g->stat &= 0x7f;                 /* B3 */
    return g->from_gs;
}

void gs_write(gs *g, uint8_t port, uint8_t v)
{
    if (port == 0xbb) {
        g->cmd = v;
        g->stat |= 0x01;
    } else {
        g->to_gs = v;
        g->stat |= 0x80;
    }
}

void gs_take(gs *g, int32_t *l, int32_t *r)
{
    if (g->acc_t) {
        *l = (int32_t)(g->acc_l / (int64_t)g->acc_t);
        *r = (int32_t)(g->acc_r / (int64_t)g->acc_t);
    } else {
        *l = g->level_l;
        *r = g->level_r;
    }
    g->acc_l = g->acc_r = 0;
    g->acc_t = 0;
}
