/*
 * zxm: the one machine interface ZX Cradle drives (docs/cradle/design.md).
 *
 * The host (the JavaScript runtime, or a test) never sees a machine's own
 * structures: it looks a machine up by id, allocates zxm_size() bytes,
 * creates it from ROM images, runs it a frame at a time and reads the
 * picture and sound straight out of the instance. The core does no file
 * I/O: ROMs, tapes and snapshots arrive as bytes, SD cards as block
 * callbacks, serial devices as byte callbacks.
 *
 * This is the same C that builds natively and to zxcore.wasm.
 */
#ifndef ZXM_H
#define ZXM_H

#include <stddef.h>
#include <stdint.h>

typedef struct zxm zxm;

typedef enum {
    ZXM_RGB333 = 1               /* uint16_t per pixel, bits 8..0 = RRRGGGBBB */
} zxm_pixfmt;

typedef enum {
    ZXM_POWER_ON,                /* as if switched off and on */
    ZXM_RESET,                   /* the reset button */
    ZXM_NMI                      /* the NMI button (Multiface, Next drive) */
} zxm_reset_kind;

typedef enum {
    ZXM_SLOT_TAPE = 1,           /* bytes: .tap, .tzx */
    ZXM_SLOT_SNAPSHOT,           /* bytes, loaded once: .sna, .z80 */
    ZXM_SLOT_SD,                 /* blocks */
    ZXM_SLOT_SERIAL,             /* a serial device on a UART */
    ZXM_SLOT_DISK,               /* a floppy drive: bytes or blocks */
    ZXM_SLOT_CARTRIDGE,          /* a ROM cartridge */
    ZXM_SLOT_HARDDISK,           /* a hard disk */
    ZXM_SLOT_COMPANION           /* bytes, kept: files a snapshot goes with
                                    (Spec256 graphics); insert them first,
                                    eject clears them all */
} zxm_slot_kind;

typedef struct zxm_slot {
    const char *id;              /* "tape", "snapshot", "sd0", "uart0" ... */
    const char *name;            /* for people: "Tape", "SD card" */
    zxm_slot_kind kind;
    const char *exts;            /* file types it takes: ".tap .tzx" */
} zxm_slot;

typedef enum {
    ZXM_IN_KEYBOARD = 1,         /* 8x5 Spectrum matrix (+ Next extra keys) */
    ZXM_IN_KEMPSTON,             /* Kempston joystick */
    ZXM_IN_MOUSE,                /* Kempston mouse */
    ZXM_IN_ORIC_KEYBOARD,        /* 8x8 Oric matrix: ZXM_EV_KEY a = row 0..7, b = column 0..7 */
    ZXM_IN_SAM_KEYBOARD,         /* SAM Coupe matrix */
    ZXM_IN_MSX_KEYBOARD,         /* MSX matrix */
    ZXM_IN_JOYSTICK              /* a general joystick (ZXM_EV_JOYSTICK) */
} zxm_input_kind;

typedef struct zxm_input_desc {
    const char *id;
    zxm_input_kind kind;
} zxm_input_desc;

typedef struct zxm_info {
    const char *id;              /* "zx48", "zx128", "next", "scorpion", "atm2" */
    const char *name;            /* "ZX Spectrum 128K" */
    uint32_t cpu_hz, frame_t;    /* nominal clock, T-states per frame */
    uint16_t fb_w, fb_h;         /* framebuffer size in pixels */
    zxm_pixfmt fb_format;
    uint32_t audio_rate;         /* stereo int16 samples per second */
    const char *const *roms;     /* ROM file names in order, NULL-terminated */
    const zxm_slot *slots;       /* media slots, ending with a NULL id */
    const zxm_input_desc *inputs;     /* inputs, ending with a NULL id */
    const char *cpu;             /* "z80", "6502": how zxm_regs reads */
    uint32_t rom_optional;       /* bit i: roms[i] may be left out */
} zxm_info;

/* ROM images in the order zxm_info.roms names them (16K each on the
 * Spectrums and Orics). */
typedef struct zxm_rom_set {
    const uint8_t *rom[4];
    uint32_t len[4];
} zxm_rom_set;

typedef enum {
    ZXM_EV_KEY = 1,              /* a = row 0..7, b = bit 0..4, down */
    ZXM_EV_KEMPSTON,             /* a = state: bit 0 right, 1 left, 2 down, 3 up, 4 fire */
    ZXM_EV_MOUSE,                /* dx, dy (down positive), dz wheel; a = buttons 1 L, 2 R, 4 M */
    ZXM_EV_RELEASE_ALL,          /* every key up */
    ZXM_EV_JOYSTICK              /* reserved for the MSX joystick */
} zxm_event_kind;

typedef struct zxm_input_event {
    uint32_t t;                  /* T-state in the frame; today applied at the next frame start */
    uint8_t kind, a, b, down;
    int16_t dx, dy, dz;
} zxm_input_event;

