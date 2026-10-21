/*
 * s256trace: run a 48K game with the Spec256 tracer and report which bytes
 * of memory are drawn to the screen, in what colours and at what width.
 *
 *   s256trace [-r romdir] [-f frames] [-k seed] [-s every] [-g dir] game.sna|game.z80 out-dir
 *
 * The game runs for `frames` frames (default 6000, two minutes) with keys
 * and the Kempston joystick pressed at random. out-dir gets:
 *
 *   start.sna    the game as a 48K .sna, from a .z80 (Spec256 needs .sna)
 *   mem.bin      the 64K memory map at the start (ROM and RAM)
 *   trace.json   one entry per drawn byte (see below)
 *   shotNNNN.ppm a screenshot every `every` frames (default 1000; 0 = none)
 *
 * With -g, the game plays in colour with the Spec256 files in dir instead
 * (an empty dir plays it as it was): the same keys at the same moments, so
 * the screenshots compare frame for frame. Nothing is traced.
 *
 * A byte counts as drawn when it reaches the bitmap unchanged, or through
 * OR or XOR onto blank screen, from where it lies at the start (the tracer
 * follows the value through registers, the stack and buffers). Each entry:
 *
 *   {"a": address, "n": times drawn, "ink": [16 counts], "paper": [16],
 *    "left": count, "up": count, "stride": {"d": count}, "across": {"d": count}}
 *
 * ink and paper count the attribute of the cell at the end of the frame it
 * was drawn in (colour + 8 for bright). stride counts, for each distance d
 * in bytes, how often the byte d further on was drawn on the pixel row just
 * above or below: d is the sprite's width in bytes. left counts how often
 * the byte before it was drawn just to its left (0 for a row's first byte),
 * up how many of the stride counts were for a row above (sprites stored
 * bottom row first). across counts, for each distance d, how often the byte
 * d further on was drawn just to the right: 1 for sprites stored row by
 * row, the height for sprites stored a column at a time.
 */
#define _DEFAULT_SOURCE
#include "../../src/zx/zx.h"
#include "../../src/spec256/spec256.h"
#include "../../src/frontend/media.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STRIDE 16
#define MAX_ACROSS 64
#define RING 64
#define FRAME_MAX 65536

typedef struct {
    uint32_t n, exact, left, up;
    uint32_t ink[16], paper[16];
    uint32_t stride[MAX_STRIDE + 1];
    uint32_t across[MAX_ACROSS + 1];
} src_info;

typedef struct {
    zx *m;
    uint8_t start[65536];
    src_info *info;
    struct { uint16_t addr, src; } ring[RING], frame[FRAME_MAX];
    int ring_pos, nframe;
} tracer;

/* Bitmap address of pixel row y, byte column x, and back. */
static int row_of(uint16_t a) { return ((a >> 8) & 7) | ((a >> 2) & 0x38) | ((a >> 5) & 0xc0); }
static uint16_t addr_of(int y, int x) { return (uint16_t)(0x4000 | (y & 7) << 8 | (y & 0x38) << 2 | (y & 0xc0) << 5 | x); }

