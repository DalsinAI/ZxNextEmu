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
    } else if (has_ext(path, ".trd") || has_ext(path, ".scl")) {
        r = zx_attach_disk_image(m, 0, d, len, has_ext(path, ".scl"));
    } else {
        fprintf(stderr, "%s: unknown file type\n", path);
    }
    if (r)
        fprintf(stderr, "%s: could not load\n", path);
    free(d);
    return r;
}
