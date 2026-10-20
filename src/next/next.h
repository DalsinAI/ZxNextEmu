/*
 * ZX Spectrum Next (TBBlue) machine.
 *
 * Modelled on the official FPGA sources (cores/zxnext in the KS2 repo):
 * zxnext.vhd for memory decode, ports, nextregs and interrupts, and the
 * video/, device/ and audio/ entities for the rest. Names in comments
 * (nr_08, port_7ffd_reg, sram_pre_override ...) are the VHDL signal names,
 * so a reader can check any behaviour against the hardware description.
 *
 * Time is counted in 28 MHz master clock ticks from the start of the frame,
 * which is the ULA interrupt. A 3.5 MHz T-state is 8 ticks, 7 MHz is 4,
 * 14 MHz is 2 and 28 MHz is 1. The 7 MHz pixel clock is 4 ticks.
 */
#ifndef ZXN_NEXT_H
#define ZXN_NEXT_H

#include <stdint.h>
#include <stdio.h>

#include "../zx/ay.h"

struct zx;
typedef struct nx_sd nx_sd;

#define NX_SRAM_SIZE  (2u << 20)   /* 2 MB: expanded Next */
#define NX_SPRITES    128
#define NX_W          640          /* framebuffer: 320 wide at 14 MHz pixels */
#define NX_H          256

typedef struct nx_slot {
    uint8_t *rd;          /* 8K read window */
    uint8_t *wr;          /* 8K write window, or the discard page */
    uint8_t wait;         /* read waits at 28 MHz (SRAM / bank 5) */
    uint8_t contend;      /* ULA contended at 3.5 MHz */
    uint8_t video;        /* writes here can change the picture */
    uint8_t rom3_ok;      /* reads come from the 48K BASIC ROM (rom3 automap) */
} nx_slot;

typedef struct nx_timing {
    int hc, vc;           /* 7 MHz pixels per line, lines per frame */
    int int_h, int_v;     /* ULA interrupt position (c_int_h, c_int_v) */
    int min_hactive;      /* first paper pixel (c_min_hactive) */
    int min_vactive;      /* first paper line (c_min_vactive) */
} nx_timing;

typedef struct nx_dma {
    uint8_t wr_seq, rd_seq, reg_temp;
    uint8_t dir_atob;
    uint8_t a_io, a_mode, a_timing;
    uint8_t b_io, b_mode, b_timing, prescaler;
    uint16_t a_start, b_start, len;
    uint8_t mode;          /* R4 mode: 00 byte, 01 continuous, 10 burst */
    uint8_t auto_restart;
    uint8_t read_mask;
    uint8_t z80_mode;      /* port 0x0B: Zilog-compatible counting */
    uint16_t src, dst, counter;
    uint8_t active;        /* transfer in progress (START_DMA ...) */
    uint8_t starting;      /* bus request still to be granted */
    uint8_t end_of_block_n, at_least_one;
    uint64_t next_clk;     /* burst mode: when the next byte may go */
} nx_dma;

typedef struct nx_ctc {
    uint8_t control;       /* control word bits 7..3 */
    uint8_t tc;            /* time constant */
    uint8_t count;
    uint8_t state;         /* 0 reset, 1 reset+await TC, 2 trigger, 3 run, 4 run+await TC */
    uint64_t next;         /* timer mode: tick of the next decrement */
} nx_ctc;

