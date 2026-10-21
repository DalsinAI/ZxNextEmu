#include "cart.h"

#include <stdlib.h>
#include <string.h>

/* What an empty part of the slot reads as. */
static uint8_t ff_block[0x2000];
static int ff_ready;

static const uint8_t *unmapped(void)
{
    if (!ff_ready) {
        memset(ff_block, 0xff, sizeof ff_block);
        ff_ready = 1;
    }
    return ff_block;
}

const char *cart_type_name(int type)
{
    static const char *const names[] = { "none", "plain", "ASCII8", "ASCII16", "Konami", "Konami SCC" };
    return type >= 0 && type <= CART_KONAMI_SCC ? names[type] : "?";
}

static int name_has(const char *name, const char *word)
{
    size_t n, w = strlen(word), i, j;
    if (!name)
        return 0;
    n = strlen(name);
    for (i = 0; i + w <= n; i++) {
        for (j = 0; j < w; j++) {
            char c = name[i + j];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != word[j])
                break;
        }
        if (j == w)
            return 1;
    }
    return 0;
}

/* Guess the mapper from the bank switch writes in the code
 * (LD (nnnn),A), the way most MSX emulators do. */
static int guess(const uint8_t *d, uint32_t len)
{
    uint32_t i;
    int score[6] = { 0, 0, 0, 0, 0, 0 }, best = CART_ASCII8, t;
    for (i = 0; i + 2 < len; i++) {
        uint16_t a;
        if (d[i] != 0x32)
            continue;
        a = (uint16_t)(d[i + 1] | d[i + 2] << 8);
        switch (a) {
        case 0x5000: case 0x9000: case 0xb000:
            score[CART_KONAMI_SCC]++;
            break;
        case 0x4000: case 0x8000: case 0xa000:
            score[CART_KONAMI]++;
            break;
        case 0x6800: case 0x7800:
            score[CART_ASCII8]++;
            break;
        case 0x6000:
            score[CART_KONAMI]++;
            score[CART_ASCII8]++;
            score[CART_ASCII16]++;
            break;
        case 0x7000:
            score[CART_KONAMI_SCC]++;
            score[CART_ASCII8]++;
            score[CART_ASCII16]++;
            break;
        case 0x77ff:
            score[CART_ASCII16]++;
            break;
        }
    }
    /* ASCII8 wins ties, then ASCII16: the usual tie-break. */
    score[CART_ASCII8]++;
    for (t = CART_ASCII8; t <= CART_KONAMI_SCC; t++)
        if (score[t] > score[best])
            best = t;
    return best;
}

int cart_load(msx_cart *c, const uint8_t *data, uint32_t len, const char *name)
{
    uint32_t size = 0x2000;
    if (!data || !len || len > 0x400000)
        return -1;
    while (size < len)
        size <<= 1;
    cart_free(c);
    c->rom = malloc(size);
    if (!c->rom)
        return -1;
    memset(c->rom, 0xff, size);
    memcpy(c->rom, data, len);
    c->size = size;
    c->pages = size >> 13;

    if (name_has(name, "[ascii8]"))
        c->type = CART_ASCII8;
    else if (name_has(name, "[ascii16]"))
        c->type = CART_ASCII16;
    else if (name_has(name, "[konamiscc]") || name_has(name, "[scc]"))
        c->type = CART_KONAMI_SCC;
    else if (name_has(name, "[konami]"))
        c->type = CART_KONAMI;
    else if (name_has(name, "[plain]") || len <= 0x10000)
        c->type = CART_PLAIN;
    else
        c->type = (uint8_t)guess(data, len);

    if (c->type == CART_PLAIN) {
        /* Where the ROM starts: by its header ("AB" and the start
         * address), else 4000. 64K images start at 0000. */
        const uint8_t *h = c->rom;
        if (size >= 0x10000) {
            c->base = 0x0000;
        } else if (h[0] == 'A' && h[1] == 'B') {
            uint16_t init = (uint16_t)(h[2] | h[3] << 8), text = (uint16_t)(h[8] | h[9] << 8);
            if (!init && text >= 0x8000)
                c->base = 0x8000;           /* a BASIC program */
            else if (size <= 0x4000 && init >= 0x8000 && init < 0xc000)
                c->base = 0x8000;
            else if (init && init < 0x4000 && size <= 0x8000)
                c->base = 0x0000;
            else
                c->base = 0x4000;
        } else if (size == 0x8000 && h[0x4000] == 'A' && h[0x4001] == 'B') {
            c->base = 0x0000;
        } else {
            c->base = 0x4000;
        }
    }
    cart_reset(c);
    return 0;
}

