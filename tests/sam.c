/*
 * SAM Coupé check, through zxm like a ZX Cradle host:
 *
 *  1. ROM 3.0 boots and BASIC runs typed commands.
 *  2. BOOT loads SAMDOS from drive 1 through the WD1772, in real time.
 *  3. BASIC pokes 0..255 four times over and SAVE "hello" writes those 1024
 *     bytes to the disk image (a block device on the image file); the test
 *     reads them back from the file through the directory and sector chain.
 *  4. SOUND sets an SAA1099 tone (octave 4, offset 100: 15625*16/411 Hz)
 *     and the sound output has that pitch.
 *
 * usage: sam romdir disk.mgt   (the disk is changed: give it a copy)
 */
#include "../src/core/zxm.h"
#include "../src/storage/blockdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rom[0x8000];

static void frames(zxm *m, int n) { while (n--) zxm_run_frame(m); }

static void type(zxm *m, const char *text, int settle)
{
    zxm_type(m, text, 0);
    while (zxm_typing(m))
        zxm_run_frame(m);
    frames(m, settle);
}

static int fail(const char *what)
{
    printf("sam: FAIL: %s\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    const zxm_info *info = zxm_lookup("sam");
    zxm_rom_set roms;
    zxm_media disk;
    zxm *m;
    FILE *fp;
    char path[1024];
    static uint8_t img[819200], file[0x10000];
    int i, e, n, crossings = 0, prev = 0, samples;
    long count = 0;
    double hz;

    if (argc < 3 || !info) {
        fprintf(stderr, "usage: %s romdir disk.mgt\n", argv[0]);
        return 2;
    }
    snprintf(path, sizeof path, "%s/samcoupe.rom", argv[1]);
    if (!(fp = fopen(path, "rb")) || fread(rom, 1, sizeof rom, fp) != sizeof rom)
        return fail("cannot read samcoupe.rom");
    fclose(fp);
    memset(&roms, 0, sizeof roms);
    roms.rom[0] = rom;
    roms.len[0] = sizeof rom;
    m = malloc(zxm_size(info));
    if (!m || zxm_create(m, info, &roms))
        return fail("create");

    memset(&disk, 0, sizeof disk);
    disk.kind = ZXM_MEDIA_BLOCKS;
    disk.name = argv[2];
    if (!(disk.blocks = zx_bd_open_image(argv[2])) || zxm_media_insert(m, "disk.1", &disk))
        return fail("insert the disk");

    frames(m, 150);                    /* the start-up screen */
    type(m, "\n\n", 50);
    type(m, "boot\n", 400);            /* SAMDOS loads; no AUTO file */
    type(m, "for i=0 to 1023: poke 40000+i,i-256*int(i/256): next i: save \"hello\" code 40000,1024\n", 1000);

    /* The saved file, from the image: SAMDOS writes the directory on
     * tracks 0..3 and chains the file's sectors from track 4. */
    zxm_media_eject(m, "disk.1");
    if (!(fp = fopen(argv[2], "rb")) || fread(img, 1, sizeof img, fp) != sizeof img)
        return fail("read the image back");
    fclose(fp);
    for (e = 0; e < 80; e++) {
        const uint8_t *d = img + ((e / 20) * 2 * 10 + (e % 20) / 2) * 512 + (e & 1) * 256;
        if (d[0] == 19 && !memcmp(d + 1, "hello     ", 10)) {
            int track = d[13], sec = d[14];
            n = 0;
            for (i = 0; i < (d[11] << 8 | d[12]) && track; i++) {
                const uint8_t *p = img + (((track & 0x7f) * 2 + (track >> 7)) * 10 + sec - 1) * 512;
                memcpy(file + n, p, 510);
                n += 510;
                track = p[510];
                sec = p[511];
            }
            if (n < 1024 + 9)
                return fail("the file is short");
            if (file[0] != 19 || (file[1] | file[2] << 8) != 1024)
                return fail("the file header");
            for (i = 0; i < 1024 && file[9 + i] == (uint8_t)(i & 255); i++)
                ;
            if (i < 1024) {
                printf("sam: byte %d is %02x\n", i, file[9 + i]);
                return fail("the saved bytes are not what BASIC poked");
            }
            break;
        }
    }
    if (e == 80)
        return fail("no \"hello\" on the disk after SAVE");
    printf("sam: BOOT loaded SAMDOS, SAVE wrote 1024 bytes that read back exactly\n");

    type(m, "sound 28,1;20,1;0,255;8,100;16,4\n", 20);
    for (i = 0; i < 50; i++) {
        const int16_t *au;
        int k;
        zxm_run_frame(m);
        au = zxm_audio(m, &samples);
        for (k = 0; k < samples; k++) {
            int v = au[k * 2] > 1000;
            crossings += v && !prev;
            prev = v;
            count++;
        }
    }
    hz = crossings * (double)info->audio_rate / (double)count;
    if (hz < 600 || hz > 617)
        return fail("SAA1099 tone pitch");
    printf("sam: SAA1099 tone at %.0f Hz (608 expected)\n", hz);
    zxm_destroy(m);
    free(m);
    return 0;
}
