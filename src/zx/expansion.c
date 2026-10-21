/*
 * Interfaces on the classic Spectrums' expansion port (expansion.h).
 *
 * divMMC behaviour follows the original divMMC / divIDE and the Next's
 * device/divmmc.vhd: CONMEM pages the ROM at 0000 and the selected RAM
 * bank at 2000; otherwise the memory pages in on an opcode fetch from an
 * entry point and out after a fetch from 1FF8-1FFF.
 */
#include "expansion.h"
#include "zx.h"
#include "../next/next.h"
#include "../storage/blockdev.h"

#include <stdlib.h>
#include <string.h>

static uint8_t giga_mode(const zx_exp *e);
static void giga_frame(zx *m);

static void free_wide(zx *m, zx_exp *e)
{
    if (!e->own_wide)
        return;
    free(m->wide);
    m->wide = NULL;
    m->pixels = &m->screen[0][0];
    m->screen_w = m->pitch = ZX_SCREEN_W;
}

static void update_map(zx *m)
{
    const zx_exp *e = m->exp;
    m->div_map = (uint8_t)((e->features & ZX_EXP_DIVMMC) && ((e->port_e3 & 0x80) || e->automap));
}

int zx_exp_attach(zx *m, uint32_t features, const uint8_t *div_rom, size_t rom_len)
{
    zx_exp *e;
    if (m->model == ZX_NEXT || m->exp)
        return -1;
    if ((features & ZX_EXP_DIVMMC) && (!div_rom || rom_len != sizeof e->div_rom))
        return -1;
    e = calloc(1, sizeof *e);
    if (!e)
        return -1;
    e->features = features;
    if (features & ZX_EXP_DIVMMC)
        memcpy(e->div_rom, div_rom, sizeof e->div_rom);
    /* Timex hi-res needs a 640-wide picture (the ATM has its own) */
    if ((features & ZX_EXP_TIMEX) && !m->wide) {
        m->wide = calloc((size_t)ZX_SCREEN_W * 2 * ZX_SCREEN_H, sizeof *m->wide);
        if (!m->wide) {
            free(e);
            return -1;
        }
        e->own_wide = 1;
        m->pixels = m->wide;
        m->screen_w = m->pitch = ZX_SCREEN_W * 2;
    }
    if ((features & ZX_EXP_GIGA)
        && !(e->giga_prev = calloc((size_t)m->pitch * ZX_SCREEN_H, sizeof *e->giga_prev))) {
        free_wide(m, e);
        free(e);
        return -1;
    }
    m->exp = e;
    zx_exp_power_on(m);
    return 0;
}

void zx_exp_free(zx *m)
{
    zx_exp *e = m->exp;
    if (!e)
        return;
    nx_sd_close(e->sd[0]);
    nx_sd_close(e->sd[1]);
    ata_free(&e->ide);
    free_wide(m, e);
    free(e->giga_prev);
    free(e->gs);
    free(e);
    m->exp = NULL;
    m->div_map = 0;
}

int zx_exp_attach_gs(zx *m, const uint8_t *rom, size_t rom_len)
{
    zx_exp *e = m->exp;
    if (!e || !(e->features & ZX_EXP_ZXI) || e->gs || !rom || rom_len != GS_ROM)
        return -1;
    if (!(e->gs = malloc(sizeof *e->gs)))
        return -1;
    gs_init(e->gs, rom);
    e->features |= ZX_EXP_GS;
    return 0;
}

void zx_exp_power_on(zx *m)
{
    zx_exp *e = m->exp;
    if (!e)
        return;
    memset(e->div_ram, 0, sizeof e->div_ram);
    e->port_e3 = 0;
    memset(e->zxi, 0, sizeof e->zxi);
    memset(e->gzxi, 0, sizeof e->gzxi);
    e->zxi[0][0x00] = 2;         /* divSD (esxDOS) */
    e->zxi[0][0x01] = ZXI_AUDIO_DEFAULT;
    e->zxi[0][0x03] = 1;         /* RTC */
    e->zxi[0][0x06] = 1;         /* SD */
    e->zxi[0][0x07] = 1;         /* Kempston */
    e->zxi[0][0x0b] = 3;         /* GigaScreen: automatic */
    e->zxi[0][0x0c] = 1;         /* Timex modes */
    e->zxi[0][0x0d] = 1;         /* ULAplus */
    sid_set_model(&e->sid, 0, 0);
    if (e->gs)
        memset(e->gs->ram, 0, sizeof e->gs->ram);
    zx_exp_reset(m);
}