static void on_write(void *user, uint16_t addr, uint8_t v, uint16_t src)
{
    tracer *t = user;
    int i, y = row_of(addr), x = addr & 31;
    if (src < 0x3d00)
        return;                    /* tag 0, or ROM code: not graphics */
    if (src >= 0x4000 && src < 0x5800)
        return;
    if (t->start[src] != v)
        return;                    /* changed on the way (shifted, masked) */
    /* The width: a byte d further on drawn a pixel row above or below. */
    for (i = 0; i < RING && y > 0 && y < 191; i++) {
        int d = src - t->ring[i].src;
        uint16_t ra = t->ring[i].addr;
        if ((ra == addr_of(y - 1, x) || ra == addr_of(y + 1, x)) && d > 0 && d <= MAX_STRIDE) {
            t->info[t->ring[i].src].stride[d]++;
            if (ra == addr_of(y + 1, x))
                t->info[t->ring[i].src].up++;
            break;
        }
    }
    /* Drawn just right of a byte d before it: d is 1 within a row (so this
     * is not a row's first byte), the height for column-stored sprites. */
    for (i = 0; i < RING && x > 0; i++) {
        int d = src - t->ring[i].src;
        if (t->ring[i].addr == addr - 1 && d > 0 && d <= MAX_ACROSS) {
            t->info[t->ring[i].src].across[d]++;
            if (d == 1)
                t->info[src].left++;
            break;
        }
    }
    t->ring[t->ring_pos].addr = addr;
    t->ring[t->ring_pos].src = src;
    t->ring_pos = (t->ring_pos + 1) % RING;
    if (!v)
        return;                    /* blank: helps find widths, not graphics */
    t->info[src].exact++;
    if (t->nframe < FRAME_MAX) {
        t->frame[t->nframe].addr = addr;
        t->frame[t->nframe].src = src;
        t->nframe++;
    }
}

/* At the end of a frame: count the attribute each drawn byte ended up in. */
static void end_frame(tracer *t)
{
    int i;
    for (i = 0; i < t->nframe; i++) {
        uint16_t a = t->frame[i].addr;
        uint8_t at = zx_peek(t->m, (uint16_t)(0x5800 + (row_of(a) >> 3) * 32 + (a & 31)));
        src_info *s = &t->info[t->frame[i].src];
        int br = at & 0x40 ? 8 : 0;
        s->n++;
        s->ink[(at & 7) + br]++;
        s->paper[((at >> 3) & 7) + br]++;
    }
    t->nframe = 0;
}

static void save_ppm(const zx *m, const char *path)
{
    FILE *fp = fopen(path, "wb");
    int x, y;
    if (!fp)
        return;
    fprintf(fp, "P6\n%d %d\n255\n", m->screen_w, m->screen_h);
    for (y = 0; y < m->screen_h; y++)
        for (x = 0; x < m->screen_w; x++) {
            uint16_t c = m->pixels[y * m->pitch + x];
            fputc(((c >> 6) & 7) * 255 / 7, fp);
            fputc(((c >> 3) & 7) * 255 / 7, fp);
            fputc((c & 7) * 255 / 7, fp);
        }
    fclose(fp);
}

/* Keys a game is likely to use: Q A O P M Z X, space, enter, 0-9. One press
 * in four is any key instead (Y or N to a question, a game's own keys). */
static const unsigned char keys[][2] = {
    {2, 0}, {1, 0}, {5, 1}, {5, 0}, {7, 2}, {0, 1}, {0, 2}, {7, 0}, {6, 0},
    {4, 0}, {3, 0}, {3, 1}, {3, 2}, {3, 3}, {3, 4}, {4, 4}, {4, 3}, {4, 2}, {4, 1},
};
#define NKEYS (sizeof keys / sizeof keys[0])

static unsigned pick_key(void)
{
    if (rand() % 4)
        return (unsigned)rand() % NKEYS;
    return NKEYS + (unsigned)rand() % 40;       /* row * 5 + bit */
}

static void press(zx *m, unsigned k, int down)
{
    if (k < NKEYS)
        zx_key(m, keys[k][0], keys[k][1], down);
    else
        zx_key(m, (int)(k - NKEYS) / 5, (int)(k - NKEYS) % 5, down);
}

