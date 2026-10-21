/*
 * Oric checks against tests/oric/testrom.s (no Oric ROM needed): the VIA
 * timer's exact period, the ULA's attributes, text and HIRES modes, the
 * keyboard matrix through the AY's port, the AY behind the VIA, and the
 * tape signal read back by a loader timing CB1 edges with T2.
 *
 *   oric testrom.rom [screen.ppm]
 */
#include "../src/oric/oric.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint16_t red = 0x1c0, yellow = 0x1f8, white = 0x1ff, black = 0;

static void via_timing(void)
{
    via6522 v;
    int i, first = -1, second = -1;
    memset(&v, 0, sizeof v);
    via_reset(&v);
    via_write(&v, 11, 0x40);             /* T1 free-running */
    via_write(&v, 4, 9998 & 0xff);
    via_write(&v, 5, 9998 >> 8);
    via_tick(&v);                        /* the write's own cycle */
    for (i = 1; i < 30000 && second < 0; i++) {
        via_tick(&v);
        if (v.ifr & VIA_IFR_T1) {
            if (first < 0)
                first = i;
            else
                second = i;
            via_read(&v, 4);
        }
    }
    CHECK(first == 9999, "T1 first underflow %d cycles after the write, want 9999 (N + 1)", first);
    CHECK(second - first == 10000, "T1 period %d, want 10000 (N + 2)", second - first);
}

static void run(oric *m, int frames)
{
    while (frames--)
        oric_run_frame(m);
}

static void video(oric *m)
{
    /* Row 0: red paper attribute, yellow ink, '0' at cell 2 (row 0 of
     * the glyph is 011100). Row 1: the same inverted. */
    CHECK(m->screen[0][0] == red, "paper attribute cell %03x", m->screen[0][0]);
    CHECK(m->screen[0][12] == red && m->screen[0][13] == yellow, "text cell %03x %03x",
          m->screen[0][12], m->screen[0][13]);
    /* Row 1 has no attributes: white on black, inverted. */
    CHECK(m->screen[8][12] == white && m->screen[8][13] == black, "inverse cell %03x %03x",
          m->screen[8][12], m->screen[8][13]);
    /* Row 2 is double height blocks; row 3 blinks with frame bit 4. */
    CHECK(m->screen[17][12] == white, "double height %03x", m->screen[17][12]);
    run(m, 16 - (int)(m->frames & 15));
    {
        uint16_t a = m->screen[25][12];
        run(m, 16);
        CHECK(a != m->screen[25][12], "blink did not change (%03x)", a);
    }
    /* A HIRES attribute switches the picture to the bitmap at A000. */
    m->ram[0xbb80] = 0x1e;
    memset(m->ram + 0xa000 + 10 * 40, 0x40, 40);    /* blank bitmap bytes */
    m->ram[0xa000 + 10 * 40 + 5] = 0x7f;
    run(m, 2);
    CHECK(m->screen[10][30] == white && m->screen[10][29] == black, "HIRES cell %03x %03x",
          m->screen[10][30], m->screen[10][29]);
    CHECK(m->screen[200][12] == black, "HIRES text rows %03x", m->screen[200][12]);
    /* Back to text. The font bytes the bitmap read included mode
     * attributes too (0x18 is 60 Hz text), so like the ROM's TEXT put a
     * 50 Hz text attribute at the end of the text screen. */
    m->ram[0xa000] = 0x1a;
    m->ram[0xbb80] = 0x11;
    m->ram[0xbfdf] = 0x1a;
    run(m, 3);
    CHECK(m->screen[0][12] == red, "back to text %03x", m->screen[0][12]);
    CHECK(m->frame_t == 312 * ORIC_LINE, "back to 50 Hz (%u cycles a frame)", m->frame_t);
}

static void keyboard(oric *m)
{
    oric_key(m, 3, 5, 1);
    run(m, 3);
    CHECK(m->ram[0x210] == 3 && m->ram[0x211] == 5, "key (3,5) read as (%d,%d)", m->ram[0x210], m->ram[0x211]);
    oric_key(m, 3, 5, 0);
    oric_key(m, 7, 2, 1);
    run(m, 3);
    CHECK(m->ram[0x210] == 7 && m->ram[0x211] == 2, "key (7,2) read as (%d,%d)", m->ram[0x210], m->ram[0x211]);
    oric_key(m, 7, 2, 0);
    run(m, 3);
    CHECK(m->ram[0x210] == 0xff, "no key read as row %d", m->ram[0x210]);
}

