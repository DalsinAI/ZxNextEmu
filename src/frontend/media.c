#include "media.h"
#include "../zx/tape.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *media_read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *len = (size_t)n;
    return buf;
}

static int has_ext(const char *path, const char *ext)
{
    size_t lp = strlen(path), le = strlen(ext), i;
    if (lp < le)
        return 0;
    for (i = 0; i < le; i++)
        if (tolower((unsigned char)path[lp - le + i]) != ext[i])
            return 0;
    return 1;
}

int media_load(zx *m, const char *path)
{
    size_t len;
    uint8_t *d = media_read_file(path, &len);
    int r = -1;
    if (!d) {
        fprintf(stderr, "cannot read %s\n", path);
        return -1;
    }
    if (has_ext(path, ".sna")) {
        r = zx_load_sna(m, d, len);
    } else if (has_ext(path, ".z80")) {
        r = zx_load_z80(m, d, len);
    } else if (has_ext(path, ".tap") || has_ext(path, ".tzx")) {
        zx_tape *t = zx_tape_open(d, len, has_ext(path, ".tzx"));
        if (t) {
            zx_tape_free(m->tape);
            m->tape = t;
            r = 0;
        }
    } else {
        fprintf(stderr, "%s: unknown file type\n", path);
    }
    if (r)
        fprintf(stderr, "%s: could not load\n", path);
    free(d);
    return r;
}

static const char matrix[8][6] = {
    { 0, 'z', 'x', 'c', 'v' },
    { 'a', 's', 'd', 'f', 'g' },
    { 'q', 'w', 'e', 'r', 't' },
    { '1', '2', '3', '4', '5' },
    { '0', '9', '8', '7', '6' },
    { 'p', 'o', 'i', 'u', 'y' },
    { '\n', 'l', 'k', 'j', 'h' },
    { ' ', 0, 'm', 'n', 'b' },
};

/* Symbol-shifted characters and the key they live on. */
static const char *const sym_chars = "!@#$%&'()_<>;:\"?,./*-+=^";
static const char *const sym_keys  = "1234567890rtozpcnmvbjklh";

int media_char_key(int ch, int *row, int *bit, int *caps, int *sym)
{
    int r, b;
    const char *s;
    *caps = *sym = 0;
    if (ch == '\r')
        ch = '\n';
    if (isupper(ch)) {
        *caps = 1;
        ch = tolower(ch);
    } else if (ch && (s = strchr(sym_chars, ch)) != NULL) {
        *sym = 1;
        ch = sym_keys[s - sym_chars];
    }
    for (r = 0; r < 8; r++)
        for (b = 0; b < 5; b++)
            if (matrix[r][b] == ch && ch) {
                *row = r;
                *bit = b;
                return 1;
            }
    return 0;
}

static char type_buf[1024];
static size_t type_pos, type_len;
static int type_phase, type_wait;

void media_type_delay(int frames)
{
    type_wait = frames;
}

void media_type(const char *text)
{
    size_t n = strlen(text);
    if (n >= sizeof type_buf)
        n = sizeof type_buf - 1;
    memcpy(type_buf, text, n);
    type_len = n;
    type_pos = 0;
    type_phase = 0;
}

/* Each key is held for 3 frames and released for 7: the ROM keeps a
 * released key in KSTATE for 5 interrupts, so the same key twice in a
 * row needs the longer gap. */
void media_type_frame(zx *m)
{
    int row, bit, caps, sym, i;
    if (type_pos >= type_len)
        return;
    if (type_wait > 0) {
        type_wait--;
        return;
    }
    if (type_phase == 0 || type_phase == 3) {
        for (i = 0; i < 8; i++)
            m->keys[i] = 0x1f;
    }
    if (type_phase == 0) {
        if (media_char_key((unsigned char)type_buf[type_pos], &row, &bit, &caps, &sym)) {
            zx_key(m, row, bit, 1);
            if (caps)
                zx_key(m, 0, 0, 1);
            if (sym)
                zx_key(m, 7, 1, 1);
        }
    }
    if (++type_phase == 10) {
        type_phase = 0;
        type_pos++;
    }
}