void zx_exp_reset(zx *m)
{
    zx_exp *e = m->exp;
    int i;
    if (!e)
        return;
    e->port_e3 &= 0x40;          /* MAPRAM survives a reset */
    e->port_e7 = 0xff;
    e->spi_miso = 0xff;
    e->automap = 0;
    nx_sd_deselect(e->sd[0]);
    nx_sd_deselect(e->sd[1]);
    for (i = 0; i < 3; i++)
        ay_reset(&e->ay[i]);
    e->ay_select = 0;
    memset(e->dac, 0x80, sizeof e->dac);
    e->port_ff = 0;
    saa_reset(&e->saa);
    sid_reset(&e->sid);
    e->saa_frac = e->sid_frac = e->gs_frac = 0;
    if (e->gs)
        gs_reset(e->gs);
    e->sid_sel = 0;
    e->zxi_sel = e->gzxi_sel = 0;
    memset(e->uno, 0, sizeof e->uno);
    e->uno_sel = e->uno_hi = e->uno_id = 0;
    e->radas_offset = 0;
    e->giga_flips = 0;
    ata_reset(&e->ide);
    e->ide_latch = e->ide_odd = 0;
    update_map(m);
}

void zx_exp_frame_end(zx *m)
{
    zx_exp *e = m->exp;
    nx_sd_tick(e->sd[0]);
    nx_sd_tick(e->sd[1]);
    ata_tick(&e->ide);
    e->rtc_t += m->tm->frame_t;
    if (giga_mode(e) >= 2 && e->giga_prev)
        giga_frame(m);
}

int zx_exp_attach_ide(zx *m, int unit, zx_blockdev *bd)
{
    zx_exp *e = m->exp;
    if (!e || !(e->features & ZX_EXP_IDE) || unit < 0 || unit > 1)
        return -1;
    ata_attach(&e->ide, unit, bd);
    return 0;
}

int zx_exp_attach_sd(zx *m, int slot, zx_blockdev *bd)
{
    zx_exp *e = m->exp;
    nx_sd *sd = NULL;
    if (!e || !(e->features & ZX_EXP_DIVMMC) || slot < 0 || slot > 1)
        return -1;
    if (bd && !(sd = nx_sd_new(bd)))
        return -1;
    nx_sd_close(e->sd[slot]);
    e->sd[slot] = sd;
    return 0;
}

/* ---- divMMC memory -------------------------------------------------------- */

uint8_t zx_exp_read(const zx *m, uint16_t addr)
{
    const zx_exp *e = m->exp;
    if (addr >= 0x2000)
        return e->div_ram[e->port_e3 & (ZX_DIV_BANKS - 1)][addr & 0x1fff];
    if (!(e->port_e3 & 0x80) && (e->port_e3 & 0x40))
        return e->div_ram[3][addr];
    return e->div_rom[addr];
}

void zx_exp_write(zx *m, uint16_t addr, uint8_t v)
{
    zx_exp *e = m->exp;
    int bank = e->port_e3 & (ZX_DIV_BANKS - 1);
    /* The ROM never takes writes; with MAPRAM, neither does bank 3 unless
     * CONMEM is set. */
    if (addr < 0x2000)
        return;
    if (bank == 3 && (e->port_e3 & 0xc0) == 0x40)
        return;
    e->div_ram[bank][addr & 0x1fff] = v;
}

/* The 48K BASIC ROM is paged in at 0000 (TR-DOS's 3Dxx entry only
 * counts then, as on the Next). */
static int rom48_paged(const zx *m)
{
    int r = m->model == ZX_48K ? 0 : (m->model == ZX_PLUS2A || m->model == ZX_PLUS3) ? 3 : 1;
    return m->rd[0] == m->rom[r];
}

static int entry_point(uint16_t a)
{
    return a == 0x0000 || a == 0x0008 || a == 0x0038 || a == 0x0066 || a == 0x04c6 || a == 0x0562;
}

