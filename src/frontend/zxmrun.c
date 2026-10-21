/*
 * zxmrun: run any machine through the zxm interface, headless.
 *
 *   zxmrun -m machine [-r romdir] [-f frames] [-t text [-d delay]]
 *          [-i slot=file]... [-s out.ppm] [-w out.wav] [-k frame:row:bit[:frames]]...
 *          [-T frame:text]...
 *
 * -i puts a file in a slot as bytes (a tape, a disk image, a snapshot).
 * -k holds a key from a frame for a number of frames (default 10).
 * -T starts typing text at a frame (after anything typed before it).
 * The machine list: zxmrun -l.
 */
#include "../core/zxm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n;
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) == 0 && (n = ftell(fp)) > 0 && fseek(fp, 0, SEEK_SET) == 0
        && (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, fp) == (size_t)n) {
        *len = (uint32_t)n;
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    return buf;
}

static void save_ppm(const zxm *m, const char *path)
{
    const zxm_info *info = zxm_info_of(m);
    const uint16_t *fb;
    int pitch, x, y;
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        return;
    }
    fb = zxm_framebuffer(m, &pitch);
    fprintf(fp, "P6\n%d %d\n255\n", info->fb_w, info->fb_h);
    for (y = 0; y < info->fb_h; y++)
        for (x = 0; x < info->fb_w; x++) {
            uint16_t c = fb[y * pitch + x];
            fputc(((c >> 6) & 7) * 255 / 7, fp);
            fputc(((c >> 3) & 7) * 255 / 7, fp);
            fputc((c & 7) * 255 / 7, fp);
        }
    fclose(fp);
}

static void put32(FILE *fp, uint32_t v) { fputc(v & 255, fp); fputc(v >> 8 & 255, fp); fputc(v >> 16 & 255, fp); fputc(v >> 24 & 255, fp); }
static void put16(FILE *fp, uint32_t v) { fputc(v & 255, fp); fputc(v >> 8 & 255, fp); }

typedef struct { long frame, len; int row, bit; } held_key;

/* "\n" in typed text is Enter. */
static char *unescape(const char *text)
{
    char *t = malloc(strlen(text) + 1), *r, *w;
    if (!t)
        exit(1);
    strcpy(t, text);
    for (r = w = t; *r; r++)
        *w++ = r[0] == '\\' && r[1] == 'n' ? (r++, '\n') : *r;
    *w = 0;
    return t;
}

