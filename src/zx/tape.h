/*
 * Tape deck: .TAP and .TZX played as real EAR pulses with exact T-state
 * timing, plus an optional instant-load trap on the ROM's LD-BYTES routine.
 */
#ifndef ZXN_TAPE_H
#define ZXN_TAPE_H

#include <stddef.h>
#include <stdint.h>

struct zx;
typedef struct zx_tape zx_tape;

zx_tape *zx_tape_open(const uint8_t *data, size_t len, int is_tzx);
void zx_tape_free(zx_tape *t);

void zx_tape_play(zx_tape *t, uint64_t now);
void zx_tape_stop(zx_tape *t);
int zx_tape_playing(const zx_tape *t);
/* Allow the instant-load trap (default on). */
void zx_tape_set_fast(zx_tape *t, int fast);

/* Advance the pulse stream to absolute machine time `now` (T-states). */
void zx_tape_advance(zx_tape *t, struct zx *m, uint64_t now);
/* Called at PC 0x0556 in the BASIC ROM. Returns 1 if it loaded a block. */
int zx_tape_trap(zx_tape *t, struct zx *m);
/* The program read the EAR port from RAM: a custom loader is running, so
 * start real-time playback if the trap was in use. */
void zx_tape_custom_loader(zx_tape *t);

/* Play position, for machine state (zx_state_save). Loading fails if the
 * tape is not the one the state was saved with. */
size_t zx_tape_state_size(void);
void zx_tape_state_save(const zx_tape *t, uint8_t *out);
int zx_tape_state_load(zx_tape *t, const uint8_t *in);

#endif