/* The machine as a 48K .sna (PC pushed on the stack, as the format has it). */
static int save_sna(zx *m, const char *path)
{
    z80 *c = &m->cpu;
    uint8_t h[27];
    uint16_t sp = (uint16_t)(c->sp - 2);
    int i;
    FILE *fp;
    if (sp < 0x4000)
        return -1;
    h[0] = c->i;
    h[1] = c->l_; h[2] = c->h_; h[3] = c->e_; h[4] = c->d_;
    h[5] = c->c_; h[6] = c->b_; h[7] = c->f_; h[8] = c->a_;
    h[9] = c->l; h[10] = c->h; h[11] = c->e; h[12] = c->d;
    h[13] = c->c; h[14] = c->b;
    h[15] = (uint8_t)c->iy; h[16] = (uint8_t)(c->iy >> 8);
    h[17] = (uint8_t)c->ix; h[18] = (uint8_t)(c->ix >> 8);
    h[19] = (uint8_t)(c->iff2 << 2);
    h[20] = c->r;
    h[21] = c->f; h[22] = c->a;
    h[23] = (uint8_t)sp; h[24] = (uint8_t)(sp >> 8);
    h[25] = c->im;
    h[26] = m->border;
    if (!(fp = fopen(path, "wb")))
        return -1;
    fwrite(h, 1, sizeof h, fp);
    for (i = 0x4000; i < 0x10000; i++) {
        uint8_t v = zx_peek(m, (uint16_t)i);
        if (i == sp)
            v = (uint8_t)c->pc;
        else if (i == sp + 1)
            v = (uint8_t)(c->pc >> 8);
        fputc(v, fp);
    }
    return fclose(fp);
}

static int is_z80(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (!strcmp(name + n - 4, ".z80") || !strcmp(name + n - 4, ".Z80"));
}