typedef struct zxn {
    struct zx *m;

    uint8_t *sram;
    uint8_t bank5[0x4000];    /* BRAM: 16K bank 5 (pages 0x0A, 0x0B) */
    uint8_t bank7[0x2000];    /* BRAM: first 8K of bank 7 (page 0x0E) */
    uint8_t bootrom[0x2000];
    uint8_t ff_page[0x2000];  /* reads of unmapped memory */
    uint8_t sink[0x2000];     /* writes to read-only memory */
    nx_slot slot[8];

    /* time */
    uint64_t clk;             /* master ticks since power on */
    uint64_t frame_start;     /* clk at this frame's ULA interrupt */
    uint32_t frame_ticks;
    uint64_t frames;
    nx_timing tm;             /* effective timing for this frame */
    uint8_t speed;            /* effective CPU speed 0..3 */
    uint8_t tick;             /* master ticks per CPU T-state */

    /* reset / config (nr_02, nr_03, nr_04) */
    uint8_t config_mode, bootrom_en;
    uint8_t machine_type, machine_timing, user_dt_lock, eff_timing;
    uint8_t reset_type;
    uint8_t reset_req;        /* nr_02 asked for 1 soft / 2 hard reset */
    uint8_t romram_bank;
    uint8_t nr_register;

    /* ports */
    uint8_t port_fe;          /* bits 4..0 */
    uint8_t port_ff;          /* Timex SCLD */
    uint8_t port_7ffd, port_dffd, port_dffd_6, port_1ffd, port_1ffd_special_old;
    uint8_t port_eff7_2, port_eff7_3;
    uint8_t port_e3;          /* divMMC control */
    uint8_t port_e7;          /* SPI chip select */
    uint8_t spi_miso;         /* last byte shifted in */
    uint8_t l2_en, l2_map_wr, l2_map_rd, l2_shadow, l2_segment, l2_offset; /* port 0x123B */
    uint8_t ulap_mode, ulap_index, ulap_en;   /* ULA+ (BF3B / FF3B) */
    uint8_t mapping_mode;     /* nr_8f */
    uint8_t mmu[8];
    uint8_t rom3;             /* the 48K BASIC ROM is paged in (sram_rom3) */

    /* nextregs */
    uint8_t joy0, joy1;
    uint8_t nr_05_5060, nr_05_scandouble;
    uint8_t nr_02, nr_06, nr_07_speed, nr_08, nr_09, nr_0a, nr_0b;
    uint8_t nr_10_coreid;
    uint8_t nr_11;
    uint8_t eff_5060;         /* nr_05 bit 2 as latched at the frame start */
    uint8_t eff_contention_off; /* nr_08 bit 6 as latched */
    uint8_t l2_bank, l2_shadow_bank;          /* nr_12, nr_13 */
    uint8_t transparent;                      /* nr_14 */
    uint8_t nr_15;
    uint8_t l2_scroll_x, l2_scroll_y, l2_scroll_x_msb; /* nr_16, nr_17, nr_71 */
    uint8_t clip[4][4];       /* nr_18 L2, nr_19 sprites, nr_1a ULA, nr_1b tilemap */
    uint8_t clip_idx[4];
    uint8_t line_int_en; uint16_t line_int;   /* nr_22, nr_23 */
    uint8_t ula_scroll_x, ula_scroll_y;       /* nr_26, nr_27 */
    uint16_t tm_scroll_x; uint8_t tm_scroll_y; /* nr_2f/30, nr_31 */
    uint8_t lores_scroll_x, lores_scroll_y;   /* nr_32, nr_33 */
    uint8_t pal_idx, pal_sub, pal_stored;     /* nr_40, nr_44 */
    uint8_t ulanext_format;                   /* nr_42 */
    uint8_t nr_43;
    uint8_t fallback;                         /* nr_4a */
    uint8_t sprite_transp;                    /* nr_4b */
    uint8_t tm_transp;                        /* nr_4c */
    uint8_t copper_mode; uint16_t copper_addr; uint8_t copper_stored; /* nr_60..63 */
    uint8_t copper_offset;                    /* nr_64 */
    uint8_t nr_68, nr_6a, nr_6b, nr_6c, nr_6e, nr_6f, nr_70;
    uint8_t nr_7f;
    uint8_t nr_80, nr_81, nr_8c;
    uint8_t port_en[4], bus_en[4], nr_85_reset_type, nr_89_reset_type, nr_8a;
    uint8_t nr_b8, nr_b9, nr_ba, nr_bb;
    uint8_t nr_c0; uint8_t retn_lsb, retn_msb;
    uint8_t nr_d9, nr_da;
    uint8_t nr_c4_expbus, nr_c6, nr_cc, nr_cd, nr_ce, nr_d8;
    uint8_t im_mode;          /* CPU interrupt mode as decoded by im2_control */

    /* palettes: 1024 x (priority << 9 | RGB333) each */
    uint16_t pal_ulatm[1024];
    uint16_t pal_l2s[1024];

    /* copper */
    uint16_t copper[1024];
    uint16_t cu_pc;
    uint8_t cu_last_mode;
    uint8_t cu_move_clear;    /* a MOVE was output last cycle */
    uint64_t cu_clk;          /* master tick the copper has run up to */
    uint64_t cu_restart;      /* next cvc 0 / hc 0 point (mode 11) */

    /* divMMC automap (device/divmmc.vhd) */
    uint8_t automap_held, automap_hold;
    uint8_t automap;          /* currently mapped by automap */

    /* multiface */
    uint8_t mf_enabled;

    /* interrupts (device/im2_*.vhd) */
    uint8_t im2_state[14];    /* 0 idle, 1 request, 2 ack, 3 in service */
    uint8_t int_status[14];
    uint64_t pulse_start, pulse_end;
    uint64_t line_int_clk;    /* next line interrupt, or ~0 */

    /* devices */
    nx_dma dma;
    nx_ctc ctc[4];
    uint8_t ctc_int_en;
    nx_sd *sd[2];

    /* sprites */
    uint8_t spr_attr[NX_SPRITES][5];
    uint8_t spr_pattern[0x4000];
    uint8_t spr_attr_index;   /* sprite number for port 0x57 */
    uint8_t spr_attr_byte;    /* next attribute byte 0..4 */
    uint16_t spr_pattern_index;
    uint8_t spr_mirror_sprite;
    uint8_t spr_status;
    uint16_t spr_line[320];   /* bit 8 = pixel present, low 8 = index */
    int spr_line_y;

    /* video */
    uint16_t screen[NX_H][NX_W];
    uint64_t video_pix;       /* next 7 MHz pixel to draw (from frame start) */
    uint8_t flash;
    uint8_t keys_ext[2];      /* nr_b0, nr_b1 extended keys (active high) */

    /* Kempston mouse */
    uint8_t mouse_x, mouse_y, mouse_z, mouse_btn;
    int mouse_fx, mouse_fy;   /* movement left over from DPI scaling */

    /* audio */
    ay_chip ay[3];
    uint8_t ay_select, ay_pan[3];
    uint8_t dac[4];
    uint64_t audio_clk;       /* master tick the audio has reached */
    uint64_t audio_next;      /* master tick of the next output sample */
    uint64_t audio_prev;      /* master tick of the last output sample */
    uint64_t audio_frac;      /* fractional part of audio_next, 0.32 */
    int64_t acc_l, acc_r;     /* level x ticks since the last sample */
    uint32_t ay_div;          /* master ticks towards the next AY clock */
} zxn;

