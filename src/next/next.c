/*
 * ZX Spectrum Next: memory, ports, nextregs, interrupts, CTC, copper and
 * the frame loop. See next.h for the conventions.
 */
#include "next.h"
#include "../zx/zx.h"
#include "../devices/serial.h"

#include <stdlib.h>
#include <string.h>

/* ---- machine timing (video/zxula_timing.vhd) --------------------------- */

enum { T48, T128, TP3, TPENT };

static int timing_kind(uint8_t t)
{
    if (t & 4)
        return TPENT;
    if ((t & 3) == 2)
        return T128;
    if ((t & 3) == 3)
        return TP3;
    return T48;
}

static void set_timing(zxn *n)
{
    int k = timing_kind(n->eff_timing);
    int hz60 = n->nr_05_5060 && k != TPENT;
    nx_timing *t = &n->tm;

    switch (k) {
    case TPENT:
        t->hc = 448; t->vc = 320; t->int_h = 448 + 3 - 12; t->int_v = 319;
        t->min_hactive = 128; t->min_vactive = 80;
        break;
    case T128:
    case TP3:
        t->hc = 456; t->int_h = 136 + (k == T128 ? 4 : 2) - 12;
        t->min_hactive = 136;
        if (hz60) { t->vc = 264; t->int_v = 0; t->min_vactive = 40; }
        else      { t->vc = 311; t->int_v = 1; t->min_vactive = 64; }
        break;
    default:
        t->hc = 448; t->int_h = 128 - 12; t->int_v = 0; t->min_hactive = 128;
        if (hz60) { t->vc = 264; t->min_vactive = 40; }
        else      { t->vc = 312; t->min_vactive = 64; }
        break;
    }
    n->frame_ticks = (uint32_t)(t->hc * t->vc * 4);
}

/* ULA position of a 7 MHz pixel counted from the frame start. */
static void ula_pos(const zxn *n, uint64_t pix, int *hc_ula, int *vc_ula)
{
    const nx_timing *t = &n->tm;
    int64_t total = (int64_t)t->hc * t->vc;
    int64_t idx = (int64_t)t->int_v * t->hc + t->int_h + 1 + (int64_t)(pix % (uint64_t)total);
    idx -= (int64_t)t->min_vactive * t->hc + t->min_hactive - 11;
    idx %= total;
    if (idx < 0)
        idx += total;
    *hc_ula = (int)(idx % t->hc);
    *vc_ula = (int)(idx / t->hc);
}

/* Pixel (from frame start) at which the ULA counters reach (hc, vc). */
static int64_t ula_pix(const zxn *n, int hc, int vc)
{
    const nx_timing *t = &n->tm;
    int64_t total = (int64_t)t->hc * t->vc;
    int64_t idx = (int64_t)vc * t->hc + hc;
    idx += (int64_t)t->min_vactive * t->hc + t->min_hactive - 11;
    idx -= (int64_t)t->int_v * t->hc + t->int_h + 1;
    idx %= total;
    if (idx < 0)
        idx += total;
    return idx;
}

/* ---- memory decode (MEMORY DECODING in zxnext.vhd) --------------------- */

static int machine_type_48(const zxn *n) { return n->machine_type <= 1; }
static int machine_type_p3(const zxn *n) { return !(n->machine_type <= 1) && n->machine_type != 2 && n->machine_type != 4; }
static int locked_7ffd(const zxn *n)
{
    if (n->mapping_mode == 3 && !n->port_eff7_2)
        return 0;   /* pentagon 1024 */
    return (n->port_7ffd >> 5) & 1;
}

static uint8_t *sram_page(zxn *n, uint32_t page9)
{
    /* A21:A13 as a 9-bit page number of 8K. */
    uint32_t a = page9 << 13;
    if (a >= NX_SRAM_SIZE)
        return NULL;
    return n->sram + a;
}

static int l2_pages(const zxn *n, int *first)
{
    /* SRAM 8K pages the layer 2 display reads from. */
    *first = (n->l2_bank + 16) * 2;
    return ((n->nr_70 >> 4) & 3) ? 10 : 6;
}

static void remap(zxn *n)
{
    int s, rom, rom3, alt_128_n;
    int alt_en = n->nr_8c >> 7, alt_rw = (n->nr_8c >> 6) & 1;
    int lock1 = (n->nr_8c >> 5) & 1, lock0 = (n->nr_8c >> 4) & 1;
    int kind = timing_kind(n->eff_timing);
    int l2first, l2n = l2_pages(n, &l2first);
    int conmem = n->port_e3 >> 7, mapram = (n->port_e3 >> 6) & 1;
    int divmmc_en = (n->port_en[1] & 1) != 0;
    int div_active = divmmc_en && (conmem || n->automap);

    /* rom selection: sram_rom, sram_rom3, sram_alt_128_n */
    if (machine_type_48(n)) {
        rom = 0; rom3 = 1;
        alt_128_n = !(!lock1 && lock0);
    } else if (machine_type_p3(n)) {
        if (lock1 || lock0) {
            rom = lock1 << 1 | lock0; rom3 = lock1 && lock0; alt_128_n = lock1;
        } else {
            rom = ((n->port_1ffd >> 2) & 1) << 1 | ((n->port_7ffd >> 4) & 1);
            rom3 = rom == 3; alt_128_n = rom & 1;
        }
    } else {
        if (lock1 || lock0) {
            rom = lock1; rom3 = lock1; alt_128_n = lock1;
        } else {
            rom = (n->port_7ffd >> 4) & 1; rom3 = rom; alt_128_n = rom;
        }
    }
    n->rom3 = (uint8_t)rom3;

    for (s = 0; s < 8; s++) {
        nx_slot *sl = &n->slot[s];
        uint8_t page = n->mmu[s];
        int a13 = s & 1;
        uint8_t *rd = n->ff_page, *wr = n->sink;
        int wait = 0, video = 0, rdonly = 0;
        int override_div = 0, override_l2 = 0;
        int is_rom = 0, rom3_ok = 0;

        /* mem_contend */
        sl->contend = 0;
        if (page < 16) {
            if (kind == T48) sl->contend = (page >> 1) == 5;
            else if (kind == T128) sl->contend = (page >> 1) & 1;
            else if (kind == TP3) sl->contend = (page >> 1) >= 4;
        }

        if (s < 2 && n->mf_enabled) {
            rd = n->sram + 0x014000 + a13 * 0x2000;
            wr = a13 ? rd : n->sink;
            wait = 1;
        } else if (page < 0xe0) {
            if (page == 0x0a || page == 0x0b) {
                rd = wr = n->bank5 + (page & 1) * 0x2000;
                video = 1;
                wait = 1;
            } else if (page == 0x0e) {
                rd = wr = n->bank7;
                video = 1;
            } else {
                uint8_t *p = sram_page(n, (uint32_t)page + 0x20);
                if (p) {
                    rd = wr = p;
                    wait = 1;
                    if ((int)page + 0x20 >= l2first && (int)page + 0x20 < l2first + l2n)
                        video = 1;
                }
            }
            if (s < 2) {
                override_div = 1; override_l2 = 1;
            }
        } else if (s < 2 && n->config_mode) {
            rd = wr = n->sram + ((uint32_t)n->romram_bank << 14) + a13 * 0x2000;
            wait = 1;
            override_div = 1; override_l2 = 1;
        } else if (s < 2) {
            rd = n->sram + ((uint32_t)rom << 14) + a13 * 0x2000;
            wr = n->sink;
            rdonly = !(alt_en && alt_rw);
            wait = 1;
            is_rom = 1;
            override_div = 1; override_l2 = 1;
        }
        if (s >= 2 && s < 6 && n->l2_segment == 3)
            override_l2 = 1;

        /* rom3 automap points need the 48K ROM underneath (whatever divmmc does) */
        if (is_rom && !n->l2_map_rd)
            rom3_ok = (alt_en && rdonly) ? alt_128_n : rom3;

        /* final decode: divmmc, layer 2 mapping, altrom */
        if (override_div && div_active && s == 0 && !mapram) {
            rd = n->sram + 0x010000;
            wr = n->sink;
            wait = 1;
            video = 0;
        } else if (override_div && div_active && (s == 1 || mapram)) {
            int bank = s == 0 ? 3 : (n->port_e3 & 15);
            int ro = s == 0 || (mapram && bank == 3);
            rd = n->sram + 0x020000 + (uint32_t)bank * 0x2000;
            wr = ro ? n->sink : rd;
            wait = 1;
            video = 0;
        } else {
            if (override_l2 && (n->l2_map_rd || n->l2_map_wr)) {
                int seg = n->l2_segment == 3 ? (s >> 1) : n->l2_segment;
                int bank = (n->l2_shadow ? n->l2_shadow_bank : n->l2_bank) + ((seg + n->l2_offset) & 7);
                uint32_t pg = (uint32_t)(bank * 2 + a13);
                uint8_t *p = pg < 0xe0 ? sram_page(n, pg + 0x20) : NULL;
                int l2video = (int)pg + 0x20 >= l2first && (int)pg + 0x20 < l2first + l2n;
                if (n->l2_map_rd) {
                    rd = p ? p : n->ff_page;
                    wait = 1;
                }
                if (n->l2_map_wr) {
                    wr = p ? p : n->sink;
                    video = l2video;
                }
            }
            if (is_rom && alt_en) {
                uint8_t *alt = n->sram + 0x018000 + (uint32_t)alt_128_n * 0x4000 + a13 * 0x2000;
                if (rdonly && !n->l2_map_rd)
                    rd = alt;           /* reads come from the alt rom */
                if (!rdonly && !n->l2_map_wr)
                    wr = alt;           /* writes go to the alt rom */
            }
        }
        if (s < 2 && n->bootrom_en) {
            rd = n->bootrom;
            wait = 0;
        }
        sl->rd = rd;
        sl->wr = wr;
        sl->wait = (uint8_t)wait;
        sl->video = (uint8_t)video;
        sl->rom3_ok = (uint8_t)rom3_ok;
    }
}

/* MMU update after a write to 7FFD, DFFD, 1FFD, EFF7 or nextreg 0x8E/0x8F
 * (the MMU process in zxnext.vhd). special_old is the +3 special-paging
 * flag from before the write; ram_change is 0 only for an 0x8E write that
 * leaves the RAM bank alone. */
