/* .SNA and .Z80 snapshot loading. */
#include "zx.h"

#include <string.h>

static void set_paging(zx *m, uint8_t p7ffd, uint8_t p1ffd)
{
    /* Go through the port so the memory map and lock follow the model. */
    m->paging_locked = 0;
    m->port_1ffd = p1ffd;
    m->bus.io_write(m->bus.ctx, 0x7ffd, p7ffd);
    if (m->model == ZX_PLUS2A || m->model == ZX_PLUS3)
        m->bus.io_write(m->bus.ctx, 0x1ffd, p1ffd);
    m->t = 0;
}

static int bank_for(zx *m, uint16_t addr)
{
    if (addr < 0x8000)
        return 5;
    if (addr < 0xc000)
        return 2;
    return m->model == ZX_48K ? 0 : (m->port_7ffd & 7);
}

int zx_load_sna(zx *m, const uint8_t *d, size_t len)
{
    z80 *c = &m->cpu;
    int is128 = len > 49179;
    int i, b;
    size_t pos;

    if (len < 49179)
        return -1;
    if (is128 && m->model == ZX_48K)
        return -1;
    zx_reset(m);
    c->i = d[0];
    c->l_ = d[1]; c->h_ = d[2]; c->e_ = d[3]; c->d_ = d[4];
    c->c_ = d[5]; c->b_ = d[6]; c->f_ = d[7]; c->a_ = d[8];
    c->l = d[9]; c->h = d[10]; c->e = d[11]; c->d = d[12];
    c->c = d[13]; c->b = d[14];
    c->iy = (uint16_t)(d[15] | d[16] << 8);
    c->ix = (uint16_t)(d[17] | d[18] << 8);
    c->iff1 = c->iff2 = (d[19] >> 2) & 1;
    c->r = d[20];
    c->f = d[21]; c->a = d[22];
    c->sp = (uint16_t)(d[23] | d[24] << 8);
    c->im = d[25] & 3;
    m->border = d[26] & 7;

    if (!is128) {
        memcpy(m->ram[5], d + 27, 0x4000);
        memcpy(m->ram[2], d + 27 + 0x4000, 0x4000);
        memcpy(m->ram[m->model == ZX_48K ? 0 : 0], d + 27 + 0x8000, 0x4000);
        if (m->model != ZX_48K)
            set_paging(m, 0x30, 0); /* 48K BASIC ROM, paging locked */
        /* PC is on the stack (the snapshot was taken inside an interrupt). */
        c->pc = (uint16_t)(zx_peek(m, c->sp) | zx_peek(m, (uint16_t)(c->sp + 1)) << 8);
        c->sp = (uint16_t)(c->sp + 2);
        return 0;
    }

    if (len < 49183)
        return -1;
    c->pc = (uint16_t)(d[49179] | d[49180] << 8);
    set_paging(m, d[49181], 0);
    memcpy(m->ram[5], d + 27, 0x4000);
    memcpy(m->ram[2], d + 27 + 0x4000, 0x4000);
    memcpy(m->ram[m->port_7ffd & 7], d + 27 + 0x8000, 0x4000);
    pos = 49183;
    for (b = 0; b < 8; b++) {
        if (b == 5 || b == 2 || b == (m->port_7ffd & 7))
            continue;
        if (pos + 0x4000 > len)
            return -1;
        memcpy(m->ram[b], d + pos, 0x4000);
        pos += 0x4000;
    }
    (void)i;
    return 0;
}

/* Decompress a .Z80 block (ED ED count byte). Returns bytes consumed or -1. */
static long unpack(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen, int v1)
{
    size_t i = 0, o = 0;
    while (o < dlen && i < slen) {
        if (v1 && i + 4 <= slen && src[i] == 0 && src[i + 1] == 0xed && src[i + 2] == 0xed && src[i + 3] == 0)
            break;
        if (i + 3 < slen && src[i] == 0xed && src[i + 1] == 0xed) {
            unsigned n = src[i + 2];
            uint8_t v = src[i + 3];
            while (n-- && o < dlen)
                dst[o++] = v;
            i += 4;
        } else {
            dst[o++] = src[i++];
        }
    }
    return (long)i;
}