uint8_t zx_exp_fetch(zx *m, uint16_t addr)
{
    zx_exp *e = m->exp;
    uint8_t v;
    if (!(e->features & ZX_EXP_DIVMMC))
        return m->rd[addr >> 14][addr & 0x3fff];
    /* 3D00-3DFF (the TR-DOS entry) pages in during the fetch itself */
    if ((addr & 0xff00) == 0x3d00 && !e->automap && rom48_paged(m)) {
        e->automap = 1;
        update_map(m);
    }
    v = (addr < 0x4000 && m->div_map) ? zx_exp_read(m, addr) : m->rd[addr >> 14][addr & 0x3fff];
    /* the other entry points page in after the fetch; 1FF8-1FFF page out */
    if (addr < 0x4000) {
        if (entry_point(addr) && !e->automap) {
            e->automap = 1;
            update_map(m);
        } else if (addr >= 0x1ff8 && addr <= 0x1fff && e->automap) {
            e->automap = 0;
            update_map(m);
        }
    }
    return v;
}

/* ---- I/O ------------------------------------------------------------------ */

static uint8_t spi_xfer(zx_exp *e, uint8_t out)
{
    if (!(e->port_e7 & 1))
        return nx_sd_xfer(e->sd[0], out);
    if (!(e->port_e7 & 2))
        return nx_sd_xfer(e->sd[1], out);
    return 0xff;
}

static void spi_select(zx_exp *e, uint8_t v)
{
    uint8_t old = e->port_e7;
    e->port_e7 = v;
    if (!(old & 1) && (v & 1))
        nx_sd_deselect(e->sd[0]);
    if (!(old & 2) && (v & 2))
        nx_sd_deselect(e->sd[1]);
}

/* ---- MBIDE ---------------------------------------------------------------- */

/* The IDE register a port's low byte reaches in the m_zxi_002 mode, or -1. */
static int ide_reg(const zx_exp *e, uint8_t lsb)
{
    int mode = e->features & ZX_EXP_ZXI ? e->zxi[0][0x02] & 3 : 3, r = (lsb >> 2) & 7;
    int dron = (lsb & 0xe3) == 0xc3, pvl = (lsb & 0xe3) == 0xa3;
    if (!(e->features & ZX_EXP_IDE))
        return -1;
    switch (mode) {
    case 1: return dron ? r : -1;
    case 2: return pvl ? r : -1;
    case 3: return (dron && !(r & 1)) || (pvl && (r & 1)) ? r : -1;
    default: return -1;
    }
}

static uint8_t ide_read(zx_exp *e, int r)
{
    uint16_t w;
    if (r) {
        e->ide_odd = 0;
        return ata_reg_read(&e->ide, r);
    }
    if (e->ide_odd) {
        e->ide_odd = 0;
        return e->ide_latch;
    }
    w = ata_data_read(&e->ide);
    e->ide_latch = (uint8_t)(w >> 8);
    e->ide_odd = 1;
    return (uint8_t)w;
}

static void ide_write(zx_exp *e, int r, uint8_t v)
{
    if (r) {
        e->ide_odd = 0;
        ata_reg_write(&e->ide, r, v);
    } else if (e->ide_odd) {
        e->ide_odd = 0;
        ata_data_write(&e->ide, (uint16_t)(e->ide_latch | v << 8));
    } else {
        e->ide_latch = v;
        e->ide_odd = 1;
    }
}

/* ---- RTC72421 ------------------------------------------------------------- */

#define RTC_EPOCH 20454          /* 2026-01-01, in days since 1970-01-01 */

static int rtc_on(const zx_exp *e, uint16_t port)
{
    return (e->features & ZX_EXP_RTC) && (port & 0xf0ff) == 0x0003
        && (!(e->features & ZX_EXP_ZXI) || (e->zxi[0][0x03] & 1));
}

