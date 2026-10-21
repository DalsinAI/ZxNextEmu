/*
 * Spec256: the 256-colour Spectrum.
 *
 * A Spec256 game is an ordinary 48K/128K snapshot plus graphics files that
 * give every byte of memory eight extra bit-planes (.GFX and friends). Eight
 * shadow Z80s run alongside the real one, each on its own plane: they take
 * their opcodes from real memory but their data from their plane, so the
 * game's own drawing code paints all eight planes. On screen, pixel colour
 * = the eight plane bits at that pixel, an index into a 256-colour palette.
 *
 * Think of it as one Z80 with a 72-bit data bus: the real CPU alone drives
 * the address bus, timing, I/O and control flow. After each of its
 * instructions the shadow CPUs run the same instruction with PC, SP and the
 * flags (except carry) copied from it ("aligned"), and every memory access a
 * shadow CPU makes goes to the address the real CPU used for the same
 * access, so their pointer registers may hold colour without harm.
 *
 * Files (case does not matter, NAME is the snapshot's base name):
 *   NAME.GFX      planes for RAM 5, RAM 2 and the RAM paged at C000 (3 x 128K)
 *   NAME.GF0-GF7  planes for one RAM page each (128K), overriding .GFX
 *   NAME.GFA/GFB  planes for ROM 0 / ROM 1 of a 128K (GFB, or ROM0.GFX, is
 *                 the 48K BASIC ROM)
 *   NAME.B00-B99  320x200 background pictures, 8 bits per pixel
 *   NAME.CFG      settings, key=value; "NN: addr=VVh" picks background NN
 *                 while the byte at addr is VV
 *   NAME.PAL      a palette (768 bytes RGB) replacing the standard one
 *
 * File handling and the colour rules follow ZX-Poly's Spec256 mode
 * (github.com/raydac/zxpoly), the open reference implementation. ZX-Poly
 * lets shadow CPUs form their own addresses unless told otherwise, which
 * breaks some games (Knight Lore, Three Weeks); sharing the address bus
 * makes those work with no per-game settings.
 */
#ifndef ZXN_SPEC256_H
#define ZXN_SPEC256_H

#include <stddef.h>
#include <stdint.h>

#include "../z80/z80.h"

struct zx;
typedef struct s256 s256;

extern const uint8_t s256_default_palette[768];

/* A Spec256 context. It keeps copies of the 48K ROM and the two 128K ROMs
 * (16K each, any may be NULL) so the machine can switch model to suit the
 * snapshot. NULL if out of memory. */
s256 *s256_new(const uint8_t *rom48, const uint8_t *rom128_0, const uint8_t *rom128_1);
void s256_free(s256 *s);

/* The ROM images (for zx_create) of the model a snapshot of len bytes
 * needs: 1 for 128K, 0 for 48K. */
int s256_snapshot_is_128(size_t len);
void s256_roms(const s256 *s, int is128, const uint8_t *roms[4]);

/* Give the context one companion file. It copies what it keeps. Returns 0
 * if the file was used, -1 if its type is not a Spec256 one. */
int s256_add_file(s256 *s, const char *name, const uint8_t *data, size_t len);
/* Forget every companion file (before loading another game). */
void s256_clear_files(s256 *s);

/* The snapshot's file name, before s256_start: a few games are known by
 * it to want colours 0 and 0xFF left alone (no Paper00InkFF). */
void s256_set_name(s256 *s, const char *snapshot_file);
/* Hook the context into a machine (it wraps m->bus). Call after zx_create,
 * which forgets any earlier attachment. */
void s256_attach(s256 *s, struct zx *m);
/* After the snapshot is loaded: build the planes from the companion files
 * (the snapshot's own bytes for anything they do not cover) and start the
 * shadow CPUs in step with the real one. */
void s256_start(s256 *s);

/* Tracing, for the auto-colouring tool: after s256_start, plane 0 of every
 * mapped byte holds the low byte of its address and plane 1 the high byte
 * (bitmap bytes get 0), so a value copied, ORed or XORed onto blank screen
 * carries the address it came from. Each write to the bitmap (4000-57FF)
 * calls fn with the address, the value written and that source address (0
 * if none); the bitmap's planes are then cleared again. NULL stops it. */
typedef void (*s256_trace_fn)(void *user, uint16_t addr, uint8_t value, uint16_t source);
void s256_trace(s256 *s, s256_trace_fn fn, void *user);

/* Used by the machine (zx.c). */
void s256_step(s256 *s);
int s256_interrupt(s256 *s);
void s256_render(s256 *s);
size_t s256_state_size(const s256 *s);
void s256_state_save(const s256 *s, uint8_t *p);
void s256_state_load(s256 *s, const uint8_t *p);

#endif
