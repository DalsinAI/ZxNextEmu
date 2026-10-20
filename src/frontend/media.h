/* Front-end helpers shared by every front end: files and key names. */
#ifndef ZXN_MEDIA_H
#define ZXN_MEDIA_H

#include "../zx/zx.h"

/* Load a .sna/.z80 snapshot or insert a .tap/.tzx tape, by extension.
 * Returns 0 on success. */
int media_load(zx *m, const char *path);

/* Read a whole file; caller frees. */
uint8_t *media_read_file(const char *path, size_t *len);

#endif