static uint8_t rtc_read(const zx *m, int reg)
{
    const zx_exp *e = m->exp;
    int64_t secs = (int64_t)((e->rtc_t + m->t) / m->tm->cpu_hz) + (int64_t)RTC_EPOCH * 86400
                 + (int8_t)e->zxi[0][0x40] * 3600;
    int64_t days = secs / 86400, era, z = days + 719468;
    int s = (int)(secs % 86400), doe, yoe, doy, mp, d, mo, y;
    /* civil date from days (Howard Hinnant's algorithm) */
    era = z / 146097;
    doe = (int)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    mo = mp < 10 ? mp + 3 : mp - 9;
    y = (int)(yoe + era * 400) + (mo <= 2);
    switch (reg) {
    case 0x0: return (uint8_t)(s % 60 % 10);
    case 0x1: return (uint8_t)(s % 60 / 10);
    case 0x2: return (uint8_t)(s / 60 % 60 % 10);
    case 0x3: return (uint8_t)(s / 60 % 60 / 10);
    case 0x4: return (uint8_t)(s / 3600 % 10);
    case 0x5: return (uint8_t)(s / 3600 / 10);
    case 0x6: return (uint8_t)(d % 10);
    case 0x7: return (uint8_t)(d / 10);
    case 0x8: return (uint8_t)(mo % 10);
    case 0x9: return (uint8_t)(mo / 10);
    case 0xa: return (uint8_t)(y % 10);
    case 0xb: return (uint8_t)(y / 10 % 10);
    case 0xc: return (uint8_t)((days + 4) % 7);   /* 0 Sunday; 1970-01-01 was a Thursday */
    case 0xd: return 4;
    case 0xe: return e->rtc_ce;
    default:  return 4;                           /* register F: 24-hour clock */
    }
}

/* 713B-773B: rows 0-7 of the m_zxi registers */
static int zxi_row(uint16_t port)
{
    return (port & 0xf8ff) == 0x703b && (port >> 8) >= 0x71 ? (port >> 8) - 0x71 : -1;
}

static void zxi_write(zx_exp *e, int row, uint8_t v)
{
    e->zxi[row][e->zxi_sel] = v;
    if (row)
        return;
    switch (e->zxi_sel) {
    case 0x0c:
        if (!(v & 1))
            e->port_ff = 0;      /* Timex modes off: the standard screen */
        break;
    case 0x19:
        sid_set_model(&e->sid, v & 1, v & 2);
        break;
    }
}

/* ZX-Uno registers on FC3B (select) and FD3B (data) */
static const char core_id[] = "eLeMeNt MB";

static uint8_t uno_read(zx_exp *e)
{
    switch (e->uno_sel) {
    case 0x41: return (uint8_t)(e->uno_hi ? e->radas_offset >> 8 : e->radas_offset);
    case 0xff:
        if (e->uno_id >= sizeof core_id - 1) {
            e->uno_id = 0;       /* a zero ends the string, then it starts again */
            return 0;
        }
        return (uint8_t)core_id[e->uno_id++];
    default: return e->uno[e->uno_sel];
    }
}

static void uno_write(zx_exp *e, uint8_t v)
{
    if (e->uno_sel == 0x41) {    /* 14 bits, low byte first */
        if (e->uno_hi)
            e->radas_offset = (uint16_t)((e->radas_offset & 0xff) | (v & 0x3f) << 8);
        else
            e->radas_offset = (uint16_t)((e->radas_offset & 0x3f00) | v);
        e->uno_hi ^= 1;
        return;
    }
    e->uno[e->uno_sel] = v;
}