static void sound(oric *m)
{
    int i, lo = 32767, hi = -32768;
    CHECK(m->ay.reg[8] == 15 && m->ay.reg[0] == 0 && m->ay.reg[1] == 1 && m->ay.reg[7] == 0x7e,
          "AY registers %02x %02x %02x %02x", m->ay.reg[8], m->ay.reg[0], m->ay.reg[1], m->ay.reg[7]);
    run(m, 1);
    CHECK(m->audio_samples == 882 || m->audio_samples == 883 || m->audio_samples == 881,
          "%d samples in a 50 Hz frame", m->audio_samples);
    for (i = 0; i < m->audio_samples; i++) {
        if (m->audio[i * 2] < lo) lo = m->audio[i * 2];
        if (m->audio[i * 2] > hi) hi = m->audio[i * 2];
    }
    CHECK(hi - lo > 8000, "tone A swing %d", hi - lo);
}

static void tape(oric *m)
{
    uint8_t tap[1024];
    size_t n = 0, start, i;
    int f;
    for (i = 0; i < 16; i++)
        tap[n++] = 0x16;
    tap[n++] = 0x24;
    start = n;
    tap[n++] = 0; tap[n++] = 0; tap[n++] = 0x80; tap[n++] = 0;
    tap[n++] = 0x05; tap[n++] = 0x2b; tap[n++] = 0x05; tap[n++] = 0x00; tap[n++] = 0;
    memcpy(tap + n, "TEST", 5);
    n += 5;
    for (i = 0; i < 300; i++)
        tap[n++] = (uint8_t)(i * 37 + (i >> 3));
    CHECK(oric_tape_insert(m, tap, n) == 0, "tape insert");
    m->ram[0x220] = (uint8_t)(n - start);
    m->ram[0x221] = (uint8_t)((n - start) >> 8);
    m->ram[0x230] = 1;
    for (f = 0; f < 3000 && !m->ram[0x225]; f++)
        run(m, 1);
    CHECK(m->ram[0x225], "tape load did not finish (%d bytes)", m->ram[0x222] | m->ram[0x223] << 8);
    CHECK(!memcmp(m->ram + 0x400, tap + start, n - start), "tape bytes differ");
    CHECK(m->ram[0x224] == 0, "%d parity errors", m->ram[0x224]);
    printf("tape: %u bytes in %d frames\n", (unsigned)(n - start), f);
    oric_tape_eject(m);
}

int main(int argc, char **argv)
{
    static uint8_t rom[0x4000];
    static oric m;
    FILE *fp;
    if (argc < 2 || !(fp = fopen(argv[1], "rb")) || fread(rom, 1, sizeof rom, fp) != sizeof rom) {
        fprintf(stderr, "usage: %s testrom.rom\n", argv[0]);
        return 2;
    }
    fclose(fp);
    via_timing();
    if (oric_create(&m, ORIC_ATMOS, rom, sizeof rom)) {
        printf("FAIL: create\n");
        return 1;
    }
    run(&m, 100);
    CHECK(m.ram[0x200] + m.ram[0x201] * 256 >= 195 && m.ram[0x200] + m.ram[0x201] * 256 <= 200,
          "%d timer interrupts in 100 frames", m.ram[0x200] + m.ram[0x201] * 256);
    video(&m);
    keyboard(&m);
    sound(&m);
    tape(&m);
    if (argc > 2 && (fp = fopen(argv[2], "wb"))) {
        int x, y;
        fprintf(fp, "P6\n%d %d\n255\n", ORIC_W, ORIC_H);
        for (y = 0; y < ORIC_H; y++)
            for (x = 0; x < ORIC_W; x++) {
                uint16_t p = m.screen[y][x];
                fputc((p >> 6 & 7) * 255 / 7, fp);
                fputc((p >> 3 & 7) * 255 / 7, fp);
                fputc((p & 7) * 255 / 7, fp);
            }
        fclose(fp);
    }
    oric_free(&m);
    printf(fails ? "oric: %d failures\n" : "oric: all checks pass\n", fails);
    return fails != 0;
}