static void mmu_from_ports(zxn *n, int special_old, int ram_change)
{
    int pent = n->mapping_mode == 2 || (n->mapping_mode == 3 && !n->port_eff7_2);
    int pent1024 = n->mapping_mode == 3 && !n->port_eff7_2;
    int bank = n->port_7ffd & 7;

    if (pent)
        bank |= (n->port_7ffd >> 6 & 3) << 3;
    else
        bank |= (n->port_dffd & 3) << 3;
    if (!pent)
        bank |= (n->port_dffd >> 2 & 1) << 5;
    else if (pent1024 && (n->port_7ffd & 0x20))
        bank |= 1 << 5;
    if (!pent)
        bank |= (n->port_dffd >> 3 & 1) << 6;

    if (n->port_1ffd & 1) {
        int b2 = n->port_1ffd >> 2 & 1, b1 = n->port_1ffd >> 1 & 1;
        int hi = (b2 | b1) << 3;
        n->mmu[0] = (uint8_t)(hi | 0); n->mmu[1] = (uint8_t)(hi | 1);
        n->mmu[2] = (uint8_t)(hi | (b2 & b1) << 2 | 2); n->mmu[3] = (uint8_t)(hi | (b2 & b1) << 2 | 3);
        n->mmu[4] = (uint8_t)(hi | 4); n->mmu[5] = (uint8_t)(hi | 5);
        n->mmu[6] = (uint8_t)(((!b2) & b1) << 3 | 6); n->mmu[7] = (uint8_t)(((!b2) & b1) << 3 | 7);
        return;
    }
    if (n->port_eff7_3) {
        n->mmu[0] = 0x00; n->mmu[1] = 0x01;
    } else {
        n->mmu[0] = 0xff; n->mmu[1] = 0xff;
    }
    if (special_old) {
        n->mmu[2] = 0x0a; n->mmu[3] = 0x0b;
        n->mmu[4] = 0x04; n->mmu[5] = 0x05;
    }
    if (special_old || ram_change) {
        n->mmu[6] = (uint8_t)(bank << 1);
        n->mmu[7] = (uint8_t)(bank << 1 | 1);
    }
}

/* ---- palettes ----------------------------------------------------------- */

static void palette_defaults(zxn *n)
{
    /* Spectrum colours for the ULA (ink 0-15, paper 16-31, as the ULA
     * indexes them), and RGB332 identity everywhere else. */
    static const uint16_t spec[16] = {
        0x000, 0x005, 0x140, 0x145, 0x028, 0x02d, 0x168, 0x16d,
        0x000, 0x007, 0x1c0, 0x1c7, 0x038, 0x03f, 0x1f8, 0x1ff,
    };
    int i, p;
    for (i = 0; i < 1024; i++) {
        uint8_t v = (uint8_t)i;
        uint16_t c = (uint16_t)(v << 1 | ((v & 3) ? 1 : 0));
        n->pal_ulatm[i] = c;
        n->pal_l2s[i] = c;
    }
    for (p = 0; p < 2; p++)
        for (i = 0; i < 256; i++)
            n->pal_ulatm[p * 256 + i] = spec[i & 15];
}

static void palette_write(zxn *n, uint16_t value)
{
    int sel = (n->nr_43 >> 4) & 7;
    int idx = ((sel >> 1) & 1) << 9 | ((sel >> 2) & 1) << 8 | n->pal_idx;
    if (((sel >> 1) & 1) == (sel & 1))
        n->pal_ulatm[idx] = value;
    else
        n->pal_l2s[idx] = value;
}

static uint16_t palette_read(const zxn *n)
{
    int sel = (n->nr_43 >> 4) & 7;
    int idx = ((sel >> 1) & 1) << 9 | ((sel >> 2) & 1) << 8 | n->pal_idx;
    return ((sel >> 1) & 1) == (sel & 1) ? n->pal_ulatm[idx] : n->pal_l2s[idx];
}

/* ---- helpers -------------------------------------------------------------- */

static int port_enabled(const zxn *n, int bit)
{
    return (n->port_en[bit >> 3] >> (bit & 7)) & 1;
}

/* Bring video and audio up to the present before a change that affects them. */
static void audio_run(zxn *n, uint64_t upto);

static void sync_video(zxn *n)
{
    nx_video_run(n, n->clk);
}

static void sync_audio(zxn *n)
{
    audio_run(n, n->clk);
}

static void set_speed(zxn *n, uint8_t speed)
{
    n->speed = speed & 3;
    n->tick = (uint8_t)(8 >> n->speed);
}

/* ---- interrupts (im2_peripheral / im2_device / pulse) -------------------- */

enum { IM2_IDLE, IM2_REQ, IM2_ACK, IM2_ISR };
enum { SRC_LINE = 0, SRC_UART0_RX = 1, SRC_UART1_RX = 2, SRC_CTC0 = 3, SRC_ULA = 11 };

static int pulse_len(const zxn *n)
{
    int k = timing_kind(n->eff_timing);
    return (k == T48 || k == TP3) ? 32 : 36;
}

static void start_pulse(zxn *n, uint64_t t)
{
    if (t < n->pulse_end)
        return;
    n->pulse_start = t;
    n->pulse_end = t + (uint64_t)pulse_len(n) * n->tick;
}

static int source_enabled(const zxn *n, int src)
{
    if (src == SRC_LINE)
        return n->line_int_en;
    if (src == SRC_ULA)
        return !(n->port_ff >> 6 & 1);
    if (src >= SRC_CTC0 && src < SRC_CTC0 + 4)
        return n->ctc[src - SRC_CTC0].control >> 4 & 1;
    if (src == SRC_UART0_RX)
        return (n->nr_c6 & 3) != 0;
    if (src == SRC_UART1_RX)
        return (n->nr_c6 >> 4 & 3) != 0;
    if (src == 12)
        return n->nr_c6 >> 2 & 1;
    if (src == 13)
        return n->nr_c6 >> 6 & 1;
    return 0;
}

static void raise_int_at(zxn *n, int src, int unq, uint64_t t)
{
    int en = source_enabled(n, src);
    int im2hw = n->nr_c0 & 1;
    n->int_status[src] = 1;
    if (!en && !unq)
        return;
    if (im2hw) {
        if (n->im2_state[src] == IM2_IDLE)
            n->im2_state[src] = IM2_REQ;
        /* the ULA still pulses when the CPU is not in IM 2 */
        if (src == SRC_ULA && n->m->cpu.im != 2)
            start_pulse(n, t);
    } else {
        start_pulse(n, t);
    }
}

void nx_raise_int(zxn *n, int src, uint64_t t)
{
    raise_int_at(n, src, 0, t);
}

static void raise_int(zxn *n, int src, int unq)
{
    raise_int_at(n, src, unq, n->clk);
}

/* Device that would be acknowledged now, or -1. */
static int im2_pending(const zxn *n)
{
    int i;
    if (!(n->nr_c0 & 1) || n->m->cpu.im != 2)
        return -1;
    for (i = 0; i < 14; i++) {
        if (n->im2_state[i] == IM2_REQ)
            return i;
        if (n->im2_state[i] != IM2_IDLE)
            return -1;      /* a higher priority device is being serviced */
    }
    return -1;
}

static int int_line(const zxn *n)
{
    if (n->clk >= n->pulse_start && n->clk < n->pulse_end)
        return 1;
    return im2_pending(n) >= 0;
}

static uint8_t b_int_ack(void *ctx)
{
    zx *m = ctx;
    zxn *n = m->next;
    int d = im2_pending(n);
    if (d < 0)
        return 0xff;
    n->im2_state[d] = IM2_ISR;
    return (uint8_t)((n->nr_c0 & 0xe0) | d << 1);
}

static void b_reti(void *ctx)
{
    zx *m = ctx;
    zxn *n = m->next;
    int i;
    for (i = 0; i < 14; i++) {
        if (n->im2_state[i] == IM2_ISR) {
            n->im2_state[i] = IM2_IDLE;
            break;
        }
        if (n->im2_state[i] == IM2_ACK)
            break;
    }
}

static void b_retn(void *ctx)
{
    zx *m = ctx;
    zxn *n = m->next;
    /* divmmc_retn_seen: drop the automap */
    if (n->automap_held || n->automap) {
        n->automap_held = n->automap_hold = 0;
        n->automap = 0;
        remap(n);
    }
}

/* Line interrupt: at hc_ula 255 of the line before nr_23 (copper counter). */
static void schedule_line_int(zxn *n)
{
    const nx_timing *t = &n->tm;
    int line = n->line_int == 0 ? t->vc - 1 : n->line_int - 1;
    int vc_ula = ((line - n->copper_offset) % t->vc + t->vc) % t->vc;
    int64_t pix;
    if (n->line_int >= t->vc && n->line_int != 0) {
        n->line_int_clk = ~(uint64_t)0;
        return;
    }
    pix = ula_pix(n, 255, vc_ula);
    n->line_int_clk = n->frame_start + (uint64_t)pix * 4;
    if (n->line_int_clk < n->clk)
        n->line_int_clk += n->frame_ticks;
}

/* ---- CTC (device/ctc_chan.vhd), 4 channels at 28 MHz ---------------------- */

enum { CTC_RESET, CTC_RESET_TC, CTC_TRIGGER, CTC_RUN, CTC_RUN_TC };

static uint32_t ctc_prescale(const nx_ctc *c)
{
    return (c->control & 4) ? 256 : 16;   /* control bit 5 */
}

static int ctc_running(const nx_ctc *c)
{
    return c->state == CTC_RUN || c->state == CTC_RUN_TC;
}

static void ctc_trigger(zxn *n, int ch);

/* Count one tick on a channel; on reaching zero, reload and fire. */
static void ctc_count(zxn *n, int ch)
{
    nx_ctc *c = &n->ctc[ch];
    if ((uint8_t)(c->count - 1) == 0) {
        c->count = c->tc;
        if (ctc_running(c)) {
            raise_int(n, SRC_CTC0 + ch, 0);
            ctc_trigger(n, (ch + 1) & 3);
        }
    } else {
        c->count--;
    }
}

/* Zero-count pulse from the previous channel: the clock/trigger input. */
static void ctc_trigger(zxn *n, int ch)
{
    nx_ctc *c = &n->ctc[ch];
    if (c->state == CTC_TRIGGER && (c->control & 8) == 0 && (c->control & 1)) {
        c->state = CTC_RUN;
        c->next = n->clk + ctc_prescale(c);
        return;
    }
    if (ctc_running(c) && (c->control & 8))   /* counter mode (bit 6) */
        ctc_count(n, ch);
}