struct zx_blockdev;
struct zx_serial;

typedef enum {
    ZXM_MEDIA_BYTES = 1,         /* data/len: the core copies what it keeps */
    ZXM_MEDIA_BLOCKS,            /* blocks: the machine owns it from now on */
    ZXM_MEDIA_SERIAL             /* serial: the machine owns it from now on */
} zxm_media_kind;

typedef struct zxm_media {
    zxm_media_kind kind;
    const char *name;            /* file name; its extension picks the format */
    const uint8_t *data;
    uint32_t len;
    struct zx_blockdev *blocks;
    struct zx_serial *serial;
} zxm_media;

/* Debugging: CPU registers, memory as the CPU sees it, frame count.
 * A 6502 (zxm_info.cpu "6502") uses af = A << 8 | P, bc = X, de = Y,
 * sp = 0x100 | S and pc; the other fields are zero. */
typedef struct zxm_regs {
    uint16_t af, bc, de, hl, af_, bc_, de_, hl_, ix, iy, sp, pc, memptr;
    uint8_t i, r, iff1, iff2, im, halted;
} zxm_regs;

typedef struct zxm_debug {
    void (*get_regs)(const zxm *m, zxm_regs *out);
    void (*set_regs)(zxm *m, const zxm_regs *in);
    uint8_t (*peek)(const zxm *m, uint16_t addr);
    void (*poke)(zxm *m, uint16_t addr, uint8_t v);
    uint64_t (*frames)(const zxm *m);
} zxm_debug;

/* Machines: by id, or the i'th (NULL past the end). */
const zxm_info *zxm_lookup(const char *id);
const zxm_info *zxm_machine(int i);

/* The caller allocates zxm_size() bytes (aligned for any type) and passes
 * them to zxm_create. A Next also allocates its 2 MB of RAM inside create;
 * zxm_destroy frees it and closes attached media. */
size_t zxm_size(const zxm_info *info);
int    zxm_create(zxm *m, const zxm_info *info, const zxm_rom_set *roms);
void   zxm_destroy(zxm *m);
const zxm_info *zxm_info_of(const zxm *m);

void   zxm_reset(zxm *m, zxm_reset_kind kind);
void   zxm_run_frame(zxm *m);                       /* exactly one video frame */

/* The picture: fb_w x fb_h, pitch in pixels (a Next is 640 wide: each
 * classic pixel is two). */
const void *zxm_framebuffer(const zxm *m, int *pitch);
/* This frame's sound: *samples stereo pairs, interleaved L R. */
const int16_t *zxm_audio(const zxm *m, int *samples);

void   zxm_input(zxm *m, const zxm_input_event *ev);
/* Type ASCII text, one key every few frames, after a delay in frames. */
void   zxm_type(zxm *m, const char *text, int delay_frames);
int    zxm_typing(const zxm *m);

/* Interfaces for a classic machine's expansion port: "divmmc" (esxDOS and
 * an SD card), "mb03" (the MB03+ Ultimate: divMMC with two SD cards,
 * TurboSound, SpecDrum/Covox/SounDrive, Kempston mouse, Timex screens). */
typedef struct zxm_interface {
    const char *id;
    const char *name;
    const char *const *roms;     /* ROM file names, NULL-terminated (8K each) */
    const zxm_slot *slots;       /* the slots it adds, ending with a NULL id */
    const zxm_input_desc *inputs;     /* the inputs it adds, ending with a NULL id */
} zxm_interface;

/* Interfaces: by id, or the i'th (NULL past the end). */
const zxm_interface *zxm_interface_lookup(const char *id);
const zxm_interface *zxm_interface_at(int i);
/* Plug an interface in after zxm_create and before the first frame, with
 * its ROMs in the order zxm_interface.roms names them. One per machine; it
 * stays through resets. 0 on success, -1 for the Next or a bad ROM. */
int    zxm_attach(zxm *m, const char *id, const zxm_rom_set *roms);
/* This instance's media slots and inputs: the machine's, then its
 * interface's. Use these rather than zxm_info's once an interface is in. */
const zxm_slot *zxm_slots(const zxm *m);
const zxm_input_desc *zxm_inputs(const zxm *m);

/* 0 on success. A snapshot loads into the running machine and is not kept. */
int    zxm_media_insert(zxm *m, const char *slot, const zxm_media *media);
int    zxm_media_eject(zxm *m, const char *slot);
/* 1 while the slot's medium is working (a tape playing), for an LED. */
int    zxm_media_active(const zxm *m, const char *slot);

/* Complete state: CPU, memory, devices, the typing queue, the tape's play
 * position. Media stay attached and are not part of it. A state loads into
 * a machine of the same id built by the same core. */
size_t zxm_state_size(const zxm *m);
int    zxm_state_save(const zxm *m, void *buf, size_t len);
int    zxm_state_load(zxm *m, const void *buf, size_t len);

const zxm_debug *zxm_debugger(zxm *m);

#endif