int zx_exp_io_read(zx *m, uint16_t port, uint8_t *v)
{
    zx_exp *e = m->exp;
    uint8_t lsb = (uint8_t)port;
    int r = ide_reg(e, lsb);

    if (r >= 0) {
        *v = ide_read(e, r);
        return 1;
    }
    if (rtc_on(e, port)) {
        *v = rtc_read(m, (port >> 8) & 15);
        return 1;
    }
    if ((e->features & ZX_EXP_DIVMMC) && lsb == 0xeb) {
        *v = e->spi_miso;
        e->spi_miso = spi_xfer(e, 0xff);
        return 1;
    }
    if ((e->features & ZX_EXP_MOUSE) && lsb == 0xdf) {
        int hi = (port >> 8) & 15;
        if (hi == 0x0a || hi == 0x0b || hi == 0x0f) {
            *v = zx_kmouse_port(&e->mouse, hi, 0);
            return 1;
        }
    }
    if (e->features & ZX_EXP_ZXI) {
        int row = zxi_row(port);
        if (port == 0x703b || port == 0x7c3b || row >= 0 || port == 0x7d3b) {
            *v = port == 0x703b ? e->zxi_sel : port == 0x7c3b ? e->gzxi_sel
               : row >= 0 ? e->zxi[row][e->zxi_sel] : e->gzxi[e->gzxi_sel];
            return 1;
        }
    }
    if ((e->features & ZX_EXP_RADASTAN) && (port == 0xfc3b || port == 0xfd3b)) {
        *v = port == 0xfc3b ? e->uno_sel : uno_read(e);
        return 1;
    }
    if (zx_exp_on(e, ZX_EXP_GS, ZXI_GS) && (lsb == 0xb3 || lsb == 0xbb)) {
        *v = gs_read(e->gs, lsb);
        return 1;
    }
    if (zx_exp_on(e, ZX_EXP_SID, ZXI_SID) && port == 0x6d3b) {
        *v = e->sid_sel == 0x1f ? (uint8_t)(e->sid.model | e->sid.ntsc << 1) : sid_read(&e->sid, e->sid_sel);
        return 1;
    }
    if (zx_exp_turbosound(e) && (port & 0xc002) == 0xc000) {
        *v = ay_read(&e->ay[e->ay_select]);
        return 1;
    }
    return 0;
}

void zx_exp_io_write(zx *m, uint16_t port, uint8_t v)
{
    zx_exp *e = m->exp;
    uint8_t lsb = (uint8_t)port;
    int r = ide_reg(e, lsb);

    if (r >= 0) {
        ide_write(e, r, v);
        return;
    }
    if (rtc_on(e, port)) {
        if (((port >> 8) & 15) == 0xe)
            e->rtc_ce = v & 15;
        return;
    }
    if (e->features & ZX_EXP_DIVMMC) {
        switch (lsb) {
        case 0xe3:
            e->port_e3 = (uint8_t)((v & 0xbf) | ((v | e->port_e3) & 0x40));
            update_map(m);
            return;
        case 0xe7:
            spi_select(e, v);
            return;
        case 0xeb:
            e->spi_miso = spi_xfer(e, v);
            return;
        }
    }
    if (e->features & ZX_EXP_ZXI) {
        int row = zxi_row(port);
        if (port == 0x703b) {
            e->zxi_sel = v;
            return;
        } else if (row >= 0) {
            zxi_write(e, row, v);
            return;
        } else if (port == 0x7c3b) {
            e->gzxi_sel = v;
            return;
        } else if (port == 0x7d3b) {
            e->gzxi[e->gzxi_sel] = v;
            return;
        }
    }
    if ((e->features & ZX_EXP_RADASTAN) && (port == 0xfc3b || port == 0xfd3b)) {
        if (port == 0xfc3b) {
            e->uno_sel = v;
            e->uno_hi = 0;
            e->uno_id = 0;
        } else {
            uno_write(e, v);
        }
        return;
    }
    if (zx_exp_on(e, ZX_EXP_GS, ZXI_GS) && (lsb == 0xb3 || lsb == 0xbb)) {
        gs_write(e->gs, lsb, v);
        return;
    }
    if (zx_exp_on(e, ZX_EXP_SID, ZXI_SID) && (port == 0x6c3b || port == 0x6d3b)) {
        if (port == 0x6c3b)
            e->sid_sel = v;
        else if (e->sid_sel == 0x1f)
            sid_set_model(&e->sid, v & 1, v & 2);
        else if (e->sid_sel < 0x1d)
            sid_write(&e->sid, e->sid_sel, v);
        return;
    }
    if (zx_exp_turbosound(e) && (port & 0x0002) == 0) {
        if ((port & 0xc000) == 0xc000) {
            if ((v & 0xfc) == 0xfc && (v & 3))
                e->ay_select = (uint8_t)(3 - (v & 3));   /* FF, FE, FD */
            else
                e->ay[e->ay_select].sel = v;
        } else if ((port & 0xc000) == 0x8000) {
            ay_write(&e->ay[e->ay_select], v);
        }
    }
    if (zx_exp_on(e, ZX_EXP_DAC, ZXI_DAC)) {
        switch (lsb) {
        case 0xdf:               /* SpecDrum */
        case 0xfb:               /* Covox */
            e->dac[0] = e->dac[3] = v;
            break;
        case 0x1f: e->dac[0] = v; break;   /* SounDrive */
        case 0x0f: e->dac[1] = v; break;   /* SounDrive, stereo Covox left */
        case 0x4f: e->dac[2] = v; break;   /* SounDrive, stereo Covox right */
        case 0x5f: e->dac[3] = v; break;   /* SounDrive */
        case 0x3f: e->dac[4] = v; break;   /* Czech DAC B */
        }
    }
    if (zx_exp_on(e, ZX_EXP_SAA, ZXI_SAA)) {
        if (port == 0x6f3b || (lsb == 0xff && (port & 0x100)))
            saa_write_address(&e->saa, v);
        else if (port == 0x6e3b || lsb == 0xff)
            saa_write_data(&e->saa, v);
    }
    if ((e->features & ZX_EXP_TIMEX) && lsb == 0xff && (!(e->features & ZX_EXP_ZXI) || (e->zxi[0][0x0c] & 1)))
        e->port_ff = v;
}