static void ctc_advance(zxn *n, uint64_t upto)
{
    for (;;) {
        int i, best = -1;
        uint64_t t = upto + 1;
        for (i = 0; i < 4; i++) {
            nx_ctc *c = &n->ctc[i];
            if (ctc_running(c) && !(c->control & 8) && c->next <= t) {
                t = c->next;
                best = i;
            }
        }
        if (best < 0 || t > upto)
            return;
        {
            uint64_t save = n->clk;
            nx_ctc *c = &n->ctc[best];
            n->clk = t;
            c->next += ctc_prescale(c);
            ctc_count(n, best);
            n->clk = save;
        }
    }
}

static uint64_t ctc_next_event(const zxn *n)
{
    uint64_t t = ~(uint64_t)0;
    int i;
    for (i = 0; i < 4; i++) {
        const nx_ctc *c = &n->ctc[i];
        if (ctc_running(c) && !(c->control & 8) && c->next < t)
            t = c->next;
    }
    return t;
}

static void ctc_write(zxn *n, int ch, uint8_t v)
{
    nx_ctc *c = &n->ctc[ch];
    ctc_advance(n, n->clk);
    if (c->state == CTC_RESET_TC || c->state == CTC_RUN_TC) {
        c->tc = v;
        if (c->state == CTC_RESET_TC) {
            c->count = v;
            /* timer mode with bit 3 set waits for a trigger edge */
            if (!(c->control & 8) && (c->control & 1)) {
                c->state = CTC_TRIGGER;
            } else {
                c->state = CTC_RUN;
                c->next = n->clk + ctc_prescale(c);
            }
        } else {
            c->state = CTC_RUN;
        }
        return;
    }
    if (v & 1) {
        /* control word: bits 7..3 stored as control 4..0 */
        c->control = (uint8_t)(v >> 3);
        if (v & 2) {
            c->state = (v & 4) ? CTC_RESET_TC : CTC_RESET;
            c->count = c->tc;
        } else if (v & 4) {
            c->state = ctc_running(c) ? CTC_RUN_TC : CTC_RESET_TC;
        }
    }
    /* else: interrupt vector, unused on the Next (nextreg 0xC0 supplies it) */
}

static uint8_t ctc_read(zxn *n, int ch)
{
    nx_ctc *c = &n->ctc[ch];
    ctc_advance(n, n->clk);
    if (ctc_running(c) && !(c->control & 8)) {
        /* derive the live count from the time to the next decrement */
        return c->count;
    }
    return ctc_running(c) ? c->count : c->tc;
}

/* ---- copper (device/copper.vhd) ---------------------------------------- */

static void copper_restart_point(zxn *n);

static void copper_mode(zxn *n, uint8_t mode)
{
    if (mode != n->copper_mode) {
        if (mode == 1 || mode == 3)
            n->cu_pc = 0;
        n->cu_move_clear = 0;
    }
    n->copper_mode = mode;
    n->cu_clk = n->clk;
    copper_restart_point(n);
}

/* ---- nextregs ----------------------------------------------------------- */

static void ports_changed(zxn *n, int special_old, int ram_change)
{
    mmu_from_ports(n, special_old, ram_change);
    remap(n);
}

static void automap_reset(zxn *n)
{
    n->automap_held = n->automap_hold = 0;
    n->automap = 0;
}

static void nr_write(zxn *n, uint8_t reg, uint8_t v)
{
    int i, special_old;

    sync_video(n);

    switch (reg) {
    case 0x02:
        n->nr_02 = v & 0x80;
        if (v & 2)
            n->reset_req = 2;
        else if (v & 1)
            n->reset_req = 1;
        if (!(v & 0x10))
            n->nr_da = 0;
        break;
    case 0x03:
        n->bootrom_en = 0;
        if ((v & 0x80) && !n->user_dt_lock && !(v & 8)) {
            static const uint8_t tm[8] = { 1, 1, 2, 3, 4, 3, 3, 3 };
            n->machine_timing = tm[v >> 4 & 7];
            if (n->machine_timing & 4)
                n->nr_05_5060 = 0;
        }
        n->user_dt_lock ^= (v >> 3) & 1;
        if (n->config_mode && (v & 7) >= 1 && (v & 7) <= 4)
            n->machine_type = v & 7;
        if ((v & 7) == 7)
            n->config_mode = 1;
        else if ((v & 7) != 0)
            n->config_mode = 0;
        remap(n);
        break;
    case 0x04:
        n->romram_bank = v;
        remap(n);
        break;
    case 0x05:
        n->joy0 = (uint8_t)((v >> 3 & 1) << 2 | (v >> 6 & 3));
        n->joy1 = (uint8_t)((v >> 1 & 1) << 2 | (v >> 4 & 3));
        if (!(n->machine_timing & 4))
            n->nr_05_5060 = (v >> 2) & 1;
        n->nr_05_scandouble = v & 1;
        break;
    case 0x06:
        n->nr_06 = n->config_mode ? v : (uint8_t)((v & ~4) | (n->nr_06 & 4));
        break;
    case 0x07:
        n->nr_07_speed = v & 3;
        break;
    case 0x08:
        if (v & 0x80) {
            n->port_7ffd &= (uint8_t)~0x20;
            remap(n);
        }
        sync_audio(n);
        n->nr_08 = v & 0x7f;
        break;
    case 0x09:
        n->nr_09 = v & 0xf3;
        if (v & 8) {
            n->port_e3 &= (uint8_t)~0x40;
            remap(n);
        }
        break;
    case 0x0a:
        n->nr_0a = n->config_mode ? (v & 0xfb) : (uint8_t)((n->nr_0a & 0xe0) | (v & 0x1b));
        if (!(n->nr_0a & 0x10)) {
            automap_reset(n);
            remap(n);
        }
        break;
    case 0x0b:
        n->nr_0b = v & 0xb1;
        break;
    case 0x10:
        if (n->config_mode && !(v & 0x10) && (v & 15) != 15)
            n->nr_10_coreid = v & 15;
        break;
    case 0x11:
        if (n->config_mode)
            n->nr_11 = (v & 7) == 7 ? 0 : (v & 7);
        break;
    case 0x12:
        n->l2_bank = v & 0x7f;
        remap(n);
        break;
    case 0x13:
        n->l2_shadow_bank = v & 0x7f;
        remap(n);
        break;
    case 0x14: n->transparent = v; break;
    case 0x15: n->nr_15 = v; break;
    case 0x16: n->l2_scroll_x = v; break;
    case 0x17: n->l2_scroll_y = v; break;
    case 0x18: case 0x19: case 0x1a: case 0x1b:
        i = reg - 0x18;
        n->clip[i][n->clip_idx[i]] = v;
        n->clip_idx[i] = (n->clip_idx[i] + 1) & 3;
        break;
    case 0x1c:
        for (i = 0; i < 4; i++)
            if (v >> i & 1)
                n->clip_idx[i] = 0;
        break;
    case 0x20:
        /* unqualified interrupts: bit 7 line, bit 6 ULA, bits 3:0 CTC */
        if (v & 0x80) raise_int(n, SRC_LINE, 1);
        if (v & 0x40) raise_int(n, SRC_ULA, 1);
        for (i = 0; i < 4; i++)
            if (v >> i & 1)
                raise_int(n, SRC_CTC0 + i, 1);
        break;
    case 0x22:
        n->line_int_en = (v >> 1) & 1;
        n->line_int = (uint16_t)((n->line_int & 0xff) | (v & 1) << 8);
        n->port_ff = (uint8_t)((n->port_ff & ~0x40) | (v & 4) << 4);
        schedule_line_int(n);
        break;
    case 0x23:
        n->line_int = (uint16_t)((n->line_int & 0x100) | v);
        schedule_line_int(n);
        break;
    case 0x26: n->ula_scroll_x = v; break;
    case 0x27: n->ula_scroll_y = v; break;
    case 0x2f: n->tm_scroll_x = (uint16_t)((n->tm_scroll_x & 0xff) | (v & 3) << 8); break;
    case 0x30: n->tm_scroll_x = (uint16_t)((n->tm_scroll_x & 0x300) | v); break;
    case 0x31: n->tm_scroll_y = v; break;
    case 0x32: n->lores_scroll_x = v; break;
    case 0x33: n->lores_scroll_y = v; break;
    case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
    case 0x75: case 0x76: case 0x77: case 0x78: case 0x79:
        nx_sprite_mirror(n, reg, v);
        break;
    case 0x40:
        n->pal_idx = v;
        n->pal_sub = 0;
        break;
    case 0x41:
        palette_write(n, (uint16_t)(v << 1 | ((v & 3) ? 1 : 0)));
        if (!(n->nr_43 & 0x80))
            n->pal_idx++;
        n->pal_sub = 0;
        break;
    case 0x42: n->ulanext_format = v; break;
    case 0x43:
        n->nr_43 = v;
        n->pal_sub = 0;
        break;
    case 0x44:
        if (!n->pal_sub) {
            n->pal_stored = v;
        } else {
            palette_write(n, (uint16_t)((v >> 6 & 3) << 9 | n->pal_stored << 1 | (v & 1)));
            if (!(n->nr_43 & 0x80))
                n->pal_idx++;
        }
        n->pal_sub ^= 1;
        break;
    case 0x4a: n->fallback = v; break;
    case 0x4b: n->sprite_transp = v; break;
    case 0x4c: n->tm_transp = v & 15; break;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        n->mmu[reg & 7] = v;
        remap(n);
        break;
    case 0x60: {
        uint16_t *w = &n->copper[(n->copper_addr >> 1) & 1023];
        if (n->copper_addr & 1)
            *w = (uint16_t)((*w & 0xff00) | v);
        else
            *w = (uint16_t)((*w & 0x00ff) | v << 8);
        n->copper_addr = (n->copper_addr + 1) & 0x7ff;
        n->cu_clk = n->clk;
        break;
    }
    case 0x61:
        n->copper_addr = (uint16_t)((n->copper_addr & 0x700) | v);
        break;
    case 0x62:
        n->copper_addr = (uint16_t)((n->copper_addr & 0xff) | (v & 7) << 8);
        copper_mode(n, v >> 6);
        break;
    case 0x63:
        if (n->copper_addr & 1)
            n->copper[(n->copper_addr >> 1) & 1023] = (uint16_t)(n->copper_stored << 8 | v);
        else
            n->copper_stored = v;
        n->copper_addr = (n->copper_addr + 1) & 0x7ff;
        n->cu_clk = n->clk;
        break;
    case 0x64:
        n->copper_offset = v;
        schedule_line_int(n);
        copper_restart_point(n);
        break;
    case 0x68:
        n->nr_68 = v & 0xf5;
        n->ulap_en = (v >> 3) & 1;
        break;
    case 0x69:
        n->l2_en = v >> 7;
        n->port_7ffd = (uint8_t)((n->port_7ffd & ~8) | (v >> 3 & 8));
        n->port_ff = (uint8_t)((n->port_ff & 0xc0) | (v & 0x3f));
        break;
    case 0x6a: n->nr_6a = v & 0x3f; break;
    case 0x6b: n->nr_6b = v; break;
    case 0x6c: n->nr_6c = v; break;
    case 0x6e: n->nr_6e = v & 0xbf; break;
    case 0x6f: n->nr_6f = v & 0xbf; break;
    case 0x70:
        n->nr_70 = v & 0x3f;
        remap(n);
        break;
    case 0x71: n->l2_scroll_x_msb = v & 1; break;
    case 0x7f: n->nr_7f = v; break;
    case 0x80: n->nr_80 = v; break;
    case 0x81: n->nr_81 = v & 0x78; break;
    case 0x82: case 0x83: case 0x84:
        n->port_en[reg - 0x82] = v;
        if (!(n->port_en[1] & 1))
            automap_reset(n);
        remap(n);
        break;
    case 0x85:
        n->port_en[3] = v & 15;
        n->nr_85_reset_type = v >> 7;
        break;
    case 0x86: case 0x87: case 0x88:
        n->bus_en[reg - 0x86] = v;
        break;
    case 0x89:
        n->bus_en[3] = v & 15;
        n->nr_89_reset_type = v >> 7;
        break;
    case 0x8a: n->nr_8a = v & 0x3f; break;
    case 0x8c:
        n->nr_8c = v;
        remap(n);
        break;
    case 0x8e:
        special_old = n->port_1ffd & 1;
        if (v & 8) {
            n->port_7ffd = (uint8_t)((n->port_7ffd & ~7) | (v >> 4 & 7));
            n->port_dffd = (uint8_t)((n->port_dffd & ~0x0f) | (v >> 7));
        }
        if (!(v & 4))
            n->port_7ffd = (uint8_t)((n->port_7ffd & ~0x10) | (v & 1) << 4);
        n->port_1ffd = (uint8_t)((v >> 1 & 1) << 2 | (v & 1) << 1 | (v >> 2 & 1));
        ports_changed(n, special_old, (v >> 3) & 1);
        break;
    case 0x8f:
        n->mapping_mode = v & 3;
        ports_changed(n, 0, 1);
        break;
    case 0xb8: n->nr_b8 = v; break;
    case 0xb9: n->nr_b9 = v; break;
    case 0xba: n->nr_ba = v; break;
    case 0xbb: n->nr_bb = v; break;
    case 0xc0: n->nr_c0 = v & 0xe9; break;
    case 0xc2: n->retn_lsb = v; break;
    case 0xc3: n->retn_msb = v; break;
    case 0xc4:
        n->nr_c4_expbus = v >> 7;
        n->line_int_en = (v >> 1) & 1;
        n->port_ff = (uint8_t)((n->port_ff & ~0x40) | (~v & 1) << 6);
        schedule_line_int(n);
        break;
    case 0xc5:
        for (i = 0; i < 4; i++)
            n->ctc[i].control = (uint8_t)((n->ctc[i].control & ~0x10) | (v >> i & 1) << 4);
        break;
    case 0xc6: n->nr_c6 = v & 0x77; break;
    case 0xc8:
        if (v & 1) n->int_status[SRC_ULA] = 0;
        if (v & 2) n->int_status[SRC_LINE] = 0;
        break;
    case 0xc9:
        for (i = 0; i < 8; i++)
            if (v >> i & 1)
                n->int_status[SRC_CTC0 + i] = 0;
        break;
    case 0xca:
        if (v & 0x40) n->int_status[13] = 0;
        if (v & 0x04) n->int_status[12] = 0;
        if (v & 0x30) n->int_status[2] = 0;
        if (v & 0x03) n->int_status[1] = 0;
        break;
    case 0xcc: n->nr_cc = v & 0x83; break;
    case 0xcd: n->nr_cd = v; break;
    case 0xce: n->nr_ce = v & 0x77; break;
    case 0xd8: n->nr_d8 = v & 1; break;
    case 0xd9: n->nr_d9 = v; break;
    case 0xff:
        /* ULA+ palette write arriving through the nextreg path */
        {
            int sel2 = (n->nr_43 >> 6) & 1;
            n->pal_ulatm[sel2 << 8 | 0xc0 | (n->ulap_index & 0x3f)] =
                (uint16_t)(v << 1 | ((v & 3) ? 1 : 0));
        }
        break;
    default:
        break;
    }
}

