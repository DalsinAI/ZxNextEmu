/*
 * Runs a CP/M .COM Z80 exerciser (ZEXDOC / ZEXALL) on the core, with just
 * enough BDOS (functions 2 and 9) to print its output.
 *
 *   zex tests-data/zexdoc.com
 */
#include <stdio.h>
#include <stdlib.h>

#include "../src/z80/z80.h"

static uint8_t mem[65536];
static unsigned long long tstates;

static uint8_t b_fetch(void *c, uint16_t a) { (void)c; tstates += 4; return mem[a]; }
static uint8_t b_read(void *c, uint16_t a) { (void)c; tstates += 3; return mem[a]; }
static void b_write(void *c, uint16_t a, uint8_t v) { (void)c; tstates += 3; mem[a] = v; }
static uint8_t b_in(void *c, uint16_t p) { (void)c; (void)p; tstates += 4; return 0xff; }
static void b_out(void *c, uint16_t p, uint8_t v) { (void)c; (void)p; (void)v; tstates += 4; }
static void b_internal(void *c, uint16_t a, unsigned n) { (void)c; (void)a; tstates += n; }
static void b_tick(void *c, unsigned n) { (void)c; tstates += n; }

int main(int argc, char **argv)
{
    z80 cpu;
    z80_bus bus = { 0, b_fetch, b_read, b_write, b_in, b_out, b_internal, b_tick, 0, 0, 0 };
    FILE *fp;
    size_t n;
    int failed = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s file.com\n", argv[0]);
        return 2;
    }
    fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 2;
    }
    n = fread(mem + 0x100, 1, sizeof mem - 0x100, fp);
    fclose(fp);
    (void)n;

    z80_init(&cpu, 0);
    cpu.pc = 0x100;
    cpu.sp = 0xf000;
    mem[0] = 0x76; /* warm boot: HALT */
    mem[5] = 0xc9; /* BDOS: RET (intercepted below) */

    for (;;) {
        if (cpu.pc == 5) {
            if (cpu.c == 2) {
                putchar(cpu.e);
            } else if (cpu.c == 9) {
                uint16_t a = (uint16_t)(cpu.d << 8 | cpu.e);
                while (mem[a] != '$') {
                    putchar(mem[a]);
                    a++;
                }
            }
            fflush(stdout);
        }
        if (cpu.pc == 0)
            break;
        z80_step(&cpu, &bus);
        if (cpu.halted)
            break;
    }
    printf("\n%llu T-states\n", tstates);
    return failed;
}
