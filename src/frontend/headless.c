/*
 * zxrun: headless runner for tests and scripting.
 *
 *   zxrun [-m model] [-r romdir] [-c sd-folder-or-image] [-f frames] [-t text [-d delay]] [-s out.ppm] [-w out.wav] [file]
 *
 * -c attaches an SD card image (ZX Spectrum Next, -m next).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zx/zx.h"
#include "../zx/tape.h"
#include "media.h"
#include "net_posix.h"
#include "../next/next.h"
#include "../devices/esp8266.h"

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

static void put32(FILE *fp, uint32_t v) { fputc(v & 255, fp); fputc(v >> 8 & 255, fp); fputc(v >> 16 & 255, fp); fputc(v >> 24 & 255, fp); }
static void put16(FILE *fp, uint32_t v) { fputc(v & 255, fp); fputc(v >> 8 & 255, fp); }

int main(int argc, char **argv)
{
    static zx m;
    zx_model model = ZX_48K;
    const char *romdir = "roms", *shot = NULL, *wav = NULL, *file = NULL, *text = NULL, *sd = NULL;
    long frames = 100, delay = 120, i;
    FILE *wf = NULL;
    zx_net *net = NULL;
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
        else if (!strcmp(argv[a], "-f") && a + 1 < argc) frames = atol(argv[++a]);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) shot = argv[++a];
        else if (!strcmp(argv[a], "-w") && a + 1 < argc) wav = argv[++a];
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) text = argv[++a];
        else if (!strcmp(argv[a], "-d") && a + 1 < argc) delay = atol(argv[++a]);
        else if (!strcmp(argv[a], "-n")) noscroll = 1; /* keep BASIC from asking "scroll?" */
        else if (argv[a][0] != '-') file = argv[a];
        else {
            fprintf(stderr, "usage: %s [-m model] [-r romdir] [-c sd-folder-or-image] [-f frames] [-t text [-d delay]] [-n] [-s out.ppm] [-w out.wav] [file]\n", argv[0]);
            return 2;
        }
    }
    if (zx_init(&m, model, romdir))
        return 1;
    if (model == ZX_NEXT) {
        net = zx_net_posix_new();
        nx_attach_serial(&m, 0, zx_esp8266_new(net));   /* the wifi module */
    }
    if (sd && zx_attach_sd(&m, 0, sd)) {
        fprintf(stderr, "cannot attach SD card image %s\n", sd);
        return 1;
    }
    if (file && media_load(&m, file))
        return 1;
    if (text) {
        media_type(text);
        media_type_delay((int)delay);
    }
    if (wav) {
        wf = fopen(wav, "wb");
        if (wf)
            fseek(wf, 44, SEEK_SET);
    }
    for (i = 0; i < frames; i++) {
        media_type_frame(&m);
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
    zx_net_posix_free(net);
    return 0;
}
