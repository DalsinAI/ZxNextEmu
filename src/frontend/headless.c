/*
 * zxrun: headless runner for tests and scripting.
 *
 *   zxrun [-m model] [-r romdir] [-c sd-folder-or-image] [-p pi-folder] [-g spec256-folder] [-f frames] [-t text [-d delay]] [-s out.ppm] [-w out.wav] [file]
 *
 * -g runs a Spec256 game: a folder with its .sna and graphics files.
 * -c attaches an SD card (ZX Spectrum Next, -m next): a folder or an image.
 * -p fits the Next's Raspberry Pi, keeping its files in a folder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "../zx/zx.h"
#include "../zx/tape.h"
#include "media.h"
#include "../zx/keys.h"
#include "net_posix.h"
#include "../next/next.h"
#include "../devices/esp8266.h"
#include "../devices/nextpi.h"
#include "../spec256/spec256.h"

static void save_ppm(const zx *m, const char *path)
{
    FILE *fp = fopen(path, "wb");
    int x, y;
    if (!fp) {
        perror(path);
        return;
    }
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

/* A Spec256 game folder: every graphics file, then the snapshot, which
 * picks 48K or 128K. */
static s256 *load_spec256(zx *m, const char *dir, const char *romdir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024], sna[1024] = "";
    uint8_t *data, *rom[3];
    size_t len, rlen;
    const uint8_t *roms[4];
    static const char *const rom_names[3] = { "48.rom", "128-0.rom", "128-1.rom" };
    s256 *s;
    int i;

    if (!d) {
        perror(dir);
        return NULL;
    }
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", romdir, rom_names[i]);
        rom[i] = media_read_file(path, &rlen);
    }
    s = s256_new(rom[0], rom[1], rom[2]);
    for (i = 0; i < 3; i++)
        free(rom[i]);
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (n > 4 && (!strcmp(e->d_name + n - 4, ".sna") || !strcmp(e->d_name + n - 4, ".SNA"))) {
            snprintf(sna, sizeof sna, "%s", path);
            continue;
        }
        data = media_read_file(path, &len);
        if (data && !s256_add_file(s, e->d_name, data, len))
            printf("spec256: %s\n", e->d_name);
        free(data);
    }
    closedir(d);
    data = sna[0] ? media_read_file(sna, &len) : NULL;
    if (!data) {
        fprintf(stderr, "%s: no .sna snapshot\n", dir);
        return NULL;
    }
    s256_roms(s, s256_snapshot_is_128(len), roms);
    if (zx_create(m, s256_snapshot_is_128(len) ? ZX_128K : ZX_48K, roms)) {
        fprintf(stderr, "spec256: ROMs missing in %s\n", romdir);
        return NULL;
    }
    s256_attach(s, m);
    if (zx_load_sna(m, data, len)) {
        fprintf(stderr, "%s: bad snapshot\n", sna);
        return NULL;
    }
    free(data);
    s256_set_name(s, sna);
    s256_start(s);
    return s;
}

static void put32(FILE *fp, uint32_t v) { fputc(v & 255, fp); fputc(v >> 8 & 255, fp); fputc(v >> 16 & 255, fp); fputc(v >> 24 & 255, fp); }
static void put16(FILE *fp, uint32_t v) { fputc(v & 255, fp); fputc(v >> 8 & 255, fp); }

int main(int argc, char **argv)
{
    static zx m;
    static zx_typer typer;
    zx_model model = ZX_48K;
    const char *romdir = "roms", *gfxdir = NULL, *shot = NULL, *wav = NULL, *file = NULL, *text = NULL, *sd = NULL, *pidir = NULL;
    long frames = 100, delay = 120, i;
    FILE *wf = NULL;
    zx_net *net = NULL;
    s256 *spec = NULL;
    uint32_t wav_samples = 0;
    int a, noscroll = 0;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-m") && a + 1 < argc) {
            if (zx_model_from_name(argv[++a], &model)) {
                fprintf(stderr, "unknown model %s\n", argv[a]);
                return 2;
            }
        } else if (!strcmp(argv[a], "-r") && a + 1 < argc) romdir = argv[++a];
        else if (!strcmp(argv[a], "-c") && a + 1 < argc) sd = argv[++a];
        else if (!strcmp(argv[a], "-p") && a + 1 < argc) pidir = argv[++a];
        else if (!strcmp(argv[a], "-g") && a + 1 < argc) gfxdir = argv[++a];
        else if (!strcmp(argv[a], "-f") && a + 1 < argc) frames = atol(argv[++a]);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) shot = argv[++a];
        else if (!strcmp(argv[a], "-w") && a + 1 < argc) wav = argv[++a];
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) text = argv[++a];
        else if (!strcmp(argv[a], "-d") && a + 1 < argc) delay = atol(argv[++a]);
        else if (!strcmp(argv[a], "-n")) noscroll = 1; /* keep BASIC from asking "scroll?" */
        else if (argv[a][0] != '-') file = argv[a];
        else {
            fprintf(stderr, "usage: %s [-m model] [-r romdir] [-c sd-folder-or-image] [-p pi-folder] [-g spec256-folder] [-f frames] [-t text [-d delay]] [-n] [-s out.ppm] [-w out.wav] [file]\n", argv[0]);
            return 2;
        }
    }
    if (gfxdir) {
        spec = load_spec256(&m, gfxdir, romdir);
        if (!spec)
            return 1;
        model = m.model;
    } else if (zx_init(&m, model, romdir))
        return 1;
    if (model == ZX_NEXT) {
        net = zx_net_posix_new();
        nx_attach_serial(&m, 0, zx_esp8266_new(net));   /* the wifi module */
        if (pidir)
            nx_attach_serial(&m, 1, zx_nextpi_new(pidir));
    }
    if (sd && zx_attach_sd(&m, 0, sd)) {
        fprintf(stderr, "cannot attach SD card image %s\n", sd);
        return 1;
    }
    if (file && media_load(&m, file))
        return 1;
    if (text) {
        zx_type(&typer, text);
        zx_type_delay(&typer, (int)delay);
    }
    if (wav) {
        wf = fopen(wav, "wb");
        if (wf)
            fseek(wf, 44, SEEK_SET);
    }
    for (i = 0; i < frames; i++) {
        zx_type_frame(&typer, &m);
        if (noscroll && i >= delay)
            zx_poke(&m, 23692, 255); /* SCR-CT; not before the ROM's RAM test */
        zx_run_frame(&m);
        if (wf) {
            fwrite(m.audio, 4, (size_t)m.audio_samples, wf);
            wav_samples += (uint32_t)m.audio_samples;
        }
    }
    if (wf) {
        fseek(wf, 0, SEEK_SET);
        fputs("RIFF", wf); put32(wf, 36 + wav_samples * 4); fputs("WAVEfmt ", wf);
        put32(wf, 16); put16(wf, 1); put16(wf, 2); put32(wf, ZX_AUDIO_RATE);
        put32(wf, ZX_AUDIO_RATE * 4); put16(wf, 4); put16(wf, 16);
        fputs("data", wf); put32(wf, wav_samples * 4);
        fclose(wf);
    }
    if (shot)
        save_ppm(&m, shot);
    printf("%s: %ld frames, PC=%04X\n", zx_timings[model].name, frames, m.cpu.pc);
    zx_free(&m);
    s256_free(spec);
    zx_net_posix_free(net);
    return 0;
}
