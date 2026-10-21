/*
 * Beta 128 disk check: run a Pentagon, Scorpion or ATM with a disk in
 * drive A, type at given frames, and read the screen back as text.
 *
 * usage: beta romdir machine disk steps...
 *   disk   .trd: opened as an image file, so what TR-DOS writes stays in it;
 *          .scl: given to the machine as bytes; "-" for none
 *   N:text type text at frame N (\n is Enter; ~ is Caps Shift + Symbol
 *          Shift, the 48K's extended mode; | is cursor down)
 *   N?text at frame N the screen must show text (exit 1 if not)
 *   N.     at frame N print the screen
 *   N=AAAA:HHHH...  at frame N poke hex bytes from address AAAA
 *   N@AAAA at frame N jump to AAAA
 *   N#Y,X,C at frame N picture pixel (X, Y) must be RGB333 colour C (hex)
 *   N>file at frame N save the picture as a PPM
 *
 * The screen is read by matching each 8x8 cell of the picture against the
 * 48K ROM's character set (roms/48.rom), so it reads the Spectrum screen in
 * any of these machines' Spectrum modes.
 */
#include "../src/core/zxm.h"
#include "../src/storage/blockdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t font[96][8];
static char text[24][33];

static int read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    long n;
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    *data = malloc(n > 0 ? (size_t)n : 1);
    if (!*data || fread(*data, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *len = (size_t)n;
    return 0;
}

/* The 256x192 paper as text: each cell's two colours, ink the rarer. */
static void read_screen(zxm *m)
{
    const zxm_info *info = zxm_info_of(m);
    int pitch, row, col, x, y, c, step = info->fb_w / 320;
    const uint16_t *fb = zxm_framebuffer(m, &pitch);
    for (row = 0; row < 24; row++) {
        for (col = 0; col < 32; col++) {
            uint8_t cell[8];
            uint16_t paper = fb[(size_t)(32 + row * 8) * (size_t)pitch + (size_t)(32 + col * 8) * (size_t)step];
            int ch = '?';
            for (y = 0; y < 8; y++) {
                const uint16_t *line = fb + (size_t)(32 + row * 8 + y) * (size_t)pitch;
                cell[y] = 0;
                for (x = 0; x < 8; x++)
                    if (line[(32 + col * 8 + x) * step] != paper)
                        cell[y] |= (uint8_t)(0x80 >> x);
            }
            for (c = 0; c < 96; c++) {
                int inv = 1, same = 1;
                for (y = 0; y < 8; y++) {
                    same &= cell[y] == font[c][y];
                    inv &= (uint8_t)~cell[y] == font[c][y];
                }
                if (same || inv) {
                    ch = c + 32;
                    break;
                }
            }
            text[row][col] = (char)(ch == 127 ? '(' : ch);
        }
        text[row][32] = 0;
        for (col = 31; col >= 0 && text[row][col] == ' '; col--)
            text[row][col] = 0;
    }
}

static int screen_has(const char *s)
{
    int row;
    for (row = 0; row < 24; row++)
        if (strstr(text[row], s))
            return 1;
    return 0;
}

static void print_screen(void)
{
    int row;
    for (row = 0; row < 24; row++)
        if (text[row][0])
            printf("  |%s\n", text[row]);
}

/* Keys: one every 10 frames, held for 3. */
static void type(zxm *m, const char *s, int frame0, int frame)
{
    static const char *rows[8] = { "\001zxcv", "asdfg", "qwert", "12345", "09876", "poiuy", "\nlkjh", " \002mnb" };
    static const char *sym_chars = "!@#$%&'()_<>;:\"?,./*-+=^";
    static const char *sym_keys  = "1234567890rtozpcnmvbjklh";
    int i = (frame - frame0) / 10, phase = (frame - frame0) % 10, r, b, caps = 0, sym = 0;
    zxm_input_event ev;
    char ch;
    memset(&ev, 0, sizeof ev);
    if (i < 0 || i >= (int)strlen(s))
        return;
    ev.kind = ZXM_EV_RELEASE_ALL;
    if (phase == 3) {
        zxm_input(m, &ev);
        return;
    }
    if (phase)
        return;
    ch = s[i];
    if (ch >= 'A' && ch <= 'Z') {
        caps = 1;
        ch = (char)(ch - 'A' + 'a');
    } else if (ch == '~') {
        caps = sym = 1;
        ch = 0;
    } else if (ch == '|') {
        caps = 1;
        ch = '6';
    } else if (strchr(sym_chars, ch)) {
        sym = 1;
        ch = sym_keys[strchr(sym_chars, ch) - sym_chars];
    }
    ev.kind = ZXM_EV_KEY;
    ev.down = 1;
    if (caps) { ev.a = 0; ev.b = 0; zxm_input(m, &ev); }
    if (sym) { ev.a = 7; ev.b = 1; zxm_input(m, &ev); }
    if (!ch)
        return;
    for (r = 0; r < 8; r++)
        for (b = 0; b < 5; b++)
            if (rows[r][b] == ch) {
                ev.a = (uint8_t)r;
                ev.b = (uint8_t)b;
                zxm_input(m, &ev);
            }
}

int main(int argc, char **argv)
{
    const zxm_info *info;
    zxm_rom_set roms;
    zxm_media media;
    zxm *m;
    uint8_t *rom48;
    size_t len;
    int i, frame, last = 0, fail = 0, nsteps = argc - 4;
    struct { int frame; char kind; const char *text; } *steps;

    if (argc < 4) {
        fprintf(stderr, "usage: %s romdir machine disk steps...\n", argv[0]);
        return 2;
    }
    if (!(info = zxm_lookup(argv[2]))) {
        fprintf(stderr, "unknown machine %s\n", argv[2]);
        return 2;
    }
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/48.rom", argv[1]);
        if (read_file(path, &rom48, &len) || len != 0x4000) {
            fprintf(stderr, "cannot read %s\n", path);
            return 2;
        }
        memcpy(font, rom48 + 0x3d00, sizeof font);
    }
    memset(&roms, 0, sizeof roms);
    for (i = 0; i < 4 && info->roms[i]; i++) {
        char path[1024];
        uint8_t *d;
        snprintf(path, sizeof path, "%s/%s", argv[1], info->roms[i]);
        if (read_file(path, &d, &len)) {
            /* An optional ROM is the disk interface's: skip too. */
            fprintf(stderr, "skip: no %s\n", path);
            return 77;
        }
        roms.rom[i] = d;
        roms.len[i] = (uint32_t)len;
    }
    m = malloc(zxm_size(info));
    if (!m || zxm_create(m, info, &roms)) {
        fprintf(stderr, "cannot create %s\n", info->id);
        return 1;
    }
    memset(&media, 0, sizeof media);
    media.name = argv[3];
    len = strlen(argv[3]);
    if (len > 4 && !strcmp(argv[3] + len - 4, ".trd")) {
        media.kind = ZXM_MEDIA_BLOCKS;
        if (!(media.blocks = zx_bd_open_image(argv[3])) || zxm_media_insert(m, "beta.a", &media)) {
            fprintf(stderr, "cannot insert %s\n", argv[3]);
            return 1;
        }
    } else if (strcmp(argv[3], "-")) {
        uint8_t *d;
        if (read_file(argv[3], &d, &len)) {
            fprintf(stderr, "cannot read %s\n", argv[3]);
            return 1;
        }
        media.kind = ZXM_MEDIA_BYTES;
        media.data = d;
        media.len = (uint32_t)len;
        if (zxm_media_insert(m, "beta.a", &media)) {
            fprintf(stderr, "cannot insert %s\n", argv[3]);
            return 1;
        }
        free(d);
    }

    steps = calloc((size_t)nsteps + 1, sizeof *steps);
    for (i = 0; i < nsteps; i++) {
        char *end;
        steps[i].frame = (int)strtol(argv[4 + i], &end, 10);
        steps[i].kind = *end;
        steps[i].text = *end ? end + 1 : "";
        if (steps[i].frame > last)
            last = steps[i].frame;
    }
    for (frame = 0; frame <= last; frame++) {
        char *end;
        for (i = 0; i < nsteps; i++)
            if (steps[i].kind == ':')
                type(m, steps[i].text, steps[i].frame, frame);
        zxm_run_frame(m);
        for (i = 0; i < nsteps; i++) {
            if (steps[i].frame != frame || steps[i].kind == ':')
                continue;
            if (steps[i].kind == '=' || steps[i].kind == '@') {
                const zxm_debug *dbg = zxm_debugger(m);
                unsigned long a = strtoul(steps[i].text, &end, 16);
                if (steps[i].kind == '@') {
                    zxm_regs r;
                    dbg->get_regs(m, &r);
                    r.pc = (uint16_t)a;
                    r.halted = 0;
                    dbg->set_regs(m, &r);
                } else {
                    unsigned v;
                    const char *h = end + 1;
                    while (sscanf(h, "%2x", &v) == 1) {
                        dbg->poke(m, (uint16_t)a++, (uint8_t)v);
                        h += 2;
                    }
                }
                continue;
            }
            if (steps[i].kind == '#') {
                int y = 0, x = 0, pitch;
                unsigned want = 0;
                const uint16_t *fb = zxm_framebuffer(m, &pitch);
                sscanf(steps[i].text, "%d,%d,%x", &y, &x, &want);
                if (fb[(size_t)y * (size_t)pitch + (size_t)x] != want) {
                    printf("%s frame %d: pixel (%d, %d) is %03x, not %03x\n", info->id, frame, x, y,
                           fb[(size_t)y * (size_t)pitch + (size_t)x], want);
                    fail = 1;
                }
                continue;
            }
            if (steps[i].kind == '>') {
                int pitch, x, y;
                const uint16_t *fb = zxm_framebuffer(m, &pitch);
                FILE *fp = fopen(steps[i].text, "wb");
                if (!fp)
                    continue;
                fprintf(fp, "P6\n%d %d\n255\n", info->fb_w, info->fb_h);
                for (y = 0; y < info->fb_h; y++)
                    for (x = 0; x < info->fb_w; x++) {
                        uint16_t c = fb[(size_t)y * (size_t)pitch + (size_t)x];
                        fputc(((c >> 6) & 7) * 255 / 7, fp);
                        fputc(((c >> 3) & 7) * 255 / 7, fp);
                        fputc((c & 7) * 255 / 7, fp);
                    }
                fclose(fp);
                continue;
            }
            read_screen(m);
            if (steps[i].kind == '?' && !screen_has(steps[i].text)) {
                printf("%s frame %d: no \"%s\" on the screen:\n", info->id, frame, steps[i].text);
                print_screen();
                fail = 1;
            } else if (steps[i].kind == '.') {
                printf("%s frame %d:\n", info->id, frame);
                print_screen();
            }
        }
    }
    zxm_destroy(m);
    free(m);
    if (!fail)
        printf("%s: ok\n", info->id);
    return fail;
}