static int int_status_bit(const zxn *n, int src)
{
    return n->int_status[src] || n->im2_state[src] == IM2_REQ;
}

static int cvc_now(zxn *n);

static uint8_t nr_read(zxn *n, uint8_t reg)
{
    int i;
    uint8_t v;
    switch (reg) {
    case 0x00: return 0x0a;                     /* machine id: ZX Next */
    case 0x01: return 0x32;                     /* core 3.02 */
    case 0x02: return (uint8_t)(n->nr_02 | ((n->nr_da & 3) ? 0x10 : 0) | (n->reset_type & 3));
    case 0x03: return (uint8_t)(n->pal_sub << 7 | n->machine_timing << 4 | n->user_dt_lock << 3 | n->machine_type);
    case 0x05: return (uint8_t)((n->joy0 & 3) << 6 | (n->joy1 & 3) << 4 | (n->joy0 >> 2) << 3 |
                                n->eff_5060 << 2 | (n->joy1 >> 2) << 1 | n->nr_05_scandouble);
    case 0x06: return n->nr_06;
    case 0x07: return (uint8_t)(n->speed << 4 | n->nr_07_speed);
    case 0x08: return (uint8_t)((!locked_7ffd(n)) << 7 | n->eff_contention_off << 6 | (n->nr_08 & 0x3f));
    case 0x09: return n->nr_09 & 0xf7;
    case 0x0a: return n->nr_0a & 0xfb;
    case 0x0b: return n->nr_0b;
    case 0x0e: return 0x03;                     /* sub version */
    case 0x0f: return 0x02;                     /* board issue 4 */
    case 0x10: return (uint8_t)(n->nr_10_coreid << 2);
    case 0x11: return n->nr_11;
    case 0x12: return n->l2_bank;
    case 0x13: return n->l2_shadow_bank;
    case 0x14: return n->transparent;
    case 0x15: return n->nr_15;
    case 0x16: return n->l2_scroll_x;
    case 0x17: return n->l2_scroll_y;
    case 0x18: case 0x19: case 0x1a: case 0x1b:
        i = reg - 0x18;
        return n->clip[i][n->clip_idx[i]];
    case 0x1c:
        return (uint8_t)(n->clip_idx[3] << 6 | n->clip_idx[2] << 4 | n->clip_idx[1] << 2 | n->clip_idx[0]);
    case 0x1e: return (uint8_t)(cvc_now(n) >> 8);
    case 0x1f: return (uint8_t)cvc_now(n);
    case 0x20:
        v = (uint8_t)(int_status_bit(n, SRC_LINE) << 7 | int_status_bit(n, SRC_ULA) << 6);
        for (i = 0; i < 4; i++)
            v |= (uint8_t)(int_status_bit(n, SRC_CTC0 + i) << i);
        return v;
    case 0x22:
        return (uint8_t)(int_line(n) << 7 | (n->port_ff >> 6 & 1) << 2 | n->line_int_en << 1 | (n->line_int >> 8));
    case 0x23: return (uint8_t)n->line_int;
    case 0x26: return n->ula_scroll_x;
    case 0x27: return n->ula_scroll_y;
    case 0x28: return n->pal_stored;
    case 0x2f: return (uint8_t)(n->tm_scroll_x >> 8);
    case 0x30: return (uint8_t)n->tm_scroll_x;
    case 0x31: return n->tm_scroll_y;
    case 0x32: return n->lores_scroll_x;
    case 0x33: return n->lores_scroll_y;
    case 0x34: return n->spr_mirror_sprite & 0x7f;
    case 0x40: return n->pal_idx;
    case 0x41: return (uint8_t)(palette_read(n) >> 1);
    case 0x42: return n->ulanext_format;
    case 0x43: return n->nr_43;
    case 0x44: {
        uint16_t p = palette_read(n);
        return (uint8_t)((p >> 9 & 3) << 6 | (p & 1));
    }
    case 0x4a: return n->fallback;
    case 0x4b: return n->sprite_transp;
    case 0x4c: return n->tm_transp;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        return n->mmu[reg & 7];
    case 0x61: return (uint8_t)n->copper_addr;
    case 0x62: return (uint8_t)(n->copper_mode << 6 | n->copper_addr >> 8);
    case 0x64: return n->copper_offset;
    case 0x68: return (uint8_t)((n->nr_68 & 0xf5) | n->ulap_en << 3);
    case 0x69: return (uint8_t)(n->l2_en << 7 | (n->port_7ffd & 8) << 3 | (n->port_ff & 0x3f));
    case 0x6a: return n->nr_6a;
    case 0x6b: return n->nr_6b;
    case 0x6c: return n->nr_6c;
    case 0x6e: return n->nr_6e;
    case 0x6f: return n->nr_6f;
    case 0x70: return n->nr_70;
    case 0x71: return n->l2_scroll_x_msb;
    case 0x7f: return n->nr_7f;
    case 0x80: return n->nr_80;
    case 0x81: return (uint8_t)(0x80 | n->nr_81);
    case 0x82: case 0x83: case 0x84: return n->port_en[reg - 0x82];
    case 0x85: return (uint8_t)(n->nr_85_reset_type << 7 | n->port_en[3]);
    case 0x86: case 0x87: case 0x88: return n->bus_en[reg - 0x86];
    case 0x89: return (uint8_t)(n->nr_89_reset_type << 7 | n->bus_en[3]);
    case 0x8a: return n->nr_8a;
    case 0x8c: return n->nr_8c;
    case 0x8e:
        return (uint8_t)((n->port_dffd & 1) << 7 | (n->port_7ffd & 7) << 4 | 8 |
                         (n->port_1ffd & 1) << 2 | (n->port_1ffd >> 2 & 1) << 1 |
                         (((n->port_7ffd >> 4 & 1) & !(n->port_1ffd & 1)) | ((n->port_1ffd >> 1) & n->port_1ffd & 1)));
    case 0x8f: return n->mapping_mode;
    case 0xb0: return n->keys_ext[0];
    case 0xb1: return n->keys_ext[1];
    case 0xb8: return n->nr_b8;
    case 0xb9: return n->nr_b9;
    case 0xba: return n->nr_ba;
    case 0xbb: return n->nr_bb;
    case 0xc0: {
        uint8_t im = n->m->cpu.im;
        return (uint8_t)((n->nr_c0 & 0xe9) | (im == 0 ? 0 : im == 1 ? 2 : 4) );
    }
    case 0xc2: return n->retn_lsb;
    case 0xc3: return n->retn_msb;
    case 0xc4: return (uint8_t)(n->nr_c4_expbus << 7 | n->line_int_en << 1 | !(n->port_ff >> 6 & 1));
    case 0xc5:
        v = 0;
        for (i = 0; i < 4; i++)
            v |= (uint8_t)((n->ctc[i].control >> 4 & 1) << i);
        return v;
    case 0xc6: return n->nr_c6;
    case 0xc8: return (uint8_t)(int_status_bit(n, SRC_LINE) << 1 | int_status_bit(n, SRC_ULA));
    case 0xc9:
        v = 0;
        for (i = 0; i < 8; i++)
            v |= (uint8_t)(int_status_bit(n, SRC_CTC0 + i) << i);
        return v;
    case 0xca:
        return (uint8_t)(int_status_bit(n, 13) << 6 | int_status_bit(n, 2) << 5 | int_status_bit(n, 2) << 4 |
                         int_status_bit(n, 12) << 2 | int_status_bit(n, 1) << 1 | int_status_bit(n, 1));
    case 0xcc: return n->nr_cc;
    case 0xcd: return n->nr_cd;
    case 0xce: return n->nr_ce;
    case 0xd8: return n->nr_d8;
    case 0xd9: return n->nr_d9;
    case 0xda: return n->nr_da;
    default: return 0;
    }
}

