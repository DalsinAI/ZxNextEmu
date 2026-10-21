/*
 * zxmrun: runs any zxm machine headless, through the same interface ZX
 * Cradle uses. For tests and scripting.
 *
 *   zxmrun -m machine [-r romdir] [-R n=file] [-f frames] [-t text [-d delay]]
 *          [-s out.ppm] [-w out.wav] [-x] [-p addr:len] [-P] [-k frame:row:bit[:frames]]...
 *          [-T frame:text]... [[-i] slot=file ...]
 *   zxmrun -l
 *
 * -x resets (power on) after the media go in. -p prints memory, as the
 * CPU sees it, after the run; -P prints the CPU's registers.
 * -R replaces ROM n (0..3, in the order the machine lists them); -R n=
 * leaves an optional ROM out. Media go in as slot=file: cart1=game.rom,
 * disk0=disk.dsk, ide0=folder-or-image, tape=game.tap. Block devices
 * (disks, SD cards) can be image files or folders; a disk slot that does
 * not take a block device gets the file's bytes (SAM Coupé, Beta 128).
 * -k holds a key from a frame for a number of frames (default 10).
 * -T starts typing text at a frame (after anything typed before it).
 * -l lists the machines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/zxm.h"
#include "../storage/blockdev.h"

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n;
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) == 0 && (n = ftell(fp)) > 0 && n < 64L * 1024 * 1024
        && fseek(fp, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n)) != NULL
        && fread(buf, 1, (size_t)n, fp) == (size_t)n) {
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
    int pitch, x, y;
    const uint16_t *fb = zxm_framebuffer(m, &pitch);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        return;
    }
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

static void put32(FILE *fp, uint32_t v)
{
    fputc((int)(v & 255), fp);
    fputc((int)(v >> 8 & 255), fp);
    fputc((int)(v >> 16 & 255), fp);
    fputc((int)(v >> 24 & 255), fp);
}

static void wav_header(FILE *fp, uint32_t rate, uint32_t samples)
{
    fwrite("RIFF", 1, 4, fp);
    put32(fp, 36 + samples * 4);
    fwrite("WAVEfmt ", 1, 8, fp);
    put32(fp, 16);
    put32(fp, 1 | 2u << 16);
    put32(fp, rate);
    put32(fp, rate * 4);
    put32(fp, 4 | 16u << 16);
    fwrite("data", 1, 4, fp);
    put32(fp, samples * 4);
}

typedef struct { long frame, len; int row, bit; } held_key;

/* "\n" in typed text is Enter. */
static char *unescape(const char *text)
{
    char *t = malloc(strlen(text) + 1), *w = t;
    const char *r;
    if (!t)
        exit(1);
    for (r = text; *r; r++)
        *w++ = r[0] == '\\' && r[1] == 'n' ? (r++, '\n') : *r;
    *w = 0;
    return t;
}

static const zxm_slot *slot_of(const zxm *m, const char *id)
{
    const zxm_slot *s;
    for (s = zxm_slots(m); s->id; s++)
        if (!strcmp(s->id, id))
            return s;
    return NULL;
}

static int insert(zxm *m, const char *arg)
{
    const zxm_info *info = zxm_info_of(m);
    char id[32];
    const char *eq = strchr(arg, '='), *path;
    const zxm_slot *s;
    zxm_media media;
    size_t n;
    if (!eq || (n = (size_t)(eq - arg)) >= sizeof id)
        return -1;
    memcpy(id, arg, n);
    id[n] = 0;
    path = eq + 1;
    if (!(s = slot_of(m, id))) {
        fprintf(stderr, "%s has no slot %s\n", info->id, id);
        return -1;
    }
    memset(&media, 0, sizeof media);
    media.name = path;
    if (s->kind == ZXM_SLOT_SD || s->kind == ZXM_SLOT_DISK || s->kind == ZXM_SLOT_HARDDISK) {
        media.kind = ZXM_MEDIA_BLOCKS;
        /* hard disks are FAT16 folders: Nextor and the like do not read FAT32 */
        media.blocks = s->kind == ZXM_SLOT_HARDDISK ? zx_bd_open16(path) : zx_bd_open(path);
        if (!media.blocks) {
            fprintf(stderr, "cannot open %s\n", path);
            return -1;
        }
        if (!zxm_media_insert(m, id, &media))
            return 0;
        zx_bd_close(media.blocks);
        media.blocks = NULL;
    }
    if (s->kind != ZXM_SLOT_SD && s->kind != ZXM_SLOT_HARDDISK) {
        media.kind = ZXM_MEDIA_BYTES;
        media.data = slurp(path, &media.len);
        if (!media.data) {
            fprintf(stderr, "cannot read %s\n", path);
            return -1;
        }
    }
    if (zxm_media_insert(m, id, &media)) {
        fprintf(stderr, "%s will not take %s\n", id, path);
        return -1;
    }
    free((void *)media.data);
    return 0;
}

