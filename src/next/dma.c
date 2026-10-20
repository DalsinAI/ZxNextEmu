/*
 * zxnDMA (device/dma.vhd): the Zilog-style DMA built into the Next.
 *
 * Port 0x6B is the zxnDMA (byte counter starts at 0, a block of N bytes
 * moves N bytes); port 0x0B selects Z80 DMA compatible counting (starts
 * at -1, so N+1 bytes move). While it owns the bus the CPU stops; in
 * burst mode it lets the CPU run while the prescaler holds it back.
 */
#include "next.h"

#include <string.h>

enum {
    WR_IDLE, R0_B0, R0_B1, R0_B2, R0_B3, R1_B0, R1_B1, R2_B0, R2_B1,
    R3_B0, R3_B1, R4_B0, R4_B1, R6_B0
};
enum { RD_STATUS, RD_COUNTER_LO, RD_COUNTER_HI, RD_A_LO, RD_A_HI, RD_B_LO, RD_B_HI };

void nx_dma_reset(zxn *n)
{
    nx_dma *d = &n->dma;
    memset(d, 0, sizeof *d);
    d->a_timing = 1;
    d->b_timing = 1;
    d->mode = 1;
    d->read_mask = 0x7f;
    d->end_of_block_n = 1;
    d->rd_seq = RD_STATUS;
    d->wr_seq = WR_IDLE;
}

static void load(nx_dma *d)
{
    if (d->dir_atob) {
        d->src = d->a_start;
        d->dst = d->b_start;
    } else {
        d->src = d->b_start;
        d->dst = d->a_start;
    }
    d->counter = d->z80_mode ? 0xffff : 0;
}

/* First read-sequence entry at or after `from` that the mask selects. */
static uint8_t next_read(const nx_dma *d, int from)
{
    int i;
    for (i = 0; i < 7; i++) {
        int k = (from + i) % 7;
        if (d->read_mask >> k & 1)
            return (uint8_t)k;
    }
    return RD_STATUS;
}

void nx_dma_write(zxn *n, uint8_t v)
{
    nx_dma *d = &n->dma;
    uint8_t t = d->reg_temp;

    switch (d->wr_seq) {
    case WR_IDLE:
        if (!(v & 0x80) && (v & 3)) {               /* WR0 */
            d->reg_temp = v;
            d->dir_atob = (v >> 2) & 1;
            d->wr_seq = (v & 0x08) ? R0_B0 : (v & 0x10) ? R0_B1 : (v & 0x20) ? R0_B2 : (v & 0x40) ? R0_B3 : WR_IDLE;
        }
        if (!(v & 0x80) && (v & 7) == 4) {          /* WR1: port A */
            d->reg_temp = v;
            d->a_io = (v >> 3) & 1;
            d->a_mode = (v >> 4) & 3;
            d->wr_seq = (v & 0x40) ? R1_B0 : WR_IDLE;
        }
        if (!(v & 0x80) && (v & 7) == 0) {          /* WR2: port B */
            d->reg_temp = v;
            d->b_io = (v >> 3) & 1;
            d->b_mode = (v >> 4) & 3;
            d->wr_seq = (v & 0x40) ? R2_B0 : WR_IDLE;
        }
        if ((v & 0x80) && (v & 3) == 0) {           /* WR3 */
            d->reg_temp = v;
            if (v & 0x40) {
                d->active = 1;
                d->starting = 1;
                d->next_clk = 0;
            }
            d->wr_seq = (v & 0x08) ? R3_B0 : (v & 0x10) ? R3_B1 : WR_IDLE;
        }
        if ((v & 0x80) && (v & 3) == 1) {           /* WR4 */
            d->reg_temp = v;
            d->mode = (v >> 5) & 3;
            d->wr_seq = (v & 0x04) ? R4_B0 : (v & 0x08) ? R4_B1 : WR_IDLE;
        }
        if ((v & 0xc0) == 0x80 && (v & 7) == 2) {   /* WR5 */
            d->reg_temp = v;
            d->auto_restart = (v >> 5) & 1;
            d->wr_seq = WR_IDLE;
        }
        if ((v & 0x80) && (v & 3) == 3) {           /* WR6: commands */
            d->reg_temp = v;
            d->wr_seq = WR_IDLE;
            switch (v) {
            case 0xc3:  /* reset */
                d->active = 0;
                d->end_of_block_n = 1;
                d->at_least_one = 0;
                d->a_timing = d->b_timing = 1;
                d->prescaler = 0;
                d->auto_restart = 0;
                break;
            case 0xc7: d->a_timing = 1; break;
            case 0xcb: d->b_timing = 1; break;
            case 0xcf:  /* load */
                d->end_of_block_n = 1;
                load(d);
                break;
            case 0xd3:  /* continue */
                d->end_of_block_n = 1;
                d->counter = d->z80_mode ? 0xffff : 0;
                break;
            case 0xbf: d->rd_seq = RD_STATUS; break;
            case 0x8b:
                d->end_of_block_n = 1;
                d->at_least_one = 0;
                break;
            case 0xa7: d->rd_seq = next_read(d, 0); break;
            case 0x87:
                d->active = 1;
                d->starting = 1;
                d->next_clk = 0;
                break;
            case 0x83: d->active = 0; break;
            case 0xbb: d->wr_seq = R6_B0; break;
            default: break;
            }
        }
        break;
    case R0_B0:
        d->a_start = (uint16_t)((d->a_start & 0xff00) | v);
        d->wr_seq = (t & 0x10) ? R0_B1 : (t & 0x20) ? R0_B2 : (t & 0x40) ? R0_B3 : WR_IDLE;
        break;
    case R0_B1:
        d->a_start = (uint16_t)((d->a_start & 0x00ff) | v << 8);
        d->wr_seq = (t & 0x20) ? R0_B2 : (t & 0x40) ? R0_B3 : WR_IDLE;
        break;
    case R0_B2:
        d->len = (uint16_t)((d->len & 0xff00) | v);
        d->wr_seq = (t & 0x40) ? R0_B3 : WR_IDLE;
        break;
    case R0_B3:
        d->len = (uint16_t)((d->len & 0x00ff) | v << 8);
        d->wr_seq = WR_IDLE;
        break;
    case R1_B0:
        d->a_timing = v & 3;
        d->wr_seq = (v & 0x20) ? R1_B1 : WR_IDLE;
        break;
    case R2_B0:
        d->b_timing = v & 3;
        d->wr_seq = (v & 0x20) ? R2_B1 : WR_IDLE;
        break;
    case R2_B1:
        d->prescaler = v;
        d->wr_seq = WR_IDLE;
        break;
    case R3_B0:
        d->wr_seq = (t & 0x10) ? R3_B1 : WR_IDLE;
        break;
    case R4_B0:
        d->b_start = (uint16_t)((d->b_start & 0xff00) | v);
        d->wr_seq = (t & 0x08) ? R4_B1 : WR_IDLE;
        break;
    case R4_B1:
        d->b_start = (uint16_t)((d->b_start & 0x00ff) | v << 8);
        d->wr_seq = WR_IDLE;
        break;
    case R6_B0:
        d->read_mask = v;
        d->rd_seq = next_read(d, 0);
        d->wr_seq = WR_IDLE;
        break;
    default:
        d->wr_seq = WR_IDLE;    /* R1_B1, R3_B1: interrupt control, ignored */
        break;
    }
}

