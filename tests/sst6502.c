/*
 * Runs SingleStepTests/65x02 6502 JSON files
 * (https://github.com/SingleStepTests/65x02) against the 6502 core:
 * registers, RAM, and every bus cycle's address, data and direction.
 *
 *   sst6502 file.json [file.json ...]
 *
 * Exit status is the number of files with failures.
 */
#include <stdio.h>

#include "../src/m6502/m6502.h"

#include "json.h"

/* ---- test bus: records each cycle ------------------------------------- */

static uint8_t mem[65536];
static struct { uint16_t addr; uint8_t val, write; } log_[64];
static int ncyc;

static uint8_t b_read(void *c, uint16_t a)
{
    (void)c;
    if (ncyc < 64) {
        log_[ncyc].addr = a; log_[ncyc].val = mem[a]; log_[ncyc].write = 0;
    }
    ncyc++;
    return mem[a];
}

static void b_write(void *c, uint16_t a, uint8_t v)
{
    (void)c;
    if (ncyc < 64) {
        log_[ncyc].addr = a; log_[ncyc].val = v; log_[ncyc].write = 1;
    }
    ncyc++;
    mem[a] = v;
}

static const m6502_bus bus = { 0, b_read, b_write };

static void load(m6502 *c, jval *s)
{
    jval *ram = jget(s, "ram");
    int i;
    m6502_init(c);
    c->reset_pending = 0;
    c->pc = (uint16_t)jnum(s, "pc"); c->s = (uint8_t)jnum(s, "s");
    c->a = (uint8_t)jnum(s, "a"); c->x = (uint8_t)jnum(s, "x");
    c->y = (uint8_t)jnum(s, "y"); c->p = (uint8_t)jnum(s, "p");
    for (i = 0; ram && i < ram->n; i++)
        mem[ram->kids[i].kids[0].num & 0xffff] = (uint8_t)ram->kids[i].kids[1].num;
}

static int check(const char *name, m6502 *c, jval *s, jval *cycles, int verbose)
{
    int bad = 0, i;
    jval *ram = jget(s, "ram");
#define CK(field, val) do { long w = jnum(s, field); if ((long)(val) != w) { \
        if (verbose) { printf("  %s: %s got %ld want %ld\n", name, field, (long)(val), w); } \
        bad++; } } while (0)
    CK("pc", c->pc); CK("s", c->s);
    CK("a", c->a); CK("x", c->x); CK("y", c->y); CK("p", c->p);
#undef CK
    for (i = 0; ram && i < ram->n; i++) {
        long a = ram->kids[i].kids[0].num, v = ram->kids[i].kids[1].num;
        if (mem[a & 0xffff] != v) {
            if (verbose) printf("  %s: ram[%04lx] got %02x want %02lx\n", name, a, mem[a & 0xffff], v);
            bad++;
        }
    }
    if (ncyc != cycles->n) {
        if (verbose) printf("  %s: %d cycles, want %d\n", name, ncyc, cycles->n);
        return bad + 1;
    }
    for (i = 0; i < ncyc && i < 64; i++) {
        jval *e = &cycles->kids[i];
        int w = e->kids[2].str[0] == 'w';
        if (log_[i].addr != e->kids[0].num || log_[i].val != e->kids[1].num || log_[i].write != w) {
            if (verbose)
                printf("  %s: cycle %d %04x %02x %s, want %04lx %02lx %s\n", name, i,
                       log_[i].addr, log_[i].val, log_[i].write ? "write" : "read",
                       e->kids[0].num, e->kids[1].num, e->kids[2].str);
            bad++;
            break;
        }
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
    m6502 c;

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

    for (i = 0; i < root.n; i++) {
        jval *t = &root.kids[i];
        jval *cycles = jget(t, "cycles");
        const char *name = jget(t, "name")->str;
        load(&c, jget(t, "initial"));
        ncyc = 0;
        m6502_step(&c, &bus);
        /* A JAM keeps the bus busy: run it for as many cycles as the test
         * recorded. */
        while (c.jammed && ncyc < cycles->n)
            m6502_step(&c, &bus);
        if (check(name, &c, jget(t, "final"), cycles, verbose && fails < 3))
            fails++;
    }
    printf("%s: %d/%d failed\n", path, fails, root.n);
    jfree(&root);
    free(buf);
    return fails != 0;
}

int main(int argc, char **argv)
{
    int i, bad = 0;
    for (i = 1; i < argc; i++)
        bad += run_file(argv[i], 1);
    return bad;
}
