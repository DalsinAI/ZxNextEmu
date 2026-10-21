/*
 * Expansion-port interfaces (src/zx/expansion.c): the divMMC's paging and
 * automapping, TurboSound, the DACs, the Kempston mouse and the Timex
 * screens and hi-res, Radastan, GigaScreen, the SAA1099, the SID and the
 * ZXi registers, driven through the machine's own bus.
 *
 * usage: mb03 romdir
 */
#include "../src/zx/zx.h"
#include "../src/zx/expansion.h"
#include "../src/storage/blockdev.h"

#include <stdlib.h>

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
        fails++;
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

/* An empty card: reads give zeros, writes vanish. */
static int bd_read(zx_blockdev *bd, uint64_t lba, uint8_t *buf) { (void)bd; (void)lba; memset(buf, 0, 512); return 0; }
static int bd_write(zx_blockdev *bd, uint64_t lba, const uint8_t *buf) { (void)bd; (void)lba; (void)buf; return 0; }
static void bd_close(zx_blockdev *bd) { free(bd); }

static zx_blockdev *empty_card(void)
{
    zx_blockdev *bd = calloc(1, sizeof *bd);
    bd->blocks = 65536;
    bd->read = bd_read;
    bd->write = bd_write;
    bd->close = bd_close;
    return bd;
}

/* CMD8 to card 0; the R1 byte (0x01 while the card is idle). */
static uint8_t cmd8(zx *m)
{
    static const uint8_t c[6] = { 0x48, 0x00, 0x00, 0x01, 0xaa, 0x87 };
    int i;
    uint8_t r = 0xff;
    out(m, 0x00e7, 0xfe);
    for (i = 0; i < 6; i++)
        out(m, 0x00eb, c[i]);
    for (i = 0; i < 8 && r == 0xff; i++) {
        out(m, 0x00eb, 0xff);     /* the byte it brings in is spi_miso */
        r = m->exp->spi_miso;
    }
    out(m, 0x00e7, 0xff);
    return r;
}

