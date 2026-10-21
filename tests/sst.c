/*
 * Runs SingleStepTests/z80 JSON files (https://github.com/SingleStepTests/z80)
 * against the core: registers, WZ, Q, RAM and the exact T-state count.
 *
 *   sst file.json [file.json ...]
 *
 * Exit status is the number of files with failures.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/z80/z80.h"

#include "json.h"

/* ---- test bus --------------------------------------------------------- */

static uint8_t mem[65536];
static unsigned long tstates;
static jval *ports;
static int port_idx;

static uint8_t b_fetch(void *c, uint16_t a) { (void)c; tstates += 4; return mem[a]; }
static uint8_t b_read(void *c, uint16_t a) { (void)c; tstates += 3; return mem[a]; }
static void b_write(void *c, uint16_t a, uint8_t v) { (void)c; tstates += 3; mem[a] = v; }
static uint8_t b_in(void *c, uint16_t p)
{
    (void)c; (void)p;
    tstates += 4;
    if (ports && port_idx < ports->n)
        return (uint8_t)ports->kids[port_idx++].kids[1].num;
    return 0xff;
}
static void b_out(void *c, uint16_t p, uint8_t v) { (void)c; (void)p; (void)v; tstates += 4; port_idx++; }
static void b_internal(void *c, uint16_t a, unsigned n) { (void)c; (void)a; tstates += n; }
static void b_tick(void *c, unsigned n) { (void)c; tstates += n; }

static const z80_bus bus = { 0, b_fetch, b_read, b_write, b_in, b_out, b_internal, b_tick, 0, 0, 0, 0 };

static void load(z80 *z, jval *s)
{
    jval *ram = jget(s, "ram");
    int i;
    z->pc = (uint16_t)jnum(s, "pc"); z->sp = (uint16_t)jnum(s, "sp");
    z->a = (uint8_t)jnum(s, "a"); z->f = (uint8_t)jnum(s, "f");
    z->b = (uint8_t)jnum(s, "b"); z->c = (uint8_t)jnum(s, "c");
    z->d = (uint8_t)jnum(s, "d"); z->e = (uint8_t)jnum(s, "e");
    z->h = (uint8_t)jnum(s, "h"); z->l = (uint8_t)jnum(s, "l");
    z->i = (uint8_t)jnum(s, "i"); z->r = (uint8_t)jnum(s, "r");
    z->ix = (uint16_t)jnum(s, "ix"); z->iy = (uint16_t)jnum(s, "iy");
    z->memptr = (uint16_t)jnum(s, "wz");
    z->a_ = (uint8_t)(jnum(s, "af_") >> 8); z->f_ = (uint8_t)jnum(s, "af_");
    z->b_ = (uint8_t)(jnum(s, "bc_") >> 8); z->c_ = (uint8_t)jnum(s, "bc_");
    z->d_ = (uint8_t)(jnum(s, "de_") >> 8); z->e_ = (uint8_t)jnum(s, "de_");
    z->h_ = (uint8_t)(jnum(s, "hl_") >> 8); z->l_ = (uint8_t)jnum(s, "hl_");
    z->im = (uint8_t)jnum(s, "im");
    z->iff1 = (uint8_t)jnum(s, "iff1"); z->iff2 = (uint8_t)jnum(s, "iff2");
    z->q = (uint8_t)jnum(s, "q");
    z->ei_delay = (uint8_t)jnum(s, "ei");
    z->halted = 0;
    for (i = 0; ram && i < ram->n; i++)
        mem[ram->kids[i].kids[0].num & 0xffff] = (uint8_t)ram->kids[i].kids[1].num;
}

static int check(const char *name, z80 *z, jval *s, unsigned long want_t, int verbose)
{
    int bad = 0, i;
    jval *ram = jget(s, "ram");
#define CK(field, val) do { long w = jnum(s, field); if ((long)(val) != w) { \
        if (verbose) { printf("  %s: %s got %ld want %ld\n", name, field, (long)(val), w); } \
        bad++; } } while (0)
    CK("pc", z->pc); CK("sp", z->sp);
    CK("a", z->a); CK("f", z->f); CK("b", z->b); CK("c", z->c);
    CK("d", z->d); CK("e", z->e); CK("h", z->h); CK("l", z->l);
    CK("i", z->i); CK("r", z->r); CK("ix", z->ix); CK("iy", z->iy);
    CK("wz", z->memptr);
    CK("af_", z->a_ << 8 | z->f_); CK("bc_", z->b_ << 8 | z->c_);
    CK("de_", z->d_ << 8 | z->e_); CK("hl_", z->h_ << 8 | z->l_);
    CK("im", z->im); CK("iff1", z->iff1); CK("iff2", z->iff2);
    CK("q", z->q);
#undef CK
    for (i = 0; ram && i < ram->n; i++) {
        long a = ram->kids[i].kids[0].num, v = ram->kids[i].kids[1].num;
        if (mem[a & 0xffff] != v) {
            if (verbose) printf("  %s: ram[%04lx] got %02x want %02lx\n", name, a, mem[a & 0xffff], v);
            bad++;
        }
    }
    if (tstates != want_t) {
        if (verbose) printf("  %s: T-states got %lu want %lu\n", name, tstates, want_t);
        bad++;
    }
    return bad;
}

static int run_file(const char *path, int verbose)
{
    FILE *fp = fopen(path, "rb");
    long len;
    char *buf;
    jval root;
    int i, fails = 0;
    z80 z;

    if (!fp) {
        perror(path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fclose(fp);
        return 1;
    }
    buf[len] = 0;
    fclose(fp);
    memset(&root, 0, sizeof root);
    jp = buf;
    jparse(&root);

    z80_init(&z, 0);
    for (i = 0; i < root.n; i++) {
        jval *t = &root.kids[i];
        jval *cycles = jget(t, "cycles");
        const char *name = jget(t, "name")->str;
        load(&z, jget(t, "initial"));
        ports = jget(t, "ports");
        port_idx = 0;
        tstates = 0;
        z80_step(&z, &bus);
        if (check(name, &z, jget(t, "final"), (unsigned long)cycles->n, verbose && fails < 3))
            fails++;
    }
    printf("%s: %d/%d failed\n", path, fails, root.n);
    jfree(&root);
    free(buf);
    return fails != 0;
}

int main(int argc, char **argv)
{
    int i, bad = 0, verbose = 1;
    for (i = 1; i < argc; i++)
        bad += run_file(argv[i], verbose);
    return bad;
}
