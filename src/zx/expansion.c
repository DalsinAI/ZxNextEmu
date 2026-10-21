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
    free(e);
    m->exp = NULL;
    m->div_map = 0;
}

void zx_exp_power_on(zx *m)
{
    zx_exp *e = m->exp;
    if (!e)
        return;
    memset(e->div_ram, 0, sizeof e->div_ram);
    e->port_e3 = 0;
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
    update_map(m);
}

void zx_exp_frame_end(zx *m)
{
    zx_exp *e = m->exp;
    nx_sd_tick(e->sd[0]);
    nx_sd_tick(e->sd[1]);
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
    if ((addr & 0xff00) == 0x3d00 && !e->automap) {
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

int zx_exp_io_read(zx *m, uint16_t port, uint8_t *v)
{
    zx_exp *e = m->exp;
    uint8_t lsb = (uint8_t)port;

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
    if ((e->features & ZX_EXP_TURBOSOUND) && (port & 0xc002) == 0xc000) {
        *v = ay_read(&e->ay[e->ay_select]);
        return 1;
    }
    return 0;
}

void zx_exp_io_write(zx *m, uint16_t port, uint8_t v)
{
    zx_exp *e = m->exp;
    uint8_t lsb = (uint8_t)port;

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
    if ((e->features & ZX_EXP_TURBOSOUND) && (port & 0x0002) == 0) {
        if ((port & 0xc000) == 0xc000) {
            if ((v & 0xfc) == 0xfc && (v & 3))
                e->ay_select = (uint8_t)(3 - (v & 3));   /* FF, FE, FD */
            else
                e->ay[e->ay_select].sel = v;
        } else if ((port & 0xc000) == 0x8000) {
            ay_write(&e->ay[e->ay_select], v);
        }
    }
    if (e->features & ZX_EXP_DAC) {
        switch (lsb) {
        case 0xdf:               /* SpecDrum */
        case 0xfb:               /* Covox */
            e->dac[0] = e->dac[3] = v;
            break;
        case 0x1f: e->dac[0] = v; break;   /* SounDrive */
        case 0x0f: e->dac[1] = v; break;   /* SounDrive, stereo Covox left */
        case 0x4f: e->dac[2] = v; break;   /* SounDrive, stereo Covox right */
        case 0x5f: e->dac[3] = v; break;   /* SounDrive */
        }
    }
    if ((e->features & ZX_EXP_TIMEX) && lsb == 0xff)
        e->port_ff = v;
}

/* ---- audio ---------------------------------------------------------------- */

void zx_exp_audio_run(zx *m, uint32_t dt)
{
    zx_exp *e = m->exp;
    if (e->features & ZX_EXP_TURBOSOUND) {
        /* clocked at CPU/2, like the machine's own AY */
        uint32_t c;
        e->ay_half += dt;
        c = e->ay_half >> 1;
        e->ay_half &= 1;
        ay_run(&e->ay[0], c);
        ay_run(&e->ay[1], c);
        ay_run(&e->ay[2], c);
    }
    if (e->features & ZX_EXP_DAC) {
        e->dac_l += (int64_t)((int32_t)e->dac[0] + e->dac[1] - 256) * dt;
        e->dac_r += (int64_t)((int32_t)e->dac[2] + e->dac[3] - 256) * dt;
    }
}

void zx_exp_audio_take(zx *m, uint32_t span, int32_t *l, int32_t *r)
{
    zx_exp *e = m->exp;
    if (e->features & ZX_EXP_TURBOSOUND) {
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
        if (span) {
            *l += (int32_t)(e->dac_l / (int64_t)span) * 24;
            *r += (int32_t)(e->dac_r / (int64_t)span) * 24;
        }
        e->dac_l = e->dac_r = 0;
    }
}

/* ---- state ---------------------------------------------------------------- */

/* The struct as it is in memory, then each SD card's SPI state. */
size_t zx_exp_state_size(const zx *m)
{
    return m->exp ? sizeof(zx_exp) + 2 * nx_sd_state_size() : 0;
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
}

void zx_exp_state_load(zx *m, const uint8_t *in)
{
    zx_exp *e = m->exp;
    struct nx_sd *sd[2];
    int i;
    sd[0] = e->sd[0];
    sd[1] = e->sd[1];
    memcpy(e, in, sizeof *e);
    in += sizeof *e;
    for (i = 0; i < 2; i++, in += nx_sd_state_size()) {
        e->sd[i] = sd[i];
        if (sd[i])
            nx_sd_state_load(sd[i], in);
    }
    update_map(m);
}