static void test_divmmc_more(const char *romdir)
{
    static zx m, d;
    static uint8_t rom[0x2000], state[2 << 20];
    if (machine(&m, ZX_128K, romdir, ZX_EXP_MB03, rom))
        return;
    /* 3Dxx only pages in under the 48K BASIC ROM */
    zx_exp_fetch(&m, 0x3d00);
    CHECK(!m.div_map, "3Dxx under the 128K editor ROM leaves the divMMC out");
    out(&m, 0x7ffd, 0x10);
    zx_exp_fetch(&m, 0x3d00);
    CHECK(m.div_map, "3Dxx under the 48K ROM pages in");
    zx_exp_fetch(&m, 0x1ff8);

    /* a card put in after the state was saved keeps its own state */
    CHECK(zx_state_size(&m) <= sizeof state, "state fits");
    zx_state_save(&m, state, sizeof state);
    zx_exp_attach_sd(&m, 0, empty_card());
    CHECK(cmd8(&m) == 0x01, "a new card is idle");
    CHECK(zx_state_load(&m, state, sizeof state) == 0, "state loads");
    CHECK(cmd8(&m) == 0x01, "the card is still idle after a state without it");

    /* a state from one interface does not load into another */
    if (machine(&d, ZX_128K, romdir, ZX_EXP_MB03 & ~ZX_EXP_MOUSE, rom))
        return;
    CHECK(zx_state_size(&d) == zx_state_size(&m), "same state size");
    CHECK(zx_state_load(&d, state, sizeof state) != 0, "an MB03+ state is refused by an interface without the mouse");
    zx_free(&m);
    zx_free(&d);
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
    square(&m, 0x3f);
    CHECK(swing(&m, 0) > 1500 && swing(&m, 1) > 1500, "Czech DAC B plays on both sides");
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

static void saa(zx *m, uint8_t reg, uint8_t v)
{
    out(m, 0x01ff, reg);
    out(m, 0x00ff, v);
}

static void zxi(zx *m, uint8_t reg, uint8_t v)
{
    out(m, 0x703b, reg);
    out(m, 0x713b, v);
}

static void test_saa(const char *romdir)
{
    static zx m;
    if (machine(&m, ZX_48K, romdir, ZX_EXP_SAA, NULL))
        return;
    park(&m);
    saa(&m, 0x1c, 0x02);                 /* reset generators */
    saa(&m, 0x1c, 0x01);                 /* sound on */
    saa(&m, 0x00, 0xff);                 /* channel 0: left full, right off */
    saa(&m, 0x08, 0x80);                 /* tone frequency */
    saa(&m, 0x10, 0x03);                 /* octave 3 */
    saa(&m, 0x14, 0x01);                 /* tone 0 on */
    zx_run_frame(&m);
    zx_run_frame(&m);
    CHECK(swing(&m, 0) > 2000, "the SAA1099 plays a tone");
    zx_free(&m);

    /* on the MB03+ the SAA starts off and the ZXi register switches it on */
    if (machine(&m, ZX_48K, romdir, ZX_EXP_SAA | ZX_EXP_ZXI, NULL))
        return;
    park(&m);
    out(&m, 0x703b, 1);
    CHECK(in(&m, 0x713b) == ZXI_AUDIO_DEFAULT, "ZXi register 1 reads back the sound devices");
    saa(&m, 0x1c, 0x01); saa(&m, 0x00, 0xff); saa(&m, 0x08, 0x80); saa(&m, 0x10, 0x03); saa(&m, 0x14, 0x01);
    zx_run_frame(&m);
    zx_run_frame(&m);
    CHECK(swing(&m, 0) < 100, "the SAA1099 is off by default");
    zxi(&m, 1, ZXI_AUDIO_DEFAULT | ZXI_SAA);
    /* this time through the ZXi ports: 6F3B register, 6E3B data */
    out(&m, 0x6f3b, 0x1c); out(&m, 0x6e3b, 0x01);
    out(&m, 0x6f3b, 0x00); out(&m, 0x6e3b, 0xff);
    out(&m, 0x6f3b, 0x08); out(&m, 0x6e3b, 0x80);
    out(&m, 0x6f3b, 0x10); out(&m, 0x6e3b, 0x03);
    out(&m, 0x6f3b, 0x14); out(&m, 0x6e3b, 0x01);
    zx_run_frame(&m);
    zx_run_frame(&m);
    CHECK(swing(&m, 0) > 2000, "OUT 28731,1: OUT 28987,n turns the SAA1099 on");
    zx_free(&m);
}

static void sidw(zx *m, uint8_t reg, uint8_t v)
{
    out(m, 0x6c3b, reg);
    out(m, 0x6d3b, v);
}

static void test_sid(const char *romdir)
{
    static zx m;
    int loud, i;
    if (machine(&m, ZX_128K, romdir, ZX_EXP_SID, NULL))
        return;
    park(&m);
    sidw(&m, 0x18, 0x0f);                /* volume 15, no filter */
    sidw(&m, 0x00, 0x00);
    sidw(&m, 0x01, 0x1d);                /* about 440 Hz */
    sidw(&m, 0x05, 0x00);                /* attack 2 ms, decay 6 ms */
    sidw(&m, 0x06, 0xf0);                /* sustain 15, release 6 ms */
    sidw(&m, 0x04, 0x21);                /* sawtooth, gate on */
    zx_run_frame(&m);
    zx_run_frame(&m);
    loud = swing(&m, 0);
    CHECK(loud > 3000, "the SID plays a sawtooth");
    CHECK(swing(&m, 1) == loud, "the SID is mono");
    out(&m, 0x6c3b, 0x1c);
    CHECK(in(&m, 0x6d3b) == 0x00, "ENV3 is voice 3's, not voice 1's");
    sidw(&m, 0x12, 0x21);                /* voice 3 gate on; read its envelope */
    sidw(&m, 0x13, 0x00);
    sidw(&m, 0x14, 0xf0);
    zx_run_frame(&m);
    out(&m, 0x6c3b, 0x1c);
    CHECK(in(&m, 0x6d3b) == 0xff, "ENV3 reads the attacked envelope");
    sidw(&m, 0x12, 0x20);
    sidw(&m, 0x04, 0x20);                /* gate off: release */
    for (i = 0; i < 3; i++)
        zx_run_frame(&m);
    CHECK(swing(&m, 0) < 50, "the note dies away after release");
    out(&m, 0x6c3b, 0x1c);
    CHECK(in(&m, 0x6d3b) == 0x00, "ENV3 reads zero after release");

    /* low-pass at the bottom of its range takes most of a 440 Hz saw away */
    sidw(&m, 0x04, 0x21);
    sidw(&m, 0x15, 0x00);
    sidw(&m, 0x16, 0x00);
    sidw(&m, 0x17, 0x01);                /* voice 1 through the filter */
    sidw(&m, 0x18, 0x1f);                /* low-pass */
    for (i = 0; i < 3; i++)
        zx_run_frame(&m);
    CHECK(swing(&m, 0) < loud / 3, "the low-pass filter quietens the saw");
    zx_free(&m);
}

static void test_zxi(const char *romdir)
{
    static zx m;
    int ch;
    /* With the AY switched off in register 1, a 128K's own AY answers. */
    if (machine(&m, ZX_128K, romdir, ZX_EXP_TURBOSOUND | ZX_EXP_ZXI | ZX_EXP_TIMEX, NULL))
        return;
    park(&m);
    zxi(&m, 1, ZXI_AUDIO_DEFAULT & ~ZXI_AY);
    out(&m, 0xfffd, 0); out(&m, 0xbffd, 0x55);
    CHECK(m.ay.reg[0] == 0x55, "the machine's AY takes the writes");
    out(&m, 0xfffd, 0); out(&m, 0xbffd, 0xfc);
    out(&m, 0xfffd, 1); out(&m, 0xbffd, 0x00);
    out(&m, 0xfffd, 7); out(&m, 0xbffd, 0x3e);
    out(&m, 0xfffd, 8); out(&m, 0xbffd, 0x0f);
    zx_run_frame(&m);
    zx_run_frame(&m);
    ch = swing(&m, 0);
    CHECK(ch > 2000, "and plays");
    zxi(&m, 1, ZXI_AUDIO_DEFAULT);
    zx_run_frame(&m);
    zx_run_frame(&m);
    CHECK(swing(&m, 0) < 100, "with TurboSound back on, the machine's AY is not heard");

    /* register 12 bit 0 switches the Timex port off */
    out(&m, 0x00ff, 1);
    CHECK(m.exp->port_ff == 1, "Timex port on by default");
    zxi(&m, 12, 0);
    CHECK(m.exp->port_ff == 0, "Timex off resets the screen mode");
    out(&m, 0x00ff, 1);
    CHECK(m.exp->port_ff == 0, "and ignores port FF");
    out(&m, 0x703b, 12);
    CHECK(in(&m, 0x713b) == 0, "register 12 reads back");
    /* m_zxi_101 is column 1 on the second row (723B) */
    out(&m, 0x703b, 1);
    out(&m, 0x723b, 0x02);
    CHECK(in(&m, 0x723b) == 0x02 && in(&m, 0x713b) == ZXI_AUDIO_DEFAULT, "rows are separate");
    out(&m, 0x7c3b, 8);
    out(&m, 0x7d3b, 3);
    CHECK(in(&m, 0x7d3b) == 3 && in(&m, 0x7c3b) == 8, "g_zxi registers read back");
    zx_free(&m);
}

static void test_mutes(const char *romdir)
{
    static zx m;
    if (machine(&m, ZX_PENTAGON, romdir, ZX_EXP_DAC | ZX_EXP_ZXI, NULL))
        return;
    out(&m, 0x5f, 0xff);                           /* a DAC level, then the DACs off */
    zxi(&m, 1, ZXI_AUDIO_DEFAULT & ~ZXI_DAC);
    park(&m);
    zx_run_frame(&m);
    zx_run_frame(&m);
    CHECK(m.audio[2 * (m.audio_samples - 1) + 1] == 0, "the DACs off are silent");
    zxi(&m, 1, ZXI_AUDIO_DEFAULT & ~ZXI_BEEPER);
    out(&m, 0x00fe, 0x10);                         /* EAR high */
    zx_run_frame(&m);
    CHECK(m.audio[2 * (m.audio_samples - 1)] == 0, "the beeper muted");
    zxi(&m, 1, ZXI_AUDIO_DEFAULT);
    zx_run_frame(&m);
    CHECK(m.audio[2 * (m.audio_samples - 1)] > 0, "and back on");
    zx_free(&m);
}

static void test_hires(const char *romdir)
{
    static zx m;
    const uint16_t *row;
    int x;
    if (machine(&m, ZX_48K, romdir, ZX_EXP_TIMEX, NULL))
        return;
    CHECK(m.screen_w == 640 && m.pixels == m.wide, "the Timex modes make the picture 640 wide");
    park(&m);
    memset(m.ram[5], 0, 0x4000);
    memset(m.ram[5] + 0x1800, 0x38, 768);
    m.ram[5][0x0000] = 0xf0;                       /* column 0 from screen 0 */
    m.ram[5][0x2000] = 0x0f;                       /* column 1 from screen 1 */
    out(&m, 0x00ff, 6 | 1 << 3);                   /* hi-res, blue on yellow */
    zx_run_frame(&m);
    row = m.pixels + ZX_BORDER_Y * m.pitch + ZX_BORDER_X * 2;
    for (x = 0; x < 16; x++)
        CHECK(row[x] == ((x < 4 || x >= 12) ? 0x007 : 0x1f8), "hi-res: 16 pixels from two screens");
    CHECK(m.pixels[0] == 0x1f8, "hi-res: the border takes the paper colour");
    out(&m, 0x00ff, 0);
    zx_run_frame(&m);
    CHECK(row[0] == 0x000 && row[1] == 0x000 && row[8] == 0x16d, "standard screen: each pixel twice");
    zx_free(&m);
    CHECK(m.screen_w == 320 && !m.wide, "the picture is 320 wide again without the interface");
}

static void ulaplus(zx *m, uint8_t reg, uint8_t v)
{
    out(m, 0xbf3b, reg);
    out(m, 0xff3b, v);
}

static void uno(zx *m, uint8_t reg, uint8_t v)
{
    out(m, 0xfc3b, reg);
    out(m, 0xfd3b, v);
}

static void test_radastan(const char *romdir)
{
    static zx m;
    static const char id[] = "eLeMeNt MB";
    int i, ok = 1;
    if (machine(&m, ZX_128K, romdir, ZX_EXP_RADASTAN, NULL))
        return;
    park(&m);
    for (i = 0; i < 64; i++)
        ulaplus(&m, (uint8_t)i, (uint8_t)(i * 4));  /* entry i: red i, a colour each */
    memset(m.ram[5], 0, 0x4000);
    m.ram[5][0] = 0x12;
    m.ram[5][64] = 0x34;                           /* line 1 */
    uno(&m, 0x40, 3);
    zx_run_frame(&m);
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X] == ulaplus_colour(4) && m.screen[ZX_BORDER_Y][ZX_BORDER_X + 1] == ulaplus_colour(4),
          "Radastan: the left nibble, two pixels wide");
    CHECK(m.screen[ZX_BORDER_Y + 1][ZX_BORDER_X + 2] == ulaplus_colour(8), "Radastan: the right nibble, two lines high");
    CHECK(m.screen[ZX_BORDER_Y + 2][ZX_BORDER_X] == ulaplus_colour(12), "Radastan: 64 bytes a line");
    out(&m, 0x00fe, 2);
    zx_run_frame(&m);
    CHECK(m.screen[0][0] == ulaplus_colour(2 * 4), "Radastan: the border from the palette");
    uno(&m, 0x43, 4);                              /* BOR3 */
    zx_run_frame(&m);
    CHECK(m.screen[0][0] == ulaplus_colour(10 * 4), "Radastan: BOR3 picks border colours 8-15");
    uno(&m, 0x43, 1);                              /* the second palette quarter */
    zx_run_frame(&m);
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X] == ulaplus_colour(17 * 4), "Radastan: palette quarter");
    out(&m, 0xfc3b, 0x41);
    out(&m, 0xfd3b, 64);
    out(&m, 0xfd3b, 0);                            /* offset 64: one line down */
    zx_run_frame(&m);
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X] == ulaplus_colour(19 * 4), "Radastan: the screen offset scrolls");
    uno(&m, 0x40, 0);
    zx_run_frame(&m);
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X] != ulaplus_colour(19 * 4), "Radastan off");
    ulaplus(&m, 0x40, 3);                          /* or through the ULA+ mode register */
    zx_run_frame(&m);
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X] == ulaplus_colour(19 * 4), "Radastan from ULA+ mode 3");
    out(&m, 0xfc3b, 0xff);
    for (i = 0; i < 10; i++)
        ok &= in(&m, 0xfd3b) == (uint8_t)id[i];
    CHECK(ok && in(&m, 0xfd3b) == 0 && in(&m, 0xfd3b) == 'e', "COREID reads eLeMeNt MB, a zero, then again");
    zx_free(&m);
}