uint8_t nx_dma_read(zxn *n)
{
    nx_dma *d = &n->dma;
    uint16_t a = d->dir_atob ? d->src : d->dst, b = d->dir_atob ? d->dst : d->src;
    uint8_t v;
    switch (d->rd_seq) {
    case RD_STATUS:     v = (uint8_t)(d->end_of_block_n << 5 | 0x1a | d->at_least_one); break;
    case RD_COUNTER_LO: v = (uint8_t)d->counter; break;
    case RD_COUNTER_HI: v = (uint8_t)(d->counter >> 8); break;
    case RD_A_LO:       v = (uint8_t)a; break;
    case RD_A_HI:       v = (uint8_t)(a >> 8); break;
    case RD_B_LO:       v = (uint8_t)b; break;
    default:            v = (uint8_t)(b >> 8); break;
    }
    d->rd_seq = next_read(d, d->rd_seq + 1);
    return v;
}

static unsigned cycles(uint8_t timing)
{
    return timing == 1 ? 3 : timing == 2 ? 2 : 4;
}

static void step(uint16_t *p, uint8_t mode)
{
    if (mode == 0)
        (*p)--;
    else if (mode == 1)
        (*p)++;
}

int nx_dma_run(zxn *n)
{
    nx_dma *d = &n->dma;
    uint64_t start;
    uint8_t v;
    int src_io = d->dir_atob ? d->a_io : d->b_io, dst_io = d->dir_atob ? d->b_io : d->a_io;

    if (!d->active)
        return 0;
    if (n->clk < d->next_clk) {
        if (d->mode == 2)
            return 0;               /* burst: the CPU has the bus meanwhile */
        n->clk = d->next_clk;       /* continuous: hold the bus and wait */
    }
    if (d->starting) {
        n->clk += 2u * n->tick;     /* BUSREQ / BUSACK */
        d->starting = 0;
    }

    start = n->clk;
    v = src_io ? nx_io_read(n, d->src) : nx_mem_read(n, d->src);
    n->clk += cycles(d->dir_atob ? d->a_timing : d->b_timing) * n->tick;
    if (dst_io)
        nx_io_write(n, d->dst, v);
    else
        nx_mem_write(n, d->dst, v);
    n->clk += cycles(d->dir_atob ? d->b_timing : d->a_timing) * n->tick;

    d->counter++;
    step(&d->src, d->dir_atob ? d->a_mode : d->b_mode);
    step(&d->dst, d->dir_atob ? d->b_mode : d->a_mode);
    d->at_least_one = 1;
    d->next_clk = d->prescaler ? start + (uint64_t)d->prescaler * 32 : 0;

    if (d->counter >= d->len) {
        d->end_of_block_n = 0;
        if (d->auto_restart) {
            load(d);
            d->starting = 1;
        } else {
            d->active = 0;
        }
    }
    return 1;
}