/* ---- contention (classic tables, 3.5 MHz only) -------------------------- */

static const zx_timing *classic_timing(const zxn *n)
{
    switch (timing_kind(n->eff_timing)) {
    case T48:  return &zx_timings[ZX_48K];
    case T128: return &zx_timings[ZX_128K];
    case TP3:  return &zx_timings[ZX_PLUS3];
    default:   return NULL;
    }
}

static int contention_active(const zxn *n)
{
    return n->speed == 0 && !n->eff_contention_off && timing_kind(n->eff_timing) != TPENT;
}

/* Wait states (in T-states) the ULA inserts for an access starting now. */
static uint32_t contend_delay(const zxn *n)
{
    const zx_timing *tm = classic_timing(n);
    int64_t t, c0, d, x;
    if (!tm)
        return 0;
    t = (int64_t)((n->clk - n->frame_start) >> 3);
    c0 = (int64_t)tm->contend_t - (int64_t)(64 - n->tm.min_vactive) * tm->line_t;
    if (t < c0)
        return 0;
    d = t - c0;
    if (d >= 192 * (int64_t)tm->line_t)
        return 0;
    x = d % tm->line_t;
    if (x >= 128)
        return 0;
    return tm->pattern[x & 7];
}

static inline void contend(zxn *n)
{
    n->clk += (uint64_t)contend_delay(n) << 3;
}

/* ---- events that fall due as time passes -------------------------------- */

static void copper_run(zxn *n, uint64_t upto);

static void events(zxn *n)
{
    if (n->clk >= n->line_int_clk) {
        raise_int_at(n, SRC_LINE, 0, n->line_int_clk);
        n->line_int_clk += n->frame_ticks;
    }
    if (n->copper_mode)
        copper_run(n, n->clk);
    if (ctc_next_event(n) <= n->clk)
        ctc_advance(n, n->clk);
    if (n->uart_next <= n->clk)
        nx_uart_run(n, n->clk);
}

/* ---- divMMC automap on opcode fetches (device/divmmc.vhd) --------------- */

static void automap_fetch(zxn *n, uint16_t a, int pre)
{
    int inst = 0, del = 0, off = 0, r3_inst = 0, r3_del = 0;
    int active, r3_active, now;
    const nx_slot *sl = &n->slot[a >> 13];

    if (!(n->port_en[1] & 1) || !(n->nr_0a & 0x10))
        return;
    if ((a & 0xff00) == 0x0000 && (a & 0xc7) == 0) {
        int i = (a >> 3) & 7;
        int ep = n->nr_b8 >> i & 1, valid = n->nr_b9 >> i & 1, timing = n->nr_ba >> i & 1;
        inst = ep & valid & timing;
        del = ep & valid & !timing;
        r3_inst = ep & !valid & timing;
        r3_del = ep & !valid & !timing;
    }
    if ((a & 0xff00) == 0x3d00 && (n->nr_bb & 0x80))
        r3_inst = 1;
    if (a >= 0x1ff8 && a <= 0x1fff && (n->nr_bb & 0x40))
        off = 1;
    if ((a == 0x04c6 && (n->nr_bb & 0x04)) || (a == 0x0562 && (n->nr_bb & 0x08)) ||
        (a == 0x04d7 && (n->nr_bb & 0x10)) || (a == 0x056a && (n->nr_bb & 0x20)))
        r3_del = 1;

    active = a < 0x4000 && !n->mf_enabled;
    r3_active = active && sl->rom3_ok;

    if (pre) {
        /* during the fetch: instant entry points page divMMC in at once */
        now = n->automap_held | (active & inst) | (r3_active & r3_inst);
        n->automap_hold = (uint8_t)((active & (inst | del)) | (r3_active & (r3_inst | r3_del)) |
                                    (n->automap_held & !(active & off)));
    } else {
        /* after the fetch: the held state takes over */
        n->automap_held = n->automap_hold;
        now = n->automap_held;
    }
    if (now != n->automap) {
        n->automap = (uint8_t)now;
        remap(n);
    }
}

/* ---- I/O ports ----------------------------------------------------------- */

static int p3_timing(const zxn *n) { return timing_kind(n->eff_timing) == TP3; }

static void spi_select(zxn *n, uint8_t v)
{
    int swap = (n->nr_0a >> 5) & 1;
    uint8_t old = n->port_e7, sel;
    if ((v & 3) == 2)
        sel = (uint8_t)(0xfc | (!swap) << 1 | swap);
    else if ((v & 3) == 1)
        sel = (uint8_t)(0xfc | swap << 1 | !swap);
    else if (v == 0xfb || v == 0xf7)
        sel = v;
    else if (v == 0x7f && n->config_mode)
        sel = 0x7f;
    else
        sel = 0xff;
    n->port_e7 = sel;
    if (!(old & 1) && (sel & 1))
        nx_sd_deselect(n->sd[0]);
    if (!(old & 2) && (sel & 2))
        nx_sd_deselect(n->sd[1]);
}

static uint8_t spi_xfer(zxn *n, uint8_t out)
{
    if (!(n->port_e7 & 1))
        return nx_sd_xfer(n->sd[0], out);
    if (!(n->port_e7 & 2))
        return nx_sd_xfer(n->sd[1], out);
    return 0xff;
}

static int dac_port(const zxn *n, uint8_t lsb, int ch)
{
    int sd1 = port_enabled(n, 17), sd2 = port_enabled(n, 18), pc = port_enabled(n, 19);
    int cv = port_enabled(n, 20), fb = port_enabled(n, 21) && !sd2, b3 = port_enabled(n, 22);
    int df = port_enabled(n, 23);
    switch (ch) {
    case 0: return (lsb == 0xfb && fb) || (lsb == 0xdf && df) || (lsb == 0x1f && sd1) || (lsb == 0xf1 && sd2) || (lsb == 0x3f && pc);
    case 1: return (lsb == 0xb3 && b3) || (lsb == 0x0f && (sd1 || cv)) || (lsb == 0xf3 && sd2);
    case 2: return (lsb == 0xb3 && b3) || (lsb == 0x4f && (sd1 || cv)) || (lsb == 0xf9 && sd2);
    default: return (lsb == 0xfb && fb) || (lsb == 0xdf && df) || (lsb == 0x5f && (sd1 || pc)) || (lsb == 0xfb && sd2);
    }
}

static int joy_on_1f(const zxn *n)
{
    return n->joy0 == 1 || n->joy0 == 5 || n->joy1 == 1 || n->joy1 == 5;
}

