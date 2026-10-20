/* Front-end helpers shared by every front end: files and key names. */
#ifndef ZXN_MEDIA_H
#define ZXN_MEDIA_H

#include "../zx/zx.h"

/* Load a .sna/.z80 snapshot or insert a .tap/.tzx tape, by extension.
 * Returns 0 on success. */
int media_load(zx *m, const char *path);

/* Spectrum key for an ASCII character: row/bit, plus whether CAPS SHIFT
 * (row 0 bit 0) or SYMBOL SHIFT (row 7 bit 1) must be held.
 * Returns 0 if the character has no key. */
int media_char_key(int ch, int *row, int *bit, int *caps, int *sym);

/* Queue text to be typed into the machine, one key every few frames. */
void media_type(const char *text);
/* Wait this many frames before typing (lets the ROM finish starting up). */
void media_type_delay(int frames);
/* Call once per frame before zx_run_frame to feed queued keys. */
void media_type_frame(zx *m);

/* Read a whole file; caller frees. */
uint8_t *media_read_file(const char *path, size_t *len);

#endif