int nx_init(struct zx *m);
void nx_free(struct zx *m);
void nx_power_on(struct zx *m);
void nx_soft_reset(struct zx *m);
void nx_run_frame(struct zx *m);
int nx_attach_sd(struct zx *m, int slot, const char *path);
/* Host mouse movement (dy down is positive) and buttons (1 left, 2 right,
   4 middle). */
void nx_mouse(zxn *n, int dx, int dy, int dz, int buttons);
/* Load an 8K boot ROM to use instead of the built-in one. */
int nx_load_bootrom(struct zx *m, const char *path);

/* internal, shared between the next/ files */
void nx_video_reset(zxn *n);
void nx_video_run(zxn *n, uint64_t upto_clk);
void nx_video_frame_end(zxn *n);
uint8_t nx_floating_bus(zxn *n);
void nx_sprite_port_57(zxn *n, uint8_t v);
void nx_sprite_port_5b(zxn *n, uint8_t v);
void nx_sprite_port_303b(zxn *n, uint8_t v);
void nx_sprite_mirror(zxn *n, uint8_t reg, uint8_t v);

nx_sd *nx_sd_open(const char *path);
void nx_sd_close(nx_sd *sd);
uint8_t nx_sd_xfer(nx_sd *sd, uint8_t out);
void nx_sd_deselect(nx_sd *sd);
void nx_sd_tick(nx_sd *sd);          /* once per frame: folder write-back */
int nx_sd_flush(nx_sd *sd);

void nx_dma_reset(zxn *n);
void nx_dma_write(zxn *n, uint8_t v);
uint8_t nx_dma_read(zxn *n);
/* Run the DMA for one byte if it holds the bus; 0 if the CPU may run. */
int nx_dma_run(zxn *n);

/* Bus access for the DMA (no CPU wait states or contention). */
uint8_t nx_mem_read(zxn *n, uint16_t addr);
void nx_mem_write(zxn *n, uint16_t addr, uint8_t v);
uint8_t nx_io_read(zxn *n, uint16_t port);
void nx_io_write(zxn *n, uint16_t port, uint8_t v);

extern const uint8_t nx_bootrom_data[0x2000];

#endif