/* ---- audio ---------------------------------------------------------------- */

void zx_exp_audio_run(zx *m, uint32_t dt)
{
    zx_exp *e = m->exp;
    if (zx_exp_turbosound(e)) {
        /* clocked at CPU/2, like the machine's own AY */
        uint32_t c;
        e->ay_half += dt;
        c = e->ay_half >> 1;
        e->ay_half &= 1;
        ay_run(&e->ay[0], c);
        ay_run(&e->ay[1], c);
        ay_run(&e->ay[2], c);
    }
    if (zx_exp_on(e, ZX_EXP_DAC, ZXI_DAC)) {
        int32_t mid = ((int32_t)e->dac[4] - 128) / 2;
        e->dac_l += (int64_t)((int32_t)e->dac[0] + e->dac[1] - 256 + mid) * dt;
        e->dac_r += (int64_t)((int32_t)e->dac[2] + e->dac[3] - 256 + mid) * dt;
    }
    if (zx_exp_on(e, ZX_EXP_SAA, ZXI_SAA)) {
        e->saa_frac += (uint64_t)dt * 8000000u;
        saa_run(&e->saa, (uint32_t)(e->saa_frac / m->tm->cpu_hz));
        e->saa_frac %= m->tm->cpu_hz;
    }
    if (e->gs) {                 /* runs even when not heard, as the card would */
        e->gs_frac += (uint64_t)dt * GS_HZ;
        gs_run(e->gs, e->gs_frac / m->tm->cpu_hz);
        e->gs_frac %= m->tm->cpu_hz;
    }
    if (zx_exp_on(e, ZX_EXP_SID, ZXI_SID)) {
        e->sid_frac += (uint64_t)dt * sid_hz(&e->sid);
        sid_run(&e->sid, (uint32_t)(e->sid_frac / m->tm->cpu_hz));
        e->sid_frac %= m->tm->cpu_hz;
    }
}

void zx_exp_audio_take(zx *m, uint32_t span, int32_t *l, int32_t *r)
{
    zx_exp *e = m->exp;
    if (zx_exp_turbosound(e)) {
        int i;
        for (i = 0; i < 3; i++) {
            int32_t ch[3];
            ay_take(&e->ay[i], ch);
            if (e->acb) {
                *l += (ch[0] + ch[2] / 2) / 5;
                *r += (ch[1] + ch[2] / 2) / 5;
            } else {
                *l += (ch[0] + ch[1] / 2) / 5;
                *r += (ch[2] + ch[1] / 2) / 5;
            }
        }
    }
    if (e->features & ZX_EXP_DAC) {
        if (span && zx_exp_on(e, ZX_EXP_DAC, ZXI_DAC)) {
            *l += (int32_t)(e->dac_l / (int64_t)span) * 24;
            *r += (int32_t)(e->dac_r / (int64_t)span) * 24;
        }
        e->dac_l = e->dac_r = 0;
    }
    if (zx_exp_on(e, ZX_EXP_SAA, ZXI_SAA)) {
        int32_t sl, sr;
        saa_take(&e->saa, &sl, &sr);
        *l += sl * 8;           /* as on the SAM */
        *r += sr * 8;
    }
    if (e->gs) {
        int32_t gl, gr;
        gs_take(e->gs, &gl, &gr);
        if (zx_exp_on(e, ZX_EXP_GS, ZXI_GS)) {
            *l += gl / 2;
            *r += gr / 2;
        }
    }
    if (zx_exp_on(e, ZX_EXP_SID, ZXI_SID)) {
        int32_t s = sid_take(&e->sid);
        *l += s;
        *r += s;
    }
}