int main(int argc, char **argv)
{
    const char *id = "msx2", *romdir = "roms", *text = NULL, *shot = NULL, *wav = NULL;
    const char *rom_override[4] = { NULL, NULL, NULL, NULL };
    int frames = 300, delay = 0, i, reset_after = 0, nmedia = 0, regs = 0;
    unsigned dump_addr = 0, dump_len = 0;
    const char *media[16];
    held_key keys[32];
    const char *later[16];
    long later_at[16];
    int nkeys = 0, nlater = 0, j;
    const zxm_info *info;
    zxm_rom_set roms;
    zxm *m;
    FILE *wfp = NULL;
    uint32_t wav_samples = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-l")) {
            for (j = 0; (info = zxm_machine(j)) != NULL; j++)
                printf("%-10s %s\n", info->id, info->name);
            return 0;
        } else if (!strcmp(a, "-m") && i + 1 < argc) id = argv[++i];
        else if (!strcmp(a, "-r") && i + 1 < argc) romdir = argv[++i];
        else if (!strcmp(a, "-f") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(a, "-t") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(a, "-d") && i + 1 < argc) delay = atoi(argv[++i]);
        else if (!strcmp(a, "-s") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(a, "-w") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(a, "-x")) reset_after = 1;
        else if (!strcmp(a, "-P")) regs = 1;
        else if (!strcmp(a, "-p") && i + 1 < argc && sscanf(argv[i + 1], "%x:%u", &dump_addr, &dump_len) == 2) i++;
        else if (!strcmp(a, "-R") && i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3'
                 && argv[i + 1][1] == '=') {
            i++;
            rom_override[argv[i][0] - '0'] = argv[i] + 2;
        } else if (!strcmp(a, "-i") && i + 1 < argc && nmedia < 16) media[nmedia++] = argv[++i];
        else if (!strcmp(a, "-k") && i + 1 < argc && nkeys < 32) {
            held_key *k = &keys[nkeys++];
            k->len = 10;
            if (sscanf(argv[++i], "%ld:%d:%d:%ld", &k->frame, &k->row, &k->bit, &k->len) < 3) {
                fprintf(stderr, "bad key %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(a, "-T") && i + 1 < argc && nlater < 16) {
            char *colon = strchr(argv[++i], ':');
            if (!colon) {
                fprintf(stderr, "bad -T %s (want frame:text)\n", argv[i]);
                return 2;
            }
            later_at[nlater] = atol(argv[i]);
            later[nlater++] = colon + 1;
        } else if (strchr(a, '=') && a[0] != '-' && nmedia < 16) media[nmedia++] = a;
        else {
            fprintf(stderr, "usage: %s -m machine [-r romdir] [-R n=file] [-f frames] [-t text [-d delay]]\n"
                            "       [-s out.ppm] [-w out.wav] [-x] [-p addr:len] [-P] [-k frame:row:bit[:frames]]...\n"
                            "       [-T frame:text]... [[-i] slot=file ...] | -l\n", argv[0]);
            return 2;
        }
    }
    if (!(info = zxm_lookup(id))) {
        fprintf(stderr, "unknown machine %s; machines:", id);
        for (i = 0; zxm_machine(i); i++)
            fprintf(stderr, " %s", zxm_machine(i)->id);
        fprintf(stderr, "\n");
        return 2;
    }
    memset(&roms, 0, sizeof roms);
    for (i = 0; i < 4 && info->roms[i]; i++) {
        char path[1024];
        uint8_t *data;
        uint32_t len = 0;
        if (rom_override[i])
            snprintf(path, sizeof path, "%s", rom_override[i]);
        else
            snprintf(path, sizeof path, "%s/%s", romdir, info->roms[i]);
        if (!path[0])
            continue;
        data = slurp(path, &len);
        if (!data) {
            if (info->rom_optional >> i & 1)
                continue;
            fprintf(stderr, "cannot read ROM %s\n", path);
            return 1;
        }
        roms.rom[i] = data;
        roms.len[i] = len;
    }
    m = malloc(zxm_size(info));
    if (!m || zxm_create(m, info, &roms)) {
        fprintf(stderr, "cannot create %s\n", info->id);
        return 1;
    }
    for (i = 0; i < nmedia; i++)
        if (insert(m, media[i]))
            return 1;
    if (reset_after)
        zxm_reset(m, ZXM_POWER_ON);
    if (text) {
        char *t = unescape(text);
        zxm_type(m, t, delay);
        free(t);
    }
    if (wav && (wfp = fopen(wav, "wb")) != NULL)
        wav_header(wfp, info->audio_rate, 0);
    for (i = 0; i < frames; i++) {
        for (j = 0; j < nlater; j++)
            if (later_at[j] == i) {
                char *t = unescape(later[j]);
                zxm_type(m, t, 0);
                free(t);
            }
        for (j = 0; j < nkeys; j++) {
            zxm_input_event ev;
            if (i != keys[j].frame && i != keys[j].frame + keys[j].len)
                continue;
            memset(&ev, 0, sizeof ev);
            ev.kind = ZXM_EV_KEY;
            ev.a = (uint8_t)keys[j].row;
            ev.b = (uint8_t)keys[j].bit;
            ev.down = i == keys[j].frame;
            zxm_input(m, &ev);
        }
        zxm_run_frame(m);
        if (wfp) {
            int n;
            const int16_t *au = zxm_audio(m, &n);
            fwrite(au, 4, (size_t)n, wfp);
            wav_samples += (uint32_t)n;
        }
    }
    if (wfp) {
        fseek(wfp, 0, SEEK_SET);
        wav_header(wfp, info->audio_rate, wav_samples);
        fclose(wfp);
    }
    if (shot)
        save_ppm(m, shot);
    if (dump_len) {
        const zxm_debug *dbg = zxm_debugger(m);
        unsigned k;
        for (k = 0; k < dump_len; k++)
            printf("%02x%s", dbg->peek(m, (uint16_t)(dump_addr + k)), (k & 15) == 15 || k + 1 == dump_len ? "\n" : " ");
    }
    if (regs) {
        zxm_regs r;
        zxm_debugger(m)->get_regs(m, &r);
        printf("pc %04x sp %04x af %04x bc %04x de %04x hl %04x ix %04x iy %04x i %02x im %d iff1 %d halted %d\n",
               r.pc, r.sp, r.af, r.bc, r.de, r.hl, r.ix, r.iy, r.i, r.im, r.iff1, r.halted);
    }
    zxm_destroy(m);
    free(m);
    return 0;
}