static void test_giga(const char *romdir)
{
    static zx m;
    int i;
    if (machine(&m, ZX_128K, romdir, ZX_EXP_GIGA | ZX_EXP_ZXI, NULL))
        return;
    park(&m);
    memset(m.ram[5], 0, 0x1800);
    memset(m.ram[7], 0, 0x1800);
    memset(m.ram[5] + 0x1800, 0x38, 768);          /* white paper */
    memset(m.ram[7] + 0x1800, 0x00, 768);          /* black */
    zxi(&m, 0x0b, 1);                              /* mode 0: the two screens mixed */
    zx_run_frame(&m);
    CHECK(m.screen[ZX_BORDER_Y][ZX_BORDER_X] == 0x0db, "GigaScreen mode 0 mixes banks 5 and 7");
    zxi(&m, 0x0b, 2);                              /* mode 1: frames mixed */
    zx_run_frame(&m);
    memset(m.ram[5] + 0x1800, 0x00, 768);
    zx_run_frame(&m);
    CHECK(m.pixels[ZX_BORDER_Y * m.pitch + ZX_BORDER_X] == 0x0db, "GigaScreen mode 1 mixes this frame with the last");
    zxi(&m, 0x0b, 3);                              /* mode 2: only while flipping */
    memset(m.ram[5] + 0x1800, 0x38, 768);
    zx_run_frame(&m);
    zx_run_frame(&m);
    CHECK(m.pixels[ZX_BORDER_Y * m.pitch + ZX_BORDER_X] == 0x16d, "GigaScreen mode 2: a still screen is left alone");
    for (i = 0; i < 4; i++) {
        out(&m, 0x7ffd, (uint8_t)(i & 1 ? 0x08 : 0x00));   /* show bank 7, then 5, ... */
        zx_run_frame(&m);
    }
    CHECK(m.pixels[ZX_BORDER_Y * m.pitch + ZX_BORDER_X] == 0x0db, "GigaScreen mode 2 mixes flipped screens");
    zx_free(&m);
}