/* ---- video ---------------------------------------------------------------- */

static int radastan(const zx *m)
{
    const zx_exp *e = m->exp;
    return (e->features & ZX_EXP_RADASTAN) && ((e->uno[0x40] & 3) == 3 || (m->ulaplus_mode & 3) == 3);
}

static int timex_hires(const zx_exp *e)
{
    return (e->features & ZX_EXP_TIMEX) && (e->port_ff & 6) == 6;
}

static uint8_t giga_mode(const zx_exp *e)
{
    if (!(e->features & ZX_EXP_GIGA))
        return 0;
    return e->gzxi[0x08] ? e->gzxi[0x08] & 3 : e->zxi[0][0x0b] & 3;
}

/* Half way between two RGB333 colours, channel by channel. */
static uint16_t mix333(uint16_t a, uint16_t b)
{
    return (uint16_t)((((a >> 6) + (b >> 6) + 1) >> 1) << 6 | ((((a >> 3) & 7) + ((b >> 3) & 7) + 1) >> 1) << 3
                      | (((a & 7) + (b & 7) + 1) >> 1));
}

static void double_up(const uint16_t *px, uint16_t *wide)
{
    int i;
    if (wide)
        for (i = 0; i < 8; i++)
            wide[2 * i] = wide[2 * i + 1] = px[i];
}

int zx_exp_draw(zx *m, int row, int col, uint16_t *px, uint16_t *wide)
{
    const zx_exp *e = m->exp;
    const uint8_t *scr = m->ram[m->screen_page];
    int paper = row >= 0 && row < 192 && col >= 0 && col < 32, i;

    if (radastan(m)) {
        /* 128x96, a nibble per pixel (the left one high), each shown 2x2,
         * colours from a quarter of the ULA+ palette */
        const uint8_t *pal = &m->ulaplus_pal[(e->uno[0x43] & 3) * 16];
        if (paper) {
            uint32_t base = ((e->port_ff & 1) ? 0x2000u : 0) + e->radas_offset
                            + (uint32_t)(row >> 1) * (64u + e->uno[0x42]) + (uint32_t)col * 2;
            for (i = 0; i < 4; i++) {
                uint8_t b = scr[(base + (uint32_t)(i >> 1)) & 0x3fff];
                px[2 * i] = px[2 * i + 1] = ulaplus_colour(pal[(i & 1) ? b & 15 : b >> 4]);
            }
        } else {
            uint16_t b = ulaplus_colour(pal[((e->uno[0x43] & 4) << 1) | m->border]);
            for (i = 0; i < 8; i++)
                px[i] = b;
        }
        double_up(px, wide);
        return 1;
    }
    if (timex_hires(e)) {
        /* 512x192: columns alternate between the two screens; one colour
         * and its complement, bright; the border in the paper colour */
        int c = (e->port_ff >> 3) & 7;
        uint16_t ink = zx_colours[8 | c], pap = zx_colours[8 | (7 - c)];
        if (paper) {
            uint32_t po = ((row & 0xc0) << 5) | ((row & 7) << 8) | ((row & 0x38) << 2) | col;
            uint16_t bits = (uint16_t)(scr[po] << 8 | scr[po + 0x2000]);
            for (i = 0; i < 8; i++)
                px[i] = (bits & (0xc000 >> (2 * i))) ? ink : pap;
            if (wide)
                for (i = 0; i < 16; i++)
                    wide[i] = (bits & (0x8000 >> i)) ? ink : pap;
        } else {
            for (i = 0; i < 8; i++)
                px[i] = pap;
            double_up(px, wide);
        }
        return 1;
    }
    if (paper && giga_mode(e) == 1 && m->model != ZX_48K) {
        /* GigaScreen mode 0: the 128K screens in banks 5 and 7 mixed */
        uint32_t po = ((row & 0xc0) << 5) | ((row & 7) << 8) | ((row & 0x38) << 2) | col;
        uint32_t ao = 6144 + (row >> 3) * 32 + col;
        uint16_t other[8];
        zx_exp_screen(e, &po, &ao);
        zx_ula_chunk(m, m->ram[5][po], m->ram[5][ao], px);
        zx_ula_chunk(m, m->ram[7][po], m->ram[7][ao], other);
        for (i = 0; i < 8; i++)
            px[i] = mix333(px[i], other[i]);
        double_up(px, wide);
        return 1;
    }
    return 0;
}

