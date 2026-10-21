/*
 * MGT SAM Coupé: Z80 at 6 MHz, the ASIC (paging, four screen modes,
 * contention, interrupts), SAA1099 sound, keyboard, tape, and two VL1772
 * floppy drives.
 *
 * Timing reference: SimCoupé (Simon Owen, Dave Laundon), whose contention
 * and interrupt timings were measured against real machines, and the SAM
 * Technical Manual. All times are CPU T-states from the start of the frame
 * (the frame interrupt), 384 to a line and 312 lines to a frame.
 */
#ifndef ZX_SAM_H
#define ZX_SAM_H

#include <stdint.h>
#include <stddef.h>

#include "../z80/z80.h"
#include "../devices/saa1099.h"
#include "../devices/wd177x.h"

#define SAM_CPU_HZ   6000000
#define SAM_LINE_T   384
#define SAM_LINES    312
#define SAM_FRAME_T  (SAM_LINE_T * SAM_LINES)
#define SAM_PAGES    32                 /* 512K of internal RAM */

/* The picture: 256x192 (512 wide in mode 3) with 32 lines of border above
 * and below and 32 low-res pixels each side; 640x256, each low-res pixel
 * two wide. */
#define SAM_FB_W     640
#define SAM_FB_H     256
#define SAM_AUDIO_RATE 44100

struct zx_tape;

typedef struct sam {
    z80 cpu;
    z80_bus bus;
    uint32_t t;                         /* T-state in the frame */
    uint64_t frames;

    uint8_t rom[2][0x4000];
    uint8_t ram[SAM_PAGES][0x4000];
    const uint8_t *rd[4];               /* the four 16K sections */
    uint8_t *wr[4];                     /* NULL: ROM or nothing there */
    int8_t page[4];                     /* RAM page in each section, -1 if none */

    /* ASIC */
    uint8_t lmpr, hmpr, vmpr, lepr, hepr, border, line;
    uint8_t clut[16];
    uint8_t attr, lpen, hpen;
    uint8_t keys[9];                    /* rows 0..7: 8 keys each; row 8: 5 */
    uint8_t kempston;
    uint64_t asic_ready;                /* the ASIC ignores its ports until then */
    uint64_t midi_at;                   /* a MIDI byte's interrupt, 0 for none */

    /* Video, drawn up to the current T-state */
    uint16_t screen[SAM_FB_H][SAM_FB_W];
    uint32_t video_cell;
    uint8_t flash;

    /* Sound */
    saa1099 saa;
    uint32_t saa_frac;
    uint8_t speaker, mic, ear;
    int16_t audio[SAM_AUDIO_RATE / 25 * 2];
    int audio_samples;
    uint32_t audio_t;
    uint64_t audio_frac;
    int32_t beeper_acc;

    /* Disk drives 1 and 2 */
    wd177x fdc[2];
    struct fd_disk *disk[2];

    struct zx_tape *tape;
    uint16_t hook_edge, hook_load_end;  /* ROM tape loader addresses */
} sam;

/* rom is the 32K ROM (ROM 0 then ROM 1). No file I/O. */
int sam_create(sam *s, const uint8_t *rom, size_t len);
void sam_power_on(sam *s);
void sam_reset(sam *s);
void sam_run_frame(sam *s);
void sam_free(sam *s);

/* Keyboard matrix: row 0..8, bit 0..7 (row 8 has bits 0..4). */
void sam_key(sam *s, int row, int bit, int down);
/* The SAM key for an ASCII character, with SHIFT and SYMBOL; 0 if none. */
int sam_char_key(int ch, int *row, int *bit, int *shift, int *sym);

uint8_t sam_peek(const sam *s, uint16_t addr);
void sam_poke(sam *s, uint16_t addr, uint8_t v);

/* Drive 0 or 1 (SAM drives 1 and 2); the machine owns the disk from now
 * on and closes the one it replaces. NULL ejects. */
void sam_insert_disk(sam *s, int drive, struct fd_disk *d);

size_t sam_state_size(const sam *s);
int sam_state_save(const sam *s, void *buf, size_t len);
int sam_state_load(sam *s, const void *buf, size_t len);

#endif