/* General Sound commands: send a command, wait until the GS has taken it. */
static int gs_wait(zx *m, uint8_t bit, int set)
{
    int i;
    for (i = 0; i < 400; i++) {
        if (!(in(m, 0x00bb) & bit) == !set)
            return 1;
        zx_run_frame(m);
    }
    return 0;
}

static int gs_cmd(zx *m, uint8_t cmd)
{
    out(m, 0x00bb, cmd);
    return gs_wait(m, 1, 0);
}

static void test_gs(const char *romdir)
{
    static zx m;
    static uint8_t rom[GS_ROM];
    char path[1024];
    FILE *fp;
    int32_t l = 0, r = 0;
    int i;
    snprintf(path, sizeof path, "%s/../tests-data/gs/gs105a.rom", romdir);
    if (!(fp = fopen(path, "rb")) || fread(rom, 1, sizeof rom, fp) != sizeof rom) {
        if (fp)
            fclose(fp);
        printf("skipped: General Sound (no tests-data/gs/gs105a.rom)\n");
        return;
    }
    fclose(fp);
    if (machine(&m, ZX_48K, romdir, ZX_EXP_MB03 & ~ZX_EXP_DIVMMC, NULL))
        return;
    CHECK(zx_exp_attach_gs(&m, rom, sizeof rom) == 0, "the General Sound fits");
    park(&m);
    CHECK(gs_cmd(&m, 0x00), "the GS takes a command");
    CHECK(gs_cmd(&m, 0x23), "the GS answers 'number of RAM pages'");
    CHECK(gs_wait(&m, 0x80, 1), "and sends data");
    CHECK(in(&m, 0x00b3) == GS_PAGES - 1, "512K: 15 pages besides the ROM");
    CHECK(!(in(&m, 0x00bb) & 0x80), "reading the data clears the flag");

    /* channel 0 (left) at full volume, its DAC at FF */
    out(&m, 0x00b3, 0); CHECK(gs_cmd(&m, 0x04), "set E");
    out(&m, 0x00b3, 0x3f); CHECK(gs_cmd(&m, 0x05), "volume");
    out(&m, 0x00b3, 0xff); CHECK(gs_cmd(&m, 0x06), "DAC");
    zx_run_frame(&m);
    for (i = 0; i < m.audio_samples; i++) {
        l += m.audio[2 * i];
        r += m.audio[2 * i + 1];
    }
    CHECK(m.audio_samples && l / m.audio_samples > 3000 && r / m.audio_samples < 100,
          "the left DAC moves the left side only");
    zx_free(&m);
}

int main(int argc, char **argv)
{
    const char *romdir = argc > 1 ? argv[1] : "roms";
    test_divmmc(romdir);
    test_divmmc_more(romdir);
    test_turbosound(romdir);
    test_dac(romdir);
    test_mouse(romdir);
    test_timex(romdir);
    test_saa(romdir);
    test_sid(romdir);
    test_zxi(romdir);
    test_mutes(romdir);
    test_hires(romdir);
    test_radastan(romdir);
    test_giga(romdir);
    test_gs(romdir);
    printf("%s\n", fails ? "mb03: FAILED" : "mb03: all passed");
    return fails != 0;
}