int main(int argc, char **argv)
{
    const char *romdir = "roms", *snap = NULL, *out = NULL, *gfx = NULL;
    long frames = 6000, every = 1000, f;
    unsigned seed = 1, held[3] = { 0, 0, 0 };
    static const char *const rom_names[3] = { "48.rom", "128-0.rom", "128-1.rom" };
    uint8_t *rom[3], *data;
    const uint8_t *roms[4];
    size_t len;
    char path[1024];
    static zx m;
    static tracer t;
    s256 *s;
    FILE *fp;
    int a, i, first = 1;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-r") && a + 1 < argc) romdir = argv[++a];
        else if (!strcmp(argv[a], "-f") && a + 1 < argc) frames = atol(argv[++a]);
        else if (!strcmp(argv[a], "-k") && a + 1 < argc) seed = (unsigned)atol(argv[++a]);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) every = atol(argv[++a]);
        else if (!strcmp(argv[a], "-g") && a + 1 < argc) gfx = argv[++a];
        else if (!snap) snap = argv[a];
        else out = argv[a];
    }
    if (!snap || !out) {
        fprintf(stderr, "usage: %s [-r romdir] [-f frames] [-k seed] [-s every] [-g dir] game.sna|game.z80 out-dir\n", argv[0]);
        return 2;
    }
    mkdir(out, 0777);              /* may be there already */
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", romdir, rom_names[i]);
        rom[i] = media_read_file(path, &len);
    }
    data = media_read_file(snap, &len);
    if (!data || !rom[0]) {
        fprintf(stderr, "cannot read %s or the ROMs in %s\n", snap, romdir);
        return 1;
    }
    if (is_z80(snap)) {
        /* Spec256 loads only .sna: play the game from a 48K .sna made from it. */
        static zx z;
        roms[0] = rom[0];
        roms[1] = roms[2] = roms[3] = NULL;
        snprintf(path, sizeof path, "%s/start.sna", out);
        if (zx_create(&z, ZX_48K, roms) || zx_load_z80(&z, data, len) || z.model != ZX_48K
            || save_sna(&z, path)) {
            fprintf(stderr, "%s: not a 48K snapshot, or its stack is in ROM\n", snap);
            return 1;
        }
        zx_free(&z);
        free(data);
        data = media_read_file(path, &len);
        snap = path;
    }
    if (!data || s256_snapshot_is_128(len)) {
        fprintf(stderr, "%s: only 48K snapshots for now\n", snap);
        return 1;
    }
    s = s256_new(rom[0], rom[1], rom[2]);
    if (gfx) {
        DIR *d = opendir(gfx);
        struct dirent *e;
        size_t n;
        if (!d) {
            perror(gfx);
            return 1;
        }
        while (d && (e = readdir(d)) != NULL) {
            uint8_t *f;
            n = strlen(e->d_name);
            if (e->d_name[0] == '.' || (n > 4 && !strcmp(e->d_name + n - 4, ".SNA")) || (n > 4 && !strcmp(e->d_name + n - 4, ".sna")))
                continue;
            snprintf(path, sizeof path, "%s/%s", gfx, e->d_name);
            if ((f = media_read_file(path, &n)) != NULL)
                s256_add_file(s, e->d_name, f, n);
            free(f);
        }
        closedir(d);
        s256_set_name(s, snap);
    }
    s256_roms(s, 0, roms);
    if (zx_create(&m, ZX_48K, roms) || (s256_attach(s, &m), zx_load_sna(&m, data, len))) {
        fprintf(stderr, "%s: cannot start\n", snap);
        return 1;
    }
    s256_start(s);

    t.m = &m;
    t.info = calloc(65536, sizeof *t.info);
    for (i = 0; i < 65536; i++)
        t.start[i] = zx_peek(&m, (uint16_t)i);
    snprintf(path, sizeof path, "%s/mem.bin", out);
    if (!(fp = fopen(path, "wb"))) {
        perror(path);
        return 1;
    }
    fwrite(t.start, 1, sizeof t.start, fp);
    fclose(fp);
    if (!gfx)
        s256_trace(s, on_write, &t);

    srand(seed);
    for (f = 0; f < frames; f++) {
        if (f % 20 == 0) {
            /* let go of the old keys, press up to three new ones */
            for (i = 0; i < 3; i++)
                if (held[i])
                    press(&m, held[i] - 1, 0);
            for (i = 0; i < 3; i++) {
                held[i] = rand() % 3 ? 0 : 1 + pick_key();
                if (held[i])
                    press(&m, held[i] - 1, 1);
            }
            m.kempston = (uint8_t)(rand() & 0x1f);
        }
        zx_run_frame(&m);
        if (!gfx)
            end_frame(&t);
        if (every && f % every == every - 1) {
            snprintf(path, sizeof path, "%s/shot%04ld.ppm", out, f + 1);
            save_ppm(&m, path);
        }
    }

    if (gfx)
        return 0;
    snprintf(path, sizeof path, "%s/trace.json", out);
    if (!(fp = fopen(path, "w"))) {
        perror(path);
        return 1;
    }
    fprintf(fp, "[");
    for (i = 0; i < 65536; i++) {
        src_info *si = &t.info[i];
        int k, firstd = 1;
        if (!si->n)
            continue;
        fprintf(fp, "%s\n{\"a\":%d,\"n\":%u,\"ink\":[", first ? "" : ",", i, si->n);
        for (k = 0; k < 16; k++)
            fprintf(fp, "%s%u", k ? "," : "", si->ink[k]);
        fprintf(fp, "],\"paper\":[");
        for (k = 0; k < 16; k++)
            fprintf(fp, "%s%u", k ? "," : "", si->paper[k]);
        fprintf(fp, "],\"left\":%u,\"up\":%u,\"stride\":{", si->left, si->up);
        for (k = 1; k <= MAX_STRIDE; k++)
            if (si->stride[k]) {
                fprintf(fp, "%s\"%d\":%u", firstd ? "" : ",", k, si->stride[k]);
                firstd = 0;
            }
        fprintf(fp, "},\"across\":{");
        for (k = 1, firstd = 1; k <= MAX_ACROSS; k++)
            if (si->across[k]) {
                fprintf(fp, "%s\"%d\":%u", firstd ? "" : ",", k, si->across[k]);
                firstd = 0;
            }
        fprintf(fp, "}}");
        first = 0;
    }
    fprintf(fp, "\n]\n");
    fclose(fp);
    return 0;
}