static void port_write(zxn *n, uint16_t port, uint8_t v)
{
    uint8_t lsb = (uint8_t)port, msb = (uint8_t)(port >> 8);
    int i;

    if (!(port & 1)) {
        sync_video(n);
        sync_audio(n);
        n->port_fe = v & 0x1f;
    }
    if (lsb == 0xff && port_enabled(n, 0)) {
        sync_video(n);
        n->port_ff = v;
    }
    if ((port & 3) == 1) {
        int conflict = (lsb == 0xf1 || lsb == 0xf9) && port_enabled(n, 18);
        int is_1ffd = (port & 0xf000) == 0x1000 && port_enabled(n, 3);
        int is_7ffd = !(port & 0x8000) && ((port & 0x4000) || !p3_timing(n)) && !is_1ffd && port_enabled(n, 1);
        int is_dffd = (port & 0xf000) == 0xd000 && port_enabled(n, 2);
        int is_ay = (port & 4) && port_enabled(n, 16);
        if (!conflict) {
            if (is_7ffd && !locked_7ffd(n)) {
                sync_video(n);
                n->port_7ffd = v;
                ports_changed(n, 0, 1);
            }
            if (is_dffd && !locked_7ffd(n)) {
                n->port_dffd = v & 0x1f;
                n->port_dffd_6 = (v >> 6) & 1;
                ports_changed(n, 0, 1);
            }
            if (is_1ffd && !locked_7ffd(n)) {
                int special_old = n->port_1ffd & 1;
                n->port_1ffd = v & 7;
                ports_changed(n, special_old, 1);
            }
        }
        if (is_ay && (port & 0xc000) == 0xc000 && !is_dffd) {
            if ((n->nr_08 & 2) && (v & 0x9c) == 0x9c) {
                int s = (v & 3) == 2 ? 1 : (v & 3) == 1 ? 2 : 0;
                n->ay_select = (uint8_t)s;
                n->ay_pan[s] = (v >> 5) & 3;
            } else if (!(v & 0xe0)) {
                n->ay[n->ay_select].sel = v;
            }
        } else if (is_ay && (port & 0xc000) == 0x8000) {
            sync_audio(n);
            ay_write(&n->ay[n->ay_select], v);
        }
    }
    if ((port & 0xf0ff) == 0xe0f7 && port_enabled(n, 26)) {
        n->port_eff7_2 = (v >> 2) & 1;
        n->port_eff7_3 = (v >> 3) & 1;
        ports_changed(n, 0, 1);
    }
    switch (lsb) {
    case 0xe3:
        if (port_enabled(n, 8)) {
            n->port_e3 = (uint8_t)((v & 0x8f) | ((v | n->port_e3) & 0x40));
            remap(n);
        }
        break;
    case 0xe7:
        if (port_enabled(n, 11))
            spi_select(n, v);
        break;
    case 0xeb:
        if (port_enabled(n, 11))
            n->spi_miso = spi_xfer(n, v);
        break;
    case 0x3b:
        if (msb == 0x24) {
            n->nr_register = v;
        } else if (msb == 0x25) {
            nr_write(n, n->nr_register, v);
        } else if (msb == 0x12 && port_enabled(n, 15)) {
            sync_video(n);
            if (!(v & 0x10)) {
                n->l2_en = (v >> 1) & 1;
                n->l2_map_wr = v & 1;
                n->l2_map_rd = (v >> 2) & 1;
                n->l2_shadow = (v >> 3) & 1;
                n->l2_segment = v >> 6;
            } else {
                n->l2_offset = v & 7;
            }
            remap(n);
        } else if (msb == 0x30 && port_enabled(n, 14)) {
            nx_sprite_port_303b(n, v);
        } else if (msb == 0xbf && port_enabled(n, 24)) {
            n->ulap_mode = v >> 6;
            if (!(v >> 6))
                n->ulap_index = v & 0x3f;
        } else if (msb == 0xff && port_enabled(n, 24)) {
            if (n->ulap_mode == 0)
                nr_write(n, 0xff, (uint8_t)((v & 0x1c) << 3 | (v & 0xe0) >> 3 | (v & 3)));
            else if (n->ulap_mode == 1)
                n->ulap_en = v & 1;
        } else if ((msb & 0xf8) == 0x18 && port_enabled(n, 27)) {
            if ((msb & 7) < 4)
                ctc_write(n, msb & 7, v);
        } else if ((msb >> 3) == 2 && ((msb >> 2 & 1) ^ ((msb >> 1) & msb & 1)) && port_enabled(n, 12)) {
            nx_uart_write(n, msb & 3, v);
        }
        break;
    case 0x57:
        if (port_enabled(n, 14))
            nx_sprite_port_57(n, v);
        break;
    case 0x5b:
        if (port_enabled(n, 14))
            nx_sprite_port_5b(n, v);
        break;
    case 0x6b:
    case 0x0b:
        if ((lsb == 0x6b && port_enabled(n, 5)) || (lsb == 0x0b && port_enabled(n, 25))) {
            n->dma.z80_mode = lsb == 0x0b;
            nx_dma_write(n, v);
        }
        break;
    default:
        break;
    }
    if (n->nr_08 & 8) {
        for (i = 0; i < 4; i++) {
            if (dac_port(n, lsb, i)) {
                sync_audio(n);
                n->dac[i] = v;
            }
        }
    }
}

/*
 * Kempston mouse (ps2_mouse.v): X counts up to the right, Y counts up
 * going up, the wheel is a 4-bit count, buttons read 0 when pressed.
 */
void nx_mouse(zxn *n, int dx, int dy, int dz, int buttons)
{
    int shift = n->nr_0a & 3;             /* DPI: x2, x1, /2, /4 */
    int x = n->mouse_fx + dx * 4, y = n->mouse_fy - dy * 4;
    int unit = shift == 0 ? 2 : shift == 1 ? 4 : shift == 2 ? 8 : 16;
    n->mouse_x = (uint8_t)(n->mouse_x + x / unit);
    n->mouse_y = (uint8_t)(n->mouse_y + y / unit);
    n->mouse_fx = x % unit;
    n->mouse_fy = y % unit;
    n->mouse_z = (uint8_t)(n->mouse_z + dz);
    n->mouse_btn = (uint8_t)(buttons & 7);
}

static uint8_t mouse_port(const zxn *n, int hi)
{
    int l = n->mouse_btn & 1, r = n->mouse_btn >> 1 & 1, mid = n->mouse_btn >> 2 & 1;
    if (n->nr_0a & 8) {                   /* button reverse */
        int t = l;
        l = r;
        r = t;
    }
    if (hi == 0x0b)
        return n->mouse_x;
    if (hi == 0x0f)
        return n->mouse_y;
    return (uint8_t)((n->mouse_z & 15) << 4 | 8 | !mid << 2 | !l << 1 | !r);
}

static uint8_t port_read(zxn *n, uint16_t port)
{
    zx *m = n->m;
    uint8_t lsb = (uint8_t)port, msb = (uint8_t)(port >> 8);
    uint8_t r = 0;
    int hit = 0, i;

    if (!(port & 1)) {
        uint8_t k = 0x1f;
        for (i = 0; i < 8; i++)
            if (!(msb & (1 << i)))
                k &= m->keys[i];
        r |= (uint8_t)(0xa0 | k | ((m->ear_in | (n->port_fe >> 4 & 1)) << 6));
        hit = 1;
    }
    if (lsb == 0xff) {
        int k = timing_kind(n->eff_timing);
        if ((n->nr_08 & 4) && port_enabled(n, 0))
            r |= n->port_ff;
        else if (k == T48 || k == T128)
            r |= nx_floating_bus(n);
        else
            r |= 0xff;
        hit = 1;
    }
    if ((port & 0xf003) == 0x0001 && p3_timing(n) && port_enabled(n, 4)) {
        r |= locked_7ffd(n) ? 0xff : nx_floating_bus(n);
        hit = 1;
    }
    if ((port & 3) == 1 && (port & 4) && port_enabled(n, 16)) {
        int fffd = (port & 0xc000) == 0xc000;
        int bffd = (port & 0xc000) == 0x8000;
        if (fffd || (bffd && p3_timing(n)) || (bffd && !(port & 8))) {
            r |= ay_read(&n->ay[n->ay_select]);
            hit = 1;
        }
    }
    switch (lsb) {
    case 0xe3:
        if (port_enabled(n, 8)) {
            r |= n->port_e3 & 0xcf;
            hit = 1;
        }
        break;
    case 0xeb:
        if (port_enabled(n, 11)) {
            r |= n->spi_miso;
            n->spi_miso = spi_xfer(n, 0xff);
            hit = 1;
        }
        break;
    case 0x3b:
        if (msb == 0x24) {
            r |= n->nr_register;
            hit = 1;
        } else if (msb == 0x25) {
            r |= nr_read(n, n->nr_register);
            hit = 1;
        } else if ((msb == 0x10 || msb == 0x11) && port_enabled(n, 10)) {
            r |= 0xff;      /* I2C: nothing answers, the lines float high */
            hit = 1;
        } else if (msb == 0x12 && port_enabled(n, 15)) {
            r |= (uint8_t)(n->l2_segment << 6 | n->l2_shadow << 3 | n->l2_map_rd << 2 | n->l2_en << 1 | n->l2_map_wr);
            hit = 1;
        } else if ((msb >> 3) == 2 && ((msb >> 2 & 1) ^ ((msb >> 1) & msb & 1)) && port_enabled(n, 12)) {
            r |= nx_uart_read(n, msb & 3);
            hit = 1;
        } else if (msb == 0x30 && port_enabled(n, 14)) {
            r |= n->spr_status;
            n->spr_status = 0;
            hit = 1;
        } else if (msb == 0xff && port_enabled(n, 24)) {
            if (n->ulap_mode == 0) {
                uint16_t c = n->pal_ulatm[((n->nr_43 >> 6) & 1) << 8 | 0xc0 | n->ulap_index];
                r |= (uint8_t)((c >> 3 & 7) << 5 | (c >> 6 & 7) << 2 | (c >> 1 & 3));
            } else {
                r |= n->ulap_en;
            }
            hit = 1;
        } else if ((msb & 0xf8) == 0x18 && port_enabled(n, 27)) {
            if ((msb & 7) < 4)
                r |= ctc_read(n, msb & 7);
            hit = 1;
        }
        break;
    case 0x6b:
    case 0x0b:
        if ((lsb == 0x6b && port_enabled(n, 5)) || (lsb == 0x0b && port_enabled(n, 25))) {
            r |= nx_dma_read(n);
            hit = 1;
        }
        break;
    case 0xdf:
        if (port_enabled(n, 13) && ((msb & 15) == 0x0a || (msb & 15) == 0x0b || (msb & 15) == 0x0f)) {
            r |= mouse_port(n, msb & 15);
            hit = 1;
        }
        break;
    case 0x1f:
        if (port_enabled(n, 6) && joy_on_1f(n)) {
            r |= m->kempston;
            hit = 1;
        }
        break;
    default:
        break;
    }
    return hit ? r : 0xff;
}

/* ---- bus callbacks -------------------------------------------------------- */

static uint8_t b_fetch(void *ctx, uint16_t addr)
{
    zx *m = ctx;
    zxn *n = m->next;
    nx_slot *sl;
    uint8_t v;

    events(n);
    if (contention_active(n) && n->slot[addr >> 13].contend)
        contend(n);
    automap_fetch(n, addr, 1);
    sl = &n->slot[addr >> 13];
    v = sl->rd[addr & 0x1fff];
    n->clk += 4u * n->tick + (n->speed == 3 && sl->wait);
    automap_fetch(n, addr, 0);
    return v;
}

static uint8_t b_read(void *ctx, uint16_t addr)
{
    zx *m = ctx;
    zxn *n = m->next;
    nx_slot *sl = &n->slot[addr >> 13];

    events(n);
    if (contention_active(n) && sl->contend)
        contend(n);
    n->clk += 3u * n->tick + (n->speed == 3 && sl->wait);
    return sl->rd[addr & 0x1fff];
}

static void b_write(void *ctx, uint16_t addr, uint8_t v)
{
    zx *m = ctx;
    zxn *n = m->next;
    nx_slot *sl = &n->slot[addr >> 13];

    events(n);
    if (contention_active(n) && sl->contend)
        contend(n);
    n->clk += 3u * n->tick;
    if (sl->video)
        sync_video(n);
    sl->wr[addr & 0x1fff] = v;
}

static void b_internal(void *ctx, uint16_t addr, unsigned cycles)
{
    zx *m = ctx;
    zxn *n = m->next;
    const zx_timing *tm = classic_timing(n);

    if (tm && tm->contend_internal && contention_active(n) && n->slot[addr >> 13].contend) {
        while (cycles--) {
            contend(n);
            n->clk += n->tick;
        }
    } else {
        n->clk += (uint64_t)cycles * n->tick;
    }
}

static void b_tick(void *ctx, unsigned cycles)
{
    zx *m = ctx;
    zxn *n = m->next;
    n->clk += (uint64_t)cycles * n->tick;
}

