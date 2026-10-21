/* Typing text into a machine: ASCII to the Spectrum keyboard matrix. */
#include "keys.h"

#include <ctype.h>
#include <string.h>

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

int zx_char_key(int ch, int *row, int *bit, int *caps, int *sym)
{
    int r, b;
    const char *s;
    *caps = *sym = 0;
    if (ch == '\r')
        ch = '\n';
    if (isupper(ch)) {
        *caps = 1;
        ch = tolower(ch);
    } else if (ch == '|') {   /* cursor down (Caps Shift + 6), for menus */
        *caps = 1;
        ch = '6';
    } else if (ch == '~') {   /* Caps Shift + Symbol Shift: the 48K's extended mode */
        *caps = *sym = 1;
        return 0;
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

void zx_type_delay(zx_typer *ty, int frames)
{
    ty->wait = frames;
}

void zx_type(zx_typer *ty, const char *text)
{
    size_t n = strlen(text);
    if (n > sizeof ty->buf)
        n = sizeof ty->buf;
    memcpy(ty->buf, text, n);
    ty->len = (uint32_t)n;
    ty->pos = 0;
    ty->phase = 0;
}

int zx_typing(const zx_typer *ty)
{
    return ty->pos < ty->len;
}

/* Each key is held for 3 frames and released for 7: the ROM keeps a
 * released key in KSTATE for 5 interrupts, so the same key twice in a
 * row needs the longer gap. */
void zx_type_frame(zx_typer *ty, zx *m)
{
    int row, bit, caps, sym, i;
    if (ty->pos >= ty->len)
        return;
    if (ty->wait > 0) {
        ty->wait--;
        return;
    }
    if (ty->phase == 0 || ty->phase == 3) {
        for (i = 0; i < 8; i++)
            m->keys[i] = 0x1f;
    }
    if (ty->phase == 0) {
        /* A character with no key (and not ~) presses nothing: a pause of one key's time. */
        if (zx_char_key((unsigned char)ty->buf[ty->pos], &row, &bit, &caps, &sym))
            zx_key(m, row, bit, 1);
        if (caps)
            zx_key(m, 0, 0, 1);
        if (sym)
            zx_key(m, 7, 1, 1);
    }
    if (++ty->phase == 10) {
        ty->phase = 0;
        ty->pos++;
    }
}
