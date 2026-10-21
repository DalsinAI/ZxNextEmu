/*
 * mkmgt: build a SAMDOS / MasterDOS disk image (MGT, 819200 bytes) with
 * CODE files on it, the first of which starts at track 4 sector 1 where
 * the SAM ROM's BOOT looks for a DOS.
 *
 * usage: mkmgt out.mgt file [file...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECT 512
#define IMAGE (80 * 2 * 10 * SECT)

static unsigned char disk[IMAGE];

static unsigned char *sector(int track, int s)
{
    int cyl = track & 0x7f, side = track >> 7;
    return disk + ((cyl * 2 + side) * 10 + (s - 1)) * SECT;
}

int main(int argc, char **argv)
{
    int track = 4, sec = 1, bit = 0, f;
    FILE *fp;
    if (argc < 3) {
        fprintf(stderr, "usage: %s out.mgt file [file...]\n", argv[0]);
        return 2;
    }
    for (f = 2; f < argc && f - 2 < 80; f++) {
        static unsigned char body[0x80000];
        unsigned char *dir = sector((f - 2) / 20, ((f - 2) % 20) / 2 + 1) + ((f - 2) & 1) * 256;
        const char *name = strrchr(argv[f], '/') ? strrchr(argv[f], '/') + 1 : argv[f];
        size_t len, total, pos = 0;
        int n, i;
        if (!(fp = fopen(argv[f], "rb"))) {
            perror(argv[f]);
            return 1;
        }
        len = fread(body + 9, 1, sizeof body - 9, fp);
        fclose(fp);
        /* The file header: CODE, length mod 16K, offset 0x8000, pages, page 1. */
        body[0] = 19;
        body[1] = (unsigned char)len;
        body[2] = (unsigned char)((len >> 8) & 0x3f);
        body[3] = 0x00;
        body[4] = 0x80;
        body[5] = body[6] = 0xff;
        body[7] = (unsigned char)(len >> 14);
        body[8] = 1;
        total = len + 9;
        n = (int)((total + SECT - 3) / (SECT - 2));

        dir[0] = 19;
        memset(dir + 1, ' ', 10);
        for (i = 0; i < 10 && name[i] && name[i] != '.'; i++)
            dir[1 + i] = (unsigned char)name[i];
        dir[11] = (unsigned char)(n >> 8);
        dir[12] = (unsigned char)n;
        dir[13] = (unsigned char)track;
        dir[14] = (unsigned char)sec;
        dir[236] = body[8];
        dir[237] = body[3];
        dir[238] = body[4];
        dir[239] = body[7];
        dir[240] = body[1];
        dir[241] = body[2];
        dir[242] = dir[243] = dir[244] = 0xff;   /* no auto-run */

        for (i = 0; i < n; i++) {
            unsigned char *p = sector(track, sec);
            size_t k = total - pos < SECT - 2 ? total - pos : SECT - 2;
            memcpy(p, body + pos, k);
            pos += k;
            dir[15 + bit / 8] |= (unsigned char)(1 << (bit % 8));
            bit++;
            if (++sec > 10) {
                sec = 1;
                track++;
                if ((track & 0x7f) == 80)
                    track = 0x80;      /* on to side 2 */
            }
            if (i + 1 < n) {
                p[SECT - 2] = (unsigned char)track;
                p[SECT - 1] = (unsigned char)sec;
            }
        }
    }
    if (!(fp = fopen(argv[1], "wb")) || fwrite(disk, 1, IMAGE, fp) != IMAGE) {
        perror(argv[1]);
        return 1;
    }
    fclose(fp);
    return 0;
}