/* I/O cycle timing: the classic ULA patterns, at 3.5 MHz on 48K/128K timing. */
static void io_timing(zxn *n, uint16_t port)
{
    const zx_timing *tm = classic_timing(n);
    int ula_port = !(port & 1) || ((port & 0xff) == 0x3b && ((port >> 8) == 0xbf || (port >> 8) == 0xff));
    int k = timing_kind(n->eff_timing);

    if ((port & 3) == 1 && !(port & 0x8000) && ((port & 0x4000) || !p3_timing(n)) && (k == T128 || k == TP3))
        ula_port = 1;   /* port_7ffd_active */

    if (!tm || !tm->contend_io || !contention_active(n)) {
        n->clk += 4u * n->tick;
        return;
    }
    if (n->slot[port >> 13].contend) {
        if (!ula_port) {
            for (int i = 0; i < 4; i++) {
                contend(n);
                n->clk += 8;
            }
        } else {
            contend(n);
            n->clk += 8;
            contend(n);
            n->clk += 24;
        }
    } else if (!ula_port) {
        n->clk += 32;
    } else {
        n->clk += 8;
        contend(n);
        n->clk += 24;
    }
}

static uint8_t b_io_read(void *ctx, uint16_t port)
{
    zx *m = ctx;
    zxn *n = m->next;
    events(n);
    io_timing(n, port);
    return port_read(n, port);
}

static void b_io_write(void *ctx, uint16_t port, uint8_t v)
{
    zx *m = ctx;
    zxn *n = m->next;
    events(n);
    io_timing(n, port);
    port_write(n, port, v);
}

uint8_t nx_mem_read(zxn *n, uint16_t addr)
{
    return n->slot[addr >> 13].rd[addr & 0x1fff];
}

void nx_mem_write(zxn *n, uint16_t addr, uint8_t v)
{
    nx_slot *sl = &n->slot[addr >> 13];
    if (sl->video)
        sync_video(n);
    sl->wr[addr & 0x1fff] = v;
}

uint8_t nx_io_read(zxn *n, uint16_t port)
{
    return port_read(n, port);
}

void nx_io_write(zxn *n, uint16_t port, uint8_t v)
{
    port_write(n, port, v);
}

static void b_nextreg(void *ctx, uint8_t reg, uint8_t v)
{
    zx *m = ctx;
    nr_write(m->next, reg, v);
}

/* ---- copper execution ---------------------------------------------------- */

#define NEVER (~(uint64_t)0)

static int cvc_now(zxn *n)
{
    int hc, vc;
    ula_pos(n, (n->clk - n->frame_start) >> 2, &hc, &vc);
    return (vc + n->copper_offset) % n->tm.vc;
}

static void copper_restart_point(zxn *n)
{
    const nx_timing *t = &n->tm;
    int vc_ula = ((0 - n->copper_offset) % t->vc + t->vc) % t->vc;
    uint64_t r = n->frame_start + (uint64_t)ula_pix(n, 0, vc_ula) * 4;
    while (r < n->clk)
        r += n->frame_ticks;
    n->cu_restart = r;
}

/* First tick at or after `from` when a WAIT for (cvc, hc_ula) is satisfied. */
static uint64_t wait_time(const zxn *n, uint16_t ins, uint64_t from)
{
    const nx_timing *t = &n->tm;
    int v = ins & 0x1ff, h = ((ins >> 9) & 63) * 8 + 12;
    int64_t total = (int64_t)t->hc * t->vc, len, s, rel, p;
    int vct;
    if (v >= t->vc || h >= t->hc)
        return NEVER;
    vct = ((v - n->copper_offset) % t->vc + t->vc) % t->vc;
    s = ula_pix(n, h, vct);
    len = t->hc - h;
    p = (int64_t)((from - n->frame_start) >> 2);
    rel = p % total;
    if (((rel - s) % total + total) % total < len)
        return from;
    p += ((s - rel) % total + total) % total;
    return n->frame_start + (uint64_t)p * 4;
}

static void copper_run(zxn *n, uint64_t upto)
{
    uint64_t save = n->clk;
    while (n->copper_mode && n->cu_clk <= upto) {
        uint16_t ins;
        if (n->copper_mode == 3 && n->cu_restart <= n->cu_clk) {
            n->cu_pc = 0;
            n->cu_restart += n->frame_ticks;
        }
        ins = n->copper[n->cu_pc];
        if (ins & 0x8000) {
            uint64_t t = wait_time(n, ins, n->cu_clk);
            if (n->copper_mode == 3 && t > n->cu_restart)
                t = n->cu_restart;
            if (t != n->cu_clk) {
                n->cu_clk = t;
                continue;
            }
            n->cu_pc = (n->cu_pc + 1) & 1023;
            n->cu_clk++;
        } else if (ins & 0x7f00) {
            n->clk = n->cu_clk;
            n->cu_pc = (n->cu_pc + 1) & 1023;
            n->cu_clk += 2;
            nr_write(n, (uint8_t)(ins >> 8 & 0x7f), (uint8_t)ins);
        } else {
            /* a run of NOPs: one tick each */
            uint64_t k = 1, avail = upto - n->cu_clk + 1;
            while (k < 1024 && !(n->copper[(n->cu_pc + k) & 1023] & 0xff00))
                k++;
            if (k == 1024)
                k = avail;
            if (n->copper_mode == 3 && n->cu_clk + k > n->cu_restart)
                k = n->cu_restart - n->cu_clk;
            if (k > avail)
                k = avail;
            if (k == 0)
                k = 1;
            n->cu_pc = (uint16_t)((n->cu_pc + k) & 1023);
            n->cu_clk += k;
        }
    }
    n->clk = save;
}

/* ---- audio (audio/audio_mixer.vhd, audio/turbosound.vhd) ---------------- */

#define NX_MASTER_HZ 28000000u

static void audio_accumulate(zxn *n, uint64_t dt)
{
    int32_t ear = (n->port_fe & 0x10) ? 512 : 0, mic = (n->port_fe & 0x08) ? 128 : 0;
    int32_t tape = n->m->ear_in ? 128 : 0;
    int32_t l = ear + mic + tape + ((int32_t)n->dac[0] + n->dac[1]) * 4;
    int32_t r = ear + mic + tape + ((int32_t)n->dac[2] + n->dac[3]) * 4;
    n->acc_l += (int64_t)l * (int64_t)dt;
    n->acc_r += (int64_t)r * (int64_t)dt;
    if ((n->nr_06 & 3) != 3) {
        uint64_t c = (n->ay_div + dt) >> 4;   /* AY clock: 28 MHz / 16 */
        int i;
        n->ay_div = (uint32_t)((n->ay_div + dt) & 15);
        while (c) {
            uint32_t step = c > 0x100000 ? 0x100000 : (uint32_t)c;
            for (i = 0; i < 3; i++)
                ay_run(&n->ay[i], step);
            c -= step;
        }
    }
}

static void audio_sample(zxn *n)
{
    zx *m = n->m;
    uint64_t span = n->audio_next - n->audio_prev;
    int32_t l, r, ch[3];
    int i;

    if (!span)
        span = 1;
    l = (int32_t)(n->acc_l / (int64_t)span);
    r = (int32_t)(n->acc_r / (int64_t)span);
    n->acc_l = n->acc_r = 0;

    for (i = 0; i < 3; i++) {
        int a, b, c, mono = (n->nr_09 >> (5 + i)) & 1, acb = (n->nr_08 >> 5) & 1;
        int lsum, rsum, lfin;
        ay_take(&n->ay[i], ch);
        if ((n->nr_06 & 3) == 3)
            continue;
        if (i != n->ay_select && !(n->nr_08 & 2))
            continue;
        a = ch[0] * 255 / 32767; b = ch[1] * 255 / 32767; c = ch[2] * 255 / 32767;
        lsum = ((acb || mono) ? c : b) + a;
        rsum = (mono ? lsum : c) + b;
        lfin = mono ? rsum : lsum;
        if (n->ay_pan[i] & 2) l += lfin;
        if (n->ay_pan[i] & 1) r += rsum;
    }
    /* 0..6000 full scale */
    l *= 5;
    r *= 5;
    if (m->audio_samples < (int)(sizeof m->audio / sizeof m->audio[0] / 2)) {
        m->audio[m->audio_samples * 2] = (int16_t)(l > 32767 ? 32767 : l);
        m->audio[m->audio_samples * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r);
        m->audio_samples++;
    }
}

static void audio_run(zxn *n, uint64_t upto)
{
    const uint64_t step = ((uint64_t)NX_MASTER_HZ << 32) / ZX_AUDIO_RATE;
    for (;;) {
        uint64_t next = n->audio_next;
        uint64_t end = next < upto ? next : upto;
        if (end > n->audio_clk) {
            audio_accumulate(n, end - n->audio_clk);
            n->audio_clk = end;
        }
        if (next > upto)
            return;
        audio_sample(n);
        n->audio_prev = next;
        n->audio_frac += step & 0xffffffffu;
        n->audio_next += (step >> 32) + (n->audio_frac >> 32);
        n->audio_frac &= 0xffffffffu;
    }
}

/* ---- reset and setup ------------------------------------------------------ */

static void ctc_reset(zxn *n)
{
    int i;
    for (i = 0; i < 4; i++)
        memset(&n->ctc[i], 0, sizeof n->ctc[i]);
}

