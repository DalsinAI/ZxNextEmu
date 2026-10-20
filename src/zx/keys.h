/* Typing text into a machine: ASCII to the Spectrum keyboard matrix. */
#ifndef ZXN_KEYS_H
#define ZXN_KEYS_H

#include "zx.h"

/* Spectrum key for an ASCII character: row/bit, plus whether CAPS SHIFT
 * (row 0 bit 0) or SYMBOL SHIFT (row 7 bit 1) must be held.
 * Returns 0 if the character has no key. */
int zx_char_key(int ch, int *row, int *bit, int *caps, int *sym);

/* A queue of text typed one key every few frames. Each machine (or front
 * end) keeps its own; it is plain data, so it can be part of saved state. */
typedef struct zx_typer {
    char buf[1024];
    uint32_t pos, len;
    int32_t phase, wait;
} zx_typer;

/* Replace the queue with text (cut at 1024 characters). */
void zx_type(zx_typer *ty, const char *text);
/* Wait this many frames before typing (lets the ROM finish starting up). */
void zx_type_delay(zx_typer *ty, int frames);
/* Call once per frame before zx_run_frame to press and release keys. */
void zx_type_frame(zx_typer *ty, zx *m);
int zx_typing(const zx_typer *ty);

#endif