void cart_free(msx_cart *c)
{
    free(c->rom);
    memset(c, 0, sizeof *c);
}

void cart_reset(msx_cart *c)
{
    int i;
    for (i = 0; i < 4; i++)
        c->bank[i] = (uint8_t)i;
    if (c->type == CART_ASCII8 || c->type == CART_ASCII16)
        memset(c->bank, 0, sizeof c->bank);
    if (c->type == CART_ASCII16) {
        c->bank[0] = 0; c->bank[1] = 1; c->bank[2] = 0; c->bank[3] = 1;
    }
    c->scc_on = 0;
    scc_reset(&c->scc);
}

const uint8_t *cart_bank(const msx_cart *c, int b)
{
    uint32_t page;
    if (!c->rom)
        return unmapped();
    if (c->type == CART_PLAIN) {
        /* Mirrored through the whole 64K, as incompletely decoded ROMs are. */
        uint32_t off = ((uint32_t)b * 0x2000 - c->base) & 0xffff;
        return c->rom + (off & (c->size - 1));
    }
    if (b < 2 || b > 5)
        return unmapped();
    if (c->type == CART_KONAMI_SCC && b == 4 && c->scc_on)
        return NULL;            /* 9800-98FF are the SCC */
    if (c->type == CART_ASCII16)
        page = (uint32_t)c->bank[(b - 2) & 2] * 2 + ((b - 2) & 1);
    else
        page = c->bank[b - 2];
    return c->rom + ((page & (c->pages - 1)) << 13);
}

uint8_t cart_read(msx_cart *c, uint16_t addr)
{
    if (c->type == CART_KONAMI_SCC && c->scc_on && (addr & 0xf800) == 0x9800)
        return scc_read(&c->scc, (uint8_t)addr);
    {
        const uint8_t *p = cart_bank(c, addr >> 13);
        if (!p)
            p = c->rom + ((c->bank[2] & (c->pages - 1)) << 13);
        return p[addr & 0x1fff];
    }
}

int cart_write(msx_cart *c, uint16_t addr, uint8_t val)
{
    switch (c->type) {
    case CART_ASCII8:
        if (addr >= 0x6000 && addr < 0x8000) {
            c->bank[(addr >> 11) & 3] = val;
            return 1;
        }
        break;
    case CART_ASCII16:
        if (addr >= 0x6000 && addr < 0x6800) {
            c->bank[0] = val;
            return 1;
        }
        if (addr >= 0x7000 && addr < 0x7800) {
            c->bank[2] = val;
            return 1;
        }
        break;
    case CART_KONAMI:
        if (addr >= 0x6000 && addr < 0xc000) {
            c->bank[(addr - 0x4000) >> 13] = val;
            return 1;
        }
        break;
    case CART_KONAMI_SCC:
        if ((addr & 0xf800) == 0x9800 && c->scc_on) {
            scc_write(&c->scc, (uint8_t)addr, val);
            return 0;
        }
        if ((addr & 0x1800) == 0x1000 && addr >= 0x4000 && addr < 0xc000) {
            int b = (addr - 0x4000) >> 13;
            c->bank[b] = val;
            if (b == 2)
                c->scc_on = (val & 0x3f) == 0x3f;
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}