int main(int argc, char **argv)
{
    const char *id = NULL, *romdir = "roms", *shot = NULL, *wav = NULL, *text = NULL;
    const char *media[8];
    held_key keys[32];
    const char *later[16];
    long later_at[16];
    int nmedia = 0, nkeys = 0, nlater = 0, a, i;
    long frames = 100, delay = 0, f;
    const zxm_info *info;
    zxm_rom_set roms;
    zxm *m;
    FILE *wf = NULL;
    uint32_t wav_samples = 0;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-l")) {
            for (i = 0; (info = zxm_machine(i)) != NULL; i++)
                printf("%-10s %s\n", info->id, info->name);
            return 0;
        } else if (!strcmp(argv[a], "-m") && a + 1 < argc) id = argv[++a];
        else if (!strcmp(argv[a], "-r") && a + 1 < argc) romdir = argv[++a];
        else if (!strcmp(argv[a], "-f") && a + 1 < argc) frames = atol(argv[++a]);
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) text = argv[++a];
        else if (!strcmp(argv[a], "-d") && a + 1 < argc) delay = atol(argv[++a]);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) shot = argv[++a];
        else if (!strcmp(argv[a], "-w") && a + 1 < argc) wav = argv[++a];
        else if (!strcmp(argv[a], "-i") && a + 1 < argc && nmedia < 8) media[nmedia++] = argv[++a];
        else if (!strcmp(argv[a], "-k") && a + 1 < argc && nkeys < 32) {
            held_key *k = &keys[nkeys++];
            k->len = 10;
            if (sscanf(argv[++a], "%ld:%d:%d:%ld", &k->frame, &k->row, &k->bit, &k->len) < 3) {
                fprintf(stderr, "bad key %s\n", argv[a]);
                return 2;
            }
        } else if (!strcmp(argv[a], "-T") && a + 1 < argc && nlater < 16) {
            char *colon = strchr(argv[++a], ':');
            if (!colon) {
                fprintf(stderr, "bad -T %s (want frame:text)\n", argv[a]);
                return 2;
            }
            later_at[nlater] = atol(argv[a]);
            later[nlater++] = colon + 1;
        } else {
            fprintf(stderr, "usage: %s -m machine [-r romdir] [-f frames] [-t text [-d delay]] "
                    "[-i slot=file]... [-k frame:row:bit[:frames]]... [-T frame:text]... [-s out.ppm] [-w out.wav] | -l\n", argv[0]);
            return 2;
        }
    }
    if (!id || !(info = zxm_lookup(id))) {
        fprintf(stderr, "unknown machine %s (zxmrun -l lists them)\n", id ? id : "");
        return 2;
    }

    memset(&roms, 0, sizeof roms);
    for (i = 0; i < 4 && info->roms[i]; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", romdir, info->roms[i]);
        if (!(roms.rom[i] = slurp(path, &roms.len[i])) && !(info->rom_optional >> i & 1)) {
            fprintf(stderr, "cannot read %s\n", path);
            return 1;
        }
    }
    m = malloc(zxm_size(info));
    if (!m || zxm_create(m, info, &roms)) {
        fprintf(stderr, "cannot create %s\n", info->id);
        return 1;
    }
    for (i = 0; i < nmedia; i++) {
        char slot[64];
        const char *eq = strchr(media[i], '=');
        zxm_media md;
        if (!eq || (size_t)(eq - media[i]) >= sizeof slot) {
            fprintf(stderr, "bad media %s (want slot=file)\n", media[i]);
            return 2;
        }
        memcpy(slot, media[i], (size_t)(eq - media[i]));
        slot[eq - media[i]] = 0;
        memset(&md, 0, sizeof md);
        md.kind = ZXM_MEDIA_BYTES;
        md.name = eq + 1;
        if (!(md.data = slurp(eq + 1, &md.len)) || zxm_media_insert(m, slot, &md)) {
            fprintf(stderr, "cannot put %s in %s\n", eq + 1, slot);
            return 1;
        }
    }
    if (text) {
        char *t = unescape(text);
        zxm_type(m, t, (int)delay);
        free(t);
    }
    if (wav && (wf = fopen(wav, "wb")) != NULL)
        fseek(wf, 44, SEEK_SET);

    for (f = 0; f < frames; f++) {
        for (i = 0; i < nlater; i++) {
            if (later_at[i] == f) {
                char *t = unescape(later[i]);
                zxm_type(m, t, 0);
                free(t);
            }
        }
        for (i = 0; i < nkeys; i++) {
            zxm_input_event ev;
            if (f != keys[i].frame && f != keys[i].frame + keys[i].len)
                continue;
            memset(&ev, 0, sizeof ev);
            ev.kind = ZXM_EV_KEY;
            ev.a = (uint8_t)keys[i].row;
            ev.b = (uint8_t)keys[i].bit;
            ev.down = f == keys[i].frame;
            zxm_input(m, &ev);
        }
        zxm_run_frame(m);
        if (wf) {
            int n;
            const int16_t *au = zxm_audio(m, &n);
            for (i = 0; i < n * 2; i++)
                put16(wf, (uint16_t)au[i]);
            wav_samples += (uint32_t)n;
        }
    }
    if (shot)
        save_ppm(m, shot);
    if (wf) {
        fseek(wf, 0, SEEK_SET);
        fputs("RIFF", wf); put32(wf, 36 + wav_samples * 4); fputs("WAVEfmt ", wf);
        put32(wf, 16); put16(wf, 1); put16(wf, 2); put32(wf, info->audio_rate);
        put32(wf, info->audio_rate * 4); put16(wf, 4); put16(wf, 16);
        fputs("data", wf); put32(wf, wav_samples * 4);
        fclose(wf);
    }
    zxm_destroy(m);
    free(m);
    return 0;
}