int zx_load_z80(zx *m, const uint8_t *d, size_t len)
{
    z80 *c = &m->cpu;
    uint8_t f12;
    size_t hdr, pos;
    int ver, is128 = 0;

    if (len < 30)
        return -1;
    zx_reset(m);
    c->a = d[0]; c->f = d[1];
    c->c = d[2]; c->b = d[3];
    c->l = d[4]; c->h = d[5];
    c->pc = (uint16_t)(d[6] | d[7] << 8);
    c->sp = (uint16_t)(d[8] | d[9] << 8);
    c->i = d[10];
    f12 = d[12] == 0xff ? 1 : d[12];
    c->r = (uint8_t)((d[11] & 0x7f) | (f12 & 1) << 7);
    m->border = (f12 >> 1) & 7;
    c->e = d[13]; c->d = d[14];
    c->c_ = d[15]; c->b_ = d[16]; c->e_ = d[17]; c->d_ = d[18];
    c->l_ = d[19]; c->h_ = d[20]; c->a_ = d[21]; c->f_ = d[22];
    c->iy = (uint16_t)(d[23] | d[24] << 8);
    c->ix = (uint16_t)(d[25] | d[26] << 8);
    c->iff1 = d[27] ? 1 : 0;
    c->iff2 = d[28] ? 1 : 0;
    c->im = d[29] & 3;

    if (c->pc) {
        /* Version 1: one 48K block, maybe compressed. */
        uint8_t ram[0xc000];
        if (f12 & 0x20)
            unpack(d + 30, len - 30, ram, sizeof ram, 1);
        else if (len >= 30 + sizeof ram)
            memcpy(ram, d + 30, sizeof ram);
        else
            return -1;
        if (m->model != ZX_48K)
            set_paging(m, 0x30, 0);
        memcpy(m->ram[5], ram, 0x4000);
        memcpy(m->ram[2], ram + 0x4000, 0x4000);
        memcpy(m->ram[bank_for(m, 0xc000)], ram + 0x8000, 0x4000);
        return 0;
    }

    if (len < 32)
        return -1;
    hdr = (size_t)(d[30] | d[31] << 8);
    if (len < 32 + hdr)
        return -1;
    ver = hdr == 23 ? 2 : 3;
    c->pc = (uint16_t)(d[32] | d[33] << 8);
    {
        uint8_t hw = d[34];
        if (ver == 2)
            is128 = hw >= 3;
        else
            is128 = hw >= 4;
        if (hw == 7 || hw == 8 || hw == 9 || hw == 12 || hw == 13)
            is128 = 1;
    }
    if (is128 && m->model == ZX_48K)
        return -1;
    if (is128)
        set_paging(m, d[35], (ver == 3 && hdr >= 55) ? d[86] : 0);
    else if (m->model != ZX_48K)
        set_paging(m, 0x30, 0);
    if (is128) {
        int r;
        for (r = 0; r < 16; r++) {
            m->ay.sel = (uint8_t)r;
            ay_write(&m->ay, d[39 + r]);
        }
        m->ay.sel = d[38];
    }

    pos = 32 + hdr;
    while (pos + 3 <= len) {
        size_t blen = (size_t)(d[pos] | d[pos + 1] << 8);
        int page = d[pos + 2], bank = -1;
        const uint8_t *src = d + pos + 3;
        size_t slen = blen == 0xffff ? 0x4000 : blen;
        if (pos + 3 + slen > len)
            return -1;
        if (is128) {
            if (page >= 3 && page <= 10)
                bank = page - 3;
        } else {
            if (page == 8) bank = 5;
            else if (page == 4) bank = 2;
            else if (page == 5) bank = 0;
        }
        if (bank >= 0) {
            if (blen == 0xffff)
                memcpy(m->ram[bank], src, 0x4000);
            else
                unpack(src, slen, m->ram[bank], 0x4000, 0);
        }
        pos += 3 + slen;
    }
    return 0;
}
