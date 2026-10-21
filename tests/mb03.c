/*
 * Expansion-port interfaces (src/zx/expansion.c): the divMMC's paging and
 * automapping, TurboSound, the DACs, the Kempston mouse and the Timex
 * screens, driven through the machine's own bus.
 *
 * usage: mb03 romdir
 */
#include "../src/zx/zx.h"
#include "../src/zx/expansion.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL: %s\n", what); fails++; } } while (0)

static void out(zx *m, uint16_t port, uint8_t v) { m->bus.io_write(m, port, v); }
static uint8_t in(zx *m, uint16_t port) { return m->bus.io_read(m, port); }

/* Peak-to-peak of one channel over a frame's samples. */
static int swing(const zx *m, int ch)
{
    int i, lo = 32767, hi = -32768;
    for (i = 0; i < m->audio_samples; i++) {
        int v = m->audio[i * 2 + ch];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    return m->audio_samples ? hi - lo : 0;
}

/* Park the CPU on a HALT with interrupts off so frames only run time. */
static void park(zx *m)
{
    m->cpu.iff1 = m->cpu.iff2 = 0;
    zx_poke(m, 0x8000, 0x76);
    m->cpu.pc = 0x8000;
}

static int machine(zx *m, zx_model model, const char *romdir, uint32_t features, const uint8_t *div_rom)
{
    if (zx_init(m, model, romdir) || zx_exp_attach(m, features, div_rom, 0x2000)) {
        printf("FAIL: cannot build the machine\n");
        return -1;
    }
    return 0;
}

static void test_divmmc(const char *romdir)
{
    static zx m;
    static uint8_t rom[0x2000];
    int i;
    for (i = 0; i < 0x2000; i++)
        rom[i] = (uint8_t)(i * 7 + 1);
    rom[0x0000] = 0xf3;
    if (machine(&m, ZX_48K, romdir, ZX_EXP_DIVMMC, rom))
        return;
    /* zx_reset ran with PC 0: nothing is paged until the first fetch */
    CHECK(!m.div_map, "divMMC paged in before any fetch");
    CHECK(zx_peek(&m, 0x0000) == 0xf3, "Spectrum ROM visible at 0000");

    /* CONMEM: the ROM at 0000, the selected bank at 2000, writable */
    out(&m, 0x00e3, 0x80 | 5);
    CHECK(m.div_map, "CONMEM pages the divMMC in");
    CHECK(zx_peek(&m, 0x0123) == rom[0x0123], "esxDOS ROM at 0000 with CONMEM");
    zx_poke(&m, 0x2345, 0xa5);
    CHECK(m.exp->div_ram[5][0x0345] == 0xa5, "write to bank 5 at 2000");
    zx_poke(&m, 0x0100, 0x00);
    CHECK(zx_peek(&m, 0x0100) == rom[0x0100], "the ROM ignores writes");
    out(&m, 0x00e3, 0x80 | 6);
    CHECK(zx_peek(&m, 0x2345) != 0xa5, "bank 6 is another bank");
    out(&m, 0x00e3, 0);
    CHECK(!m.div_map, "CONMEM off pages out");

    /* Automap: entry points page in after the fetch, 1FF8-1FFF page out */
    zx_exp_fetch(&m, 0x0038);
    CHECK(m.div_map, "fetch from 0038 pages in");
    zx_exp_fetch(&m, 0x1000);
    CHECK(m.div_map, "stays in across other fetches");
    CHECK(zx_exp_fetch(&m, 0x1ff8) == rom[0x1ff8], "the off-point fetch reads the divMMC");
    CHECK(!m.div_map, "fetch from 1FF8 pages out");
    CHECK(zx_exp_fetch(&m, 0x3d00) == m.exp->div_ram[0][0x1d00], "3Dxx pages in during its own fetch");
    CHECK(m.div_map, "fetch from 3D00 pages in");
    zx_exp_fetch(&m, 0x1ffc);
    CHECK(zx_exp_fetch(&m, 0x0562) == m.rom[0][0x0562], "a delayed entry point's own fetch is the Spectrum ROM");
    CHECK(m.div_map, "fetch from 0562 pages in");
    zx_exp_fetch(&m, 0x1fff);

    /* MAPRAM: bank 3 replaces the ROM, read-only, and stays set */
    out(&m, 0x00e3, 0x80 | 3);
    zx_poke(&m, 0x2000, 0x5a);
    out(&m, 0x00e3, 0x40 | 3);
    out(&m, 0x00e3, 0x00);
    CHECK(m.exp->port_e3 & 0x40, "MAPRAM sticks");
    zx_exp_fetch(&m, 0x0000);
    CHECK(zx_peek(&m, 0x0000) == 0x5a, "MAPRAM puts bank 3 at 0000");
    zx_poke(&m, 0x0000, 0x11);
    CHECK(zx_peek(&m, 0x0000) == 0x5a, "bank 3 is read-only under MAPRAM");
    zx_reset(&m);
    CHECK(m.exp->port_e3 & 0x40, "MAPRAM survives a reset");
    zx_exp_power_on(&m);
    CHECK(!(m.exp->port_e3 & 0x40), "MAPRAM clears at power-on");

    /* no card: the SPI port reads FF */
    out(&m, 0x00e7, 0xfe);
    out(&m, 0x00eb, 0xff);
    CHECK(in(&m, 0x00eb) == 0xff, "SPI with no card reads FF");
    zx_free(&m);
}

static void test_turbosound(const char *romdir)
{
    static zx m;
    int ch;
    if (machine(&m, ZX_48K, romdir, ZX_EXP_TURBOSOUND, NULL))
        return;
    park(&m);
    /* chip 1 (FE): tone A at about 440 Hz, full volume; A is on the left */
    out(&m, 0xfffd, 0xfe);
    out(&m, 0xfffd, 0); out(&m, 0xbffd, 0xfc);
    out(&m, 0xfffd, 1); out(&m, 0xbffd, 0x00);
    out(&m, 0xfffd, 7); out(&m, 0xbffd, 0x3e);
    out(&m, 0xfffd, 8); out(&m, 0xbffd, 0x0f);
    out(&m, 0xfffd, 0);
    CHECK(in(&m, 0xfffd) == 0xfc, "read back chip 1's register 0");
    out(&m, 0xfffd, 0xff);
    out(&m, 0xfffd, 0);
    CHECK(in(&m, 0xfffd) == 0x00, "chip 0 has its own registers");
    zx_run_frame(&m);
    zx_run_frame(&m);
    ch = swing(&m, 0);
    CHECK(ch > 2000, "a 48K with TurboSound plays an AY tone");
    CHECK(swing(&m, 1) < ch, "ABC stereo: channel A leans left");
    zx_free(&m);
}

/* A square wave on one DAC port: ld a,0; loop: out (port),a; cpl;
 * ld b,50; djnz $; jr loop. */
static void square(zx *m, uint8_t port)
{
    static const uint8_t prog[] = { 0x3e, 0x00, 0xd3, 0x00, 0x2f, 0x06, 0x32, 0x10, 0xfe, 0x18, 0xf7 };
    int i;
    zx_reset(m);
    m->cpu.iff1 = m->cpu.iff2 = 0;
    for (i = 0; i < (int)sizeof prog; i++)
        zx_poke(m, (uint16_t)(0x8000 + i), i == 3 ? port : prog[i]);
    m->cpu.pc = 0x8000;
    zx_run_frame(m);
    zx_run_frame(m);
}

static void test_dac(const char *romdir)
{
    static zx m;
    if (machine(&m, ZX_PENTAGON, romdir, ZX_EXP_DAC, NULL))
        return;
    square(&m, 0xdf);
    CHECK(swing(&m, 0) > 4000 && swing(&m, 1) > 4000, "SpecDrum plays on both sides");
    square(&m, 0x0f);
    CHECK(swing(&m, 0) > 4000 && swing(&m, 1) < 100, "stereo Covox left stays left");
    square(&m, 0x5f);
    CHECK(swing(&m, 1) > 4000 && swing(&m, 0) < 100, "SounDrive D is on the right");
    zx_free(&m);
}

static void test_mouse(const char *romdir)
{
    static zx m;
    uint8_t x0, y0;
    if (machine(&m, ZX_128K, romdir, ZX_EXP_MOUSE, NULL))
        return;
    x0 = in(&m, 0xfbdf);
    y0 = in(&m, 0xffdf);
    zx_mouse(&m, 10, 4, 0, 1);
    CHECK(in(&m, 0xfbdf) == (uint8_t)(x0 + 10), "mouse X counts right");
    CHECK(in(&m, 0xffdf) == (uint8_t)(y0 - 4), "mouse Y counts up");
    CHECK((in(&m, 0xfadf) & 3) == 1, "left button reads 0");
    zx_free(&m);
}

static void test_timex(const char *romdir)
{
    static zx m;
    int x;
    if (machine(&m, ZX_48K, romdir, ZX_EXP_TIMEX, NULL))
        return;
    park(&m);
    memset(m.ram[5], 0, 0x4000);
    memset(m.ram[5] + 0x1800, 0x38, 768);          /* screen 0: black on white */
    m.ram[5][0x2000] = 0xff;                       /* screen 1: first byte set */
    m.ram[5][0x3800] = 0x07;                       /* screen 1 attribute: white on black */
    out(&m, 0x00ff, 1);
    zx_run_frame(&m);
    for (x = 0; x < 8; x++)
        CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X + x] == 0x16d, "second screen: ink white");
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X + 8] == 0x000, "second screen: paper black");

    /* hi-colour: pixel row 1's attribute is its own byte at 0x6000 + 0x100 */
    out(&m, 0x00ff, 2);
    m.ram[5][0x0000] = 0x00;
    m.ram[5][0x0100] = 0x00;
    m.ram[5][0x2000] = 0x10;                       /* row 0: paper red */
    m.ram[5][0x2100] = 0x20;                       /* row 1: paper green */
    zx_run_frame(&m);
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X] == 0x140, "hi-colour row 0 red");
    CHECK(m.screen[ZX_BORDER_Y + 1][ZX_BORDER_X] == 0x028, "hi-colour row 1 green");
    zx_free(&m);
}

int main(int argc, char **argv)
{
    const char *romdir = argc > 1 ? argv[1] : "roms";
    test_divmmc(romdir);
    test_turbosound(romdir);
    test_dac(romdir);
    test_mouse(romdir);
    test_timex(romdir);
    printf("%s\n", fails ? "mb03: FAILED" : "mb03: all passed");
    return fails != 0;
}
