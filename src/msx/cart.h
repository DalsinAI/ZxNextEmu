/* MSX ROM cartridges: plain ROMs and the common mappers. */
#ifndef MSX_CART_H
#define MSX_CART_H

#include <stdint.h>

#include "scc.h"

typedef enum {
    CART_NONE,
    CART_PLAIN,                /* up to 64K, no mapper */
    CART_ASCII8,
    CART_ASCII16,
    CART_KONAMI,               /* Konami without SCC */
    CART_KONAMI_SCC
} cart_type;

typedef struct msx_cart {
    uint8_t *rom;              /* owned; a power of two of 8K pages */
    uint32_t size;             /* bytes, rounded up */
    uint32_t pages;            /* 8K pages */
    uint8_t type;
    uint8_t bank[4];           /* 8K banks at 4000, 6000, 8000, A000 */
    uint16_t base;             /* plain ROMs: the address of byte 0 */
    uint8_t scc_on;
    scc_chip scc;
} msx_cart;

/* The type from the name ("[ascii8]" and so on) or by looking at the
 * code; copies the data. 0 on success. */
int cart_load(msx_cart *c, const uint8_t *data, uint32_t len, const char *name);
void cart_free(msx_cart *c);
void cart_reset(msx_cart *c);
/* The 8K block the CPU sees at bank b (0..7) of the slot, or NULL when
 * reads there must go through cart_read. */
const uint8_t *cart_bank(const msx_cart *c, int b);
uint8_t cart_read(msx_cart *c, uint16_t addr);
/* Returns 1 when the memory map changed. */
int cart_write(msx_cart *c, uint16_t addr, uint8_t val);

const char *cart_type_name(int type);

#endif
