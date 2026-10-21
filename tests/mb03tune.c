/*
 * Writes a 48K .sna that plays the MB03+'s SID and SAA1099 (switched on in
 * the ZXi registers) and sets two General Sound DACs, for the
 * native/WebAssembly check in "make wasmtest": di; OUT each (port, value)
 * of the table; send each GS (data, command) pair, waiting for the GS to
 * take it; then jr $.
 *
 * usage: mb03tune out.sna
 */
#include <stdio.h>
#include <string.h>

static const unsigned short tune[][2] = {
    /* SID: volume 15 + low-pass, resonance 15 on voice 1, cutoff, voice 1
     * pulse at 440 Hz, voice 2 sawtooth */
    { 0x6c3b, 0x18 }, { 0x6d3b, 0x1f }, { 0x6c3b, 0x17 }, { 0x6d3b, 0xf1 },
    { 0x6c3b, 0x16 }, { 0x6d3b, 0x28 }, { 0x6c3b, 0x01 }, { 0x6d3b, 0x1d },
    { 0x6c3b, 0x03 }, { 0x6d3b, 0x08 }, { 0x6c3b, 0x06 }, { 0x6d3b, 0xf0 },
    { 0x6c3b, 0x04 }, { 0x6d3b, 0x41 }, { 0x6c3b, 0x08 }, { 0x6d3b, 0x28 },
    { 0x6c3b, 0x0d }, { 0x6d3b, 0xf0 }, { 0x6c3b, 0x0b }, { 0x6d3b, 0x21 },
    /* m_zxi_001: everything on, the SAA1099 too */
    { 0x703b, 0x01 }, { 0x713b, 0x7f },
    /* SAA1099: sound on, channel 0 left, a tone in octave 3 */
    { 0x6f3b, 0x1c }, { 0x6e3b, 0x01 }, { 0x6f3b, 0x00 }, { 0x6e3b, 0xf0 },
    { 0x6f3b, 0x08 }, { 0x6e3b, 0x80 }, { 0x6f3b, 0x10 }, { 0x6e3b, 0x03 },
    { 0x6f3b, 0x14 }, { 0x6e3b, 0x01 },
};

/* General Sound: channel 0 (left) and 2 (right) at full volume, DACs set */
static const unsigned char gs_cmds[][2] = {
    { 0, 0x04 }, { 0x3f, 0x05 }, { 0xc0, 0x06 }, { 2, 0x04 }, { 0x3f, 0x05 }, { 0x40, 0x06 },
};

int main(int argc, char **argv)
{
    static unsigned char sna[27 + 49152];
    static const unsigned char code[] = {
        0xf3,                   /* di */
        0x21, 0x00, 0x81,       /* ld hl,0x8100 */
        0x06, 0x00,             /* ld b,n */
        0xc5,                   /* loop: push bc */
        0x4e, 0x23, 0x46, 0x23, /* ld c,(hl): inc hl: ld b,(hl): inc hl */
        0x7e, 0x23,             /* ld a,(hl): inc hl */
        0xed, 0x79,             /* out (c),a */
        0xc1,                   /* pop bc */
        0x10, 0xf4,             /* djnz loop */
        0x21, 0x00, 0x82,       /* ld hl,0x8200 */
        0x06, 0x00,             /* ld b,n */
        0x7e, 0xd3, 0xb3, 0x23, /* gs: ld a,(hl): out (0xb3),a: inc hl */
        0x7e, 0xd3, 0xbb, 0x23, /* ld a,(hl): out (0xbb),a: inc hl */
        0xdb, 0xbb, 0x0f,       /* wait: in a,(0xbb): rrca */
        0x38, 0xfb,             /* jr c,wait */
        0x10, 0xf1,             /* djnz gs */
        0x18, 0xfe              /* jr $ */
    };
    unsigned char *ram = sna + 27;
    size_t i, n = sizeof tune / sizeof tune[0];
    FILE *fp;
    if (argc != 2) {
        fprintf(stderr, "usage: %s out.sna\n", argv[0]);
        return 2;
    }
    memcpy(ram + 0x8000 - 0x4000, code, sizeof code);
    ram[0x8000 - 0x4000 + 5] = (unsigned char)n;
    ram[0x8000 - 0x4000 + 22] = (unsigned char)(sizeof gs_cmds / sizeof gs_cmds[0]);
    memcpy(ram + 0x8200 - 0x4000, gs_cmds, sizeof gs_cmds);
    for (i = 0; i < n; i++) {
        ram[0x8100 - 0x4000 + i * 3] = (unsigned char)tune[i][0];
        ram[0x8100 - 0x4000 + i * 3 + 1] = (unsigned char)(tune[i][0] >> 8);
        ram[0x8100 - 0x4000 + i * 3 + 2] = (unsigned char)tune[i][1];
    }
    memset(ram + 0x1800, 0x38, 768);            /* white screen */
    /* registers: SP 0xff00 with PC 0x8000 on the stack, IM 1, border 7 */
    sna[23] = 0x00; sna[24] = 0xff;
    sna[25] = 1;
    sna[26] = 7;
    ram[0xff00 - 0x4000] = 0x00;
    ram[0xff01 - 0x4000] = 0x80;
    if (!(fp = fopen(argv[1], "wb")) || fwrite(sna, 1, sizeof sna, fp) != sizeof sna) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    fclose(fp);
    return 0;
}