/* The `reset = '1'` branches of zxnext.vhd and its devices. */
void nx_soft_reset(zx *m)
{
    zxn *n = m->next;
    int i;
    static const uint8_t clip_def[4][4] = {
        { 0, 255, 0, 191 }, { 0, 255, 0, 191 }, { 0, 255, 0, 191 }, { 0, 159, 0, 255 },
    };

    n->nr_06 = (uint8_t)((n->nr_06 & 0x5f) | 0xa0);
    n->nr_08 &= (uint8_t)~0x40;
    n->nr_09 &= (uint8_t)~0x10;
    n->nr_0b = 0x01;
    n->l2_bank = 8;
    n->l2_shadow_bank = 11;
    n->transparent = 0xe3;
    n->nr_15 = 0;
    n->l2_scroll_x = n->l2_scroll_y = n->l2_scroll_x_msb = 0;
    memcpy(n->clip, clip_def, sizeof n->clip);
    memset(n->clip_idx, 0, sizeof n->clip_idx);
    n->line_int_en = 0;
    n->line_int = 0;
    n->ula_scroll_x = n->ula_scroll_y = 0;
    n->tm_scroll_x = 0;
    n->tm_scroll_y = 0;
    n->lores_scroll_x = n->lores_scroll_y = 0;
    n->pal_idx = 0;
    n->pal_sub = 0;
    n->ulanext_format = 7;
    n->nr_43 = 0;
    n->pal_stored = 0;
    n->fallback = 0xe3;
    n->sprite_transp = 0xe3;
    n->tm_transp = 0x0f;
    n->copper_mode = 0;
    n->copper_addr = 0;
    n->copper_stored = 0;
    n->copper_offset = 0;
    n->cu_pc = 0;
    n->nr_68 = 0;
    n->nr_6a = 0;
    n->nr_6b = 0;
    n->nr_6c = 0;
    n->nr_6e = 0x2c;
    n->nr_6f = 0x0c;
    n->nr_70 = 0;
    if (n->nr_85_reset_type) {
        memset(n->port_en, 0xff, 3);
        n->port_en[3] = 0x0f;
    }
    if (!n->nr_89_reset_type) {
        memset(n->bus_en, 0xff, 3);
        n->bus_en[3] = 0x0f;
    }
    n->nr_b8 = 0x83;
    n->nr_b9 = 0x01;
    n->nr_ba = 0x00;
    n->nr_bb = 0xcd;
    n->nr_c0 = 0;
    n->nr_c4_expbus = 1;
    n->nr_c6 = 0;
    n->nr_cc = n->nr_cd = n->nr_ce = 0;
    n->nr_d8 = 0;
    n->nr_d9 = n->nr_da = 0;
    n->nr_02 = 0;
    n->retn_lsb = n->retn_msb = 0;
    if (n->config_mode)
        n->bootrom_en = 1;
    n->nr_80 = (uint8_t)((n->nr_80 & 0x0f) | n->nr_80 << 4);
    n->nr_8c = (uint8_t)((n->nr_8c & 0x0f) | n->nr_8c << 4);
    n->nr_07_speed = 0;
    set_speed(n, 0);
    n->nr_register = 0x24;

    n->port_fe = 0;
    n->port_ff = 0;
    n->port_7ffd = n->port_dffd = n->port_dffd_6 = n->port_1ffd = 0;
    n->port_eff7_2 = n->port_eff7_3 = 0;
    n->port_e3 = 0;
    n->port_e7 = 0xff;
    n->spi_miso = 0xff;
    n->l2_en = n->l2_map_wr = n->l2_map_rd = n->l2_shadow = n->l2_segment = n->l2_offset = 0;
    n->ulap_mode = n->ulap_index = n->ulap_en = 0;
    n->mmu[0] = 0xff; n->mmu[1] = 0xff; n->mmu[2] = 0x0a; n->mmu[3] = 0x0b;
    n->mmu[4] = 0x04; n->mmu[5] = 0x05; n->mmu[6] = 0x00; n->mmu[7] = 0x01;
    automap_reset(n);
    n->mf_enabled = 0;

    memset(n->im2_state, 0, sizeof n->im2_state);
    memset(n->int_status, 0, sizeof n->int_status);
    n->pulse_start = n->pulse_end = 0;
    ctc_reset(n);
    nx_dma_reset(n);
    nx_uart_reset(n);
    for (i = 0; i < 3; i++) {
        ay_reset(&n->ay[i]);
        n->ay_pan[i] = 3;
    }
    n->ay_select = 0;
    memset(n->dac, 0x80, sizeof n->dac);
    nx_video_reset(n);

    z80_reset(&m->cpu);
    n->reset_req = 0;
    remap(n);
    schedule_line_int(n);
    copper_restart_point(n);
}

void nx_power_on(zx *m)
{
    zxn *n = m->next;
    n->config_mode = 1;
    n->bootrom_en = 1;
    n->machine_timing = 3;
    n->machine_type = 3;
    n->eff_timing = 3;
    n->user_dt_lock = 0;
    n->reset_type = 4;
    n->romram_bank = 0;
    n->nr_05_5060 = 0;
    n->eff_5060 = 0;
    n->nr_05_scandouble = 1;
    n->nr_08 = 0x10;
    n->nr_09 = 0;
    n->nr_0a = 0x01;          /* mouse DPI 01 */
    n->nr_06 = 0;
    n->nr_80 = n->nr_8c = 0;
    n->nr_81 = 0;
    n->mapping_mode = 0;
    n->nr_85_reset_type = 1;
    n->nr_89_reset_type = 0;
    n->joy0 = n->joy1 = 0;
    n->nr_10_coreid = 0;
    n->nr_11 = 0;
    n->nr_7f = 0;
    set_timing(n);
    palette_defaults(n);
    memset(n->copper, 0, sizeof n->copper);
    nx_uart_power_on(n);
    nx_soft_reset(m);
}

int nx_init(zx *m)
{
    zxn *n = calloc(1, sizeof *n);
    if (!n)
        return -1;
    n->sram = calloc(1, NX_SRAM_SIZE);
    if (!n->sram) {
        free(n);
        return -1;
    }
    n->m = m;
    m->next = n;
    memcpy(n->bootrom, nx_bootrom_data, sizeof n->bootrom);
    memset(n->ff_page, 0xff, sizeof n->ff_page);
    n->line_int_clk = NEVER;
    n->cu_clk = NEVER;

    z80_init(&m->cpu, 1);
    m->bus.ctx = m;
    m->bus.fetch = b_fetch;
    m->bus.read = b_read;
    m->bus.write = b_write;
    m->bus.io_read = b_io_read;
    m->bus.io_write = b_io_write;
    m->bus.internal = b_internal;
    m->bus.tick = b_tick;
    m->bus.int_ack = b_int_ack;
    m->bus.nextreg_write = b_nextreg;
    m->bus.reti = b_reti;
    m->bus.retn = b_retn;

    m->pixels = &n->screen[0][0];
    m->screen_w = NX_W;
    m->screen_h = NX_H;
    m->pitch = NX_W;

    nx_power_on(m);
    return 0;
}

void nx_free(zx *m)
{
    zxn *n = m->next;
    int i;
    if (!n)
        return;
    nx_sd_close(n->sd[0]);
    nx_sd_close(n->sd[1]);
    for (i = 0; i < 2; i++)
        if (n->uart[i].dev && n->uart[i].dev->close)
            n->uart[i].dev->close(n->uart[i].dev);
    free(n->sram);
    free(n);
    m->next = NULL;
}

int nx_attach_sd_dev(zx *m, int slot, struct zx_blockdev *bd)
{
    zxn *n = m->next;
    nx_sd *sd = nx_sd_new(bd);
    if (!sd)
        return -1;
    nx_sd_close(n->sd[slot & 1]);
    n->sd[slot & 1] = sd;
    return 0;
}

void nx_attach_serial(zx *m, int uart, struct zx_serial *dev)
{
    zxn *n = m->next;
    nx_uart *u = &n->uart[uart & 1];
    if (u->dev && u->dev->close)
        u->dev->close(u->dev);
    u->dev = dev;
    u->rx_free = n->clk;
}

void nx_set_bootrom(zx *m, const uint8_t data[0x2000])
{
    memcpy(m->next->bootrom, data, sizeof m->next->bootrom);
}

/* ---- state --------------------------------------------------------------- */

/* zxn struct, SRAM, then each attached card's SPI state. Pointers in the
 * saved struct are meaningless; load keeps the live ones and rebuilds the
 * memory map from the saved registers. */
size_t nx_state_size(const zx *m)
{
    (void)m;
    return sizeof(zxn) + NX_SRAM_SIZE + 2 * nx_sd_state_size();
}

void nx_state_save(const zx *m, uint8_t *out)
{
    const zxn *n = m->next;
    int i;
    memcpy(out, n, sizeof *n);
    out += sizeof *n;
    memcpy(out, n->sram, NX_SRAM_SIZE);
    out += NX_SRAM_SIZE;
    for (i = 0; i < 2; i++, out += nx_sd_state_size())
        if (n->sd[i])
            nx_sd_state_save(n->sd[i], out);
        else
            memset(out, 0, nx_sd_state_size());
}

void nx_state_load(zx *m, const uint8_t *in, int has_sd[2])
{
    zxn *n = m->next;
    uint8_t *sram = n->sram;
    nx_sd *sd[2];
    struct zx_serial *dev[2];
    int i;
    sd[0] = n->sd[0];
    sd[1] = n->sd[1];
    dev[0] = n->uart[0].dev;
    dev[1] = n->uart[1].dev;
    memcpy(n, in, sizeof *n);
    in += sizeof *n;
    n->m = m;
    n->sram = sram;
    memcpy(n->sram, in, NX_SRAM_SIZE);
    in += NX_SRAM_SIZE;
    for (i = 0; i < 2; i++, in += nx_sd_state_size()) {
        n->sd[i] = sd[i];
        n->uart[i].dev = dev[i];
        if (sd[i] && has_sd[i])
            nx_sd_state_load(sd[i], in);
    }
    remap(n);
}

/* ---- the frame loop ------------------------------------------------------- */

static void new_frame(zxn *n)
{
    n->eff_timing = n->machine_timing;
    n->eff_5060 = n->nr_05_5060;
    n->eff_contention_off = (n->nr_08 >> 6) & 1;
    set_timing(n);
    remap(n);
    raise_int_at(n, SRC_ULA, 0, n->frame_start);
    schedule_line_int(n);
    copper_restart_point(n);
}

void nx_run_frame(zx *m)
{
    zxn *n = m->next;
    uint64_t end = n->frame_start + n->frame_ticks;

    m->audio_samples = 0;
    while (n->clk < end) {
        if (n->reset_req) {
            if (n->reset_req == 2) {
                n->config_mode = 1;
                n->bootrom_en = 1;
                n->machine_timing = 3;
                n->machine_type = 3;
                n->reset_type = 2;
            } else {
                n->reset_type = 1;
            }
            nx_soft_reset(m);
        }
        if (n->speed != n->nr_07_speed)
            set_speed(n, n->nr_07_speed);
        events(n);
        if (n->dma.active && nx_dma_run(n))
            continue;
        if (int_line(n) && z80_int_enabled(&m->cpu)) {
            z80_interrupt(&m->cpu, &m->bus);
            continue;
        }
        z80_step(&m->cpu, &m->bus);
    }

    nx_video_run(n, end);
    nx_video_frame_end(n);
    audio_run(n, end);
    nx_uart_poll(n, end);
    nx_sd_tick(n->sd[0]);
    nx_sd_tick(n->sd[1]);

    n->frame_start = end;
    n->frames++;
    m->frames++;
    new_frame(n);
}