/* GigaScreen modes 1 and 2 at the end of a frame: show it mixed with the
 * last one. Mode 2 does so only while the program shows a different screen
 * (bank 5 or 7, Timex screen 0 or 1) every frame. */
static void giga_frame(zx *m)
{
    zx_exp *e = m->exp;
    uint8_t mode = giga_mode(e), buf = (uint8_t)(m->screen_page << 1 | (e->port_ff & 1));
    size_t i, n = (size_t)m->pitch * ZX_SCREEN_H;
    uint16_t *pic = e->own_wide ? m->wide : &m->screen[0][0];
    int mix;
    if (m->wide && !e->own_wide)
        return;                  /* the ATM draws its own picture */
    e->giga_flips = buf != e->giga_buf ? (uint8_t)(e->giga_flips < 2 ? e->giga_flips + 1 : 2) : 0;
    e->giga_buf = buf;
    mix = mode == 2 || (mode == 3 && e->giga_flips >= 2);
    for (i = 0; i < n; i++) {
        uint16_t now = pic[i];
        if (mix)
            pic[i] = mix333(now, e->giga_prev[i]);
        e->giga_prev[i] = now;
    }
}

/* ---- state ---------------------------------------------------------------- */

/* The struct as it is in memory, each SD card's SPI state, then the last
 * frame GigaScreen mixes with. */
static size_t giga_size(const zx *m)
{
    return m->exp->giga_prev ? (size_t)m->pitch * ZX_SCREEN_H * sizeof *m->exp->giga_prev : 0;
}

size_t zx_exp_state_size(const zx *m)
{
    return m->exp ? sizeof(zx_exp) + 2 * nx_sd_state_size() + giga_size(m)
                    + (m->exp->gs ? sizeof(gs) : 0) : 0;
}

void zx_exp_state_save(const zx *m, uint8_t *out)
{
    const zx_exp *e = m->exp;
    int i;
    memcpy(out, e, sizeof *e);
    out += sizeof *e;
    for (i = 0; i < 2; i++, out += nx_sd_state_size()) {
        if (e->sd[i])
            nx_sd_state_save(e->sd[i], out);
        else
            memset(out, 0, nx_sd_state_size());
    }
    if (e->giga_prev)
        memcpy(out, e->giga_prev, giga_size(m));
    out += giga_size(m);
    if (e->gs)
        memcpy(out, e->gs, sizeof *e->gs);
}

void zx_exp_state_load(zx *m, const uint8_t *in, const int has_sd[2])
{
    zx_exp *e = m->exp;
    struct nx_sd *sd[2];
    struct zx_blockdev *cf[2];
    uint16_t *giga_prev = e->giga_prev;
    gs *g = e->gs;
    int i;
    sd[0] = e->sd[0];
    sd[1] = e->sd[1];
    cf[0] = e->ide.dev[0].bd;
    cf[1] = e->ide.dev[1].bd;
    memcpy(e, in, sizeof *e);
    e->giga_prev = giga_prev;
    e->gs = g;
    e->ide.dev[0].bd = cf[0];
    e->ide.dev[1].bd = cf[1];
    in += sizeof *e;
    for (i = 0; i < 2; i++, in += nx_sd_state_size()) {
        e->sd[i] = sd[i];
        if (sd[i] && has_sd[i])
            nx_sd_state_load(sd[i], in);
    }
    if (giga_prev)
        memcpy(giga_prev, in, giga_size(m));
    in += giga_size(m);
    if (g)
        memcpy(g, in, sizeof *g);
    update_map(m);
}
