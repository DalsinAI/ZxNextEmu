#include "floppy.h"
#include "../storage/blockdev.h"

#include <stdlib.h>
#include <string.h>

#define MGT_CYLS 80

uint16_t fd_crc(const uint8_t *p, int n, uint16_t crc)
{
    int i;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (i = 0; i < 8; i++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static void regular(fd_disk *d, int cyls, int heads, int spt, int n, uint32_t (*where)(const fd_disk *, int, int, int))
{
    int c, h, i;
    d->cyls = cyls;
    d->heads = heads;
    for (c = 0; c < cyls && c < FD_MAX_CYLS; c++) {
        for (h = 0; h < heads; h++) {
            fd_track *t = &d->track[c][h];
            t->count = (uint8_t)spt;
            for (i = 0; i < spt; i++) {
                fd_sector *s = &t->s[i];
                s->c = (uint8_t)c;
                s->h = (uint8_t)h;
                s->r = (uint8_t)(i + 1);
                s->n = (uint8_t)n;
                s->len = (uint16_t)(128 << n);
                s->off = where(d, c, h, i);
            }
        }
    }
}

static uint32_t mgt_where(const fd_disk *d, int c, int h, int i)
{
    int spt = d->track[0][0].count;
    return ((uint32_t)(c * 2 + h) * (uint32_t)spt + (uint32_t)i) * 512u;
}

static uint32_t sad_where(const fd_disk *d, int c, int h, int i)
{
    const fd_sector *s0 = &d->track[0][0].s[0];
    int spt = d->track[0][0].count;
    return 22u + ((uint32_t)(h * d->cyls + c) * (uint32_t)spt + (uint32_t)i) * s0->len;
}

static int size_code(unsigned len)
{
    int n = 0;
    while ((128u << n) < len && n < 7)
        n++;
    return n;
}

static int mgt_layout(fd_disk *d, uint32_t len)
{
    int spt;
    if (len == 2u * MGT_CYLS * 10 * 512)
        spt = 10;
    else if (len == 2u * MGT_CYLS * 9 * 512)
        spt = 9;
    else
        return -1;
    d->format = FD_MGT;
    d->track[0][0].count = (uint8_t)spt;   /* mgt_where reads it */
    regular(d, MGT_CYLS, 2, spt, 2, mgt_where);
    return 0;
}

static int sad_layout(fd_disk *d)
{
    const uint8_t *h = d->data;
    int heads = h[18], cyls = h[19], spt = h[20];
    uint32_t size = (uint32_t)h[21] << 6;
    if (!heads || heads > 2 || !cyls || cyls > FD_MAX_CYLS || !spt || spt > FD_MAX_SECTORS
        || size < 128 || size > 1024
        || 22u + (uint32_t)heads * cyls * spt * size > d->len)
        return -1;
    d->format = FD_SAD;
    d->cyls = cyls;
    d->track[0][0].count = (uint8_t)spt;
    d->track[0][0].s[0].len = (uint16_t)size;
    regular(d, cyls, heads, spt, size_code(size), sad_where);
    return 0;
}

static int edsk_layout(fd_disk *d)
{
    const uint8_t *p = d->data;
    int ext = !memcmp(p, "EXTENDED", 8), cyls = p[0x30], heads = p[0x31], c, h, i;
    uint32_t pos = 0x100, std_size = (uint32_t)(p[0x32] | p[0x33] << 8);
    if (!heads || heads > 2)
        return -1;
    d->format = FD_EDSK;
    d->cyls = cyls < FD_MAX_CYLS ? cyls : FD_MAX_CYLS;
    d->heads = heads;
    for (c = 0; c < cyls; c++) {
        for (h = 0; h < heads; h++) {
            uint32_t tsize = ext ? (uint32_t)p[0x34 + c * heads + h] << 8 : std_size, data;
            const uint8_t *ti;
            fd_track *t;
            int count;
            if (!tsize)
                continue;
            if (pos + 0x100 > d->len || memcmp(p + pos, "Track-Info", 10))
                return c ? 0 : -1;       /* keep what was readable */
            ti = p + pos;
            count = ti[0x15] < FD_MAX_SECTORS ? ti[0x15] : FD_MAX_SECTORS;
            data = pos + 0x100;
            if (c < FD_MAX_CYLS) {
                t = &d->track[c][h];
                t->count = (uint8_t)count;
                for (i = 0; i < count; i++) {
                    const uint8_t *si = ti + 0x18 + i * 8;
                    fd_sector *s = &t->s[i];
                    uint32_t len = ext ? (uint32_t)(si[6] | si[7] << 8) : (128u << (ti[0x14] & 7));
                    s->c = si[0];
                    s->h = si[1];
                    s->r = si[2];
                    s->n = si[3];
                    s->flags = 0;
                    if ((si[4] & 0x20) && !(si[5] & 0x20))
                        s->flags |= FD_ID_CRC;
                    if (si[5] & 0x20)
                        s->flags |= FD_DATA_CRC;
                    if (si[5] & 0x40)
                        s->flags |= FD_DELETED;
                    if ((si[4] & 0x01) || (si[5] & 0x01) || !len)
                        s->flags |= FD_NO_DATA;
                    s->off = data;
                    s->len = (uint16_t)(len > 0xffff ? 0xffff : len);
                    if (data + len > d->len)
                        s->flags |= FD_NO_DATA;
                    data += len;
                }
            }
            pos += tsize;
        }
    }
    return 0;
}

fd_disk *fd_open(const uint8_t *data, uint32_t len, const char *name)
{
    (void)name;
    fd_disk *d = calloc(1, sizeof *d);
    int ok = -1;
    if (!d || !(d->data = malloc(len ? len : 1))) {
        free(d);
        return NULL;
    }
    memcpy(d->data, data, len);
    d->len = len;
    if (len >= 0x100 && (!memcmp(data, "EXTENDED CPC DSK", 16) || !memcmp(data, "MV - CPC", 8)))
        ok = edsk_layout(d);
    else if (len >= 22 && !memcmp(data, "Aley's disk backup", 18))
        ok = sad_layout(d);
    else
        ok = mgt_layout(d, len);     /* .mgt, raw .dsk / .img: known by size */
    if (ok) {
        fd_close(d);
        return NULL;
    }
    return d;
}

fd_disk *fd_open_blocks(struct zx_blockdev *bd)
{
    fd_disk *d;
    if (!bd || (bd->blocks != 1600 && bd->blocks != 1440))
        return NULL;
    if (!(d = calloc(1, sizeof *d)))
        return NULL;
    d->bd = bd;
    d->readonly = bd->readonly;
    mgt_layout(d, (uint32_t)bd->blocks * 512u);
    return d;
}

void fd_close(fd_disk *d)
{
    if (!d)
        return;
    zx_bd_close(d->bd);
    free(d->data);
    free(d);
}

const fd_track *fd_track_at(const fd_disk *d, int cyl, int head)
{
    if (!d || cyl < 0 || cyl >= d->cyls || cyl >= FD_MAX_CYLS || head < 0 || head >= d->heads)
        return NULL;
    return &d->track[cyl][head];
}

int fd_read(fd_disk *d, int cyl, int head, int idx, uint8_t *buf)
{
    const fd_track *t = fd_track_at(d, cyl, head);
    const fd_sector *s;
    uint32_t want, n;
    if (!t || idx < 0 || idx >= t->count)
        return -1;
    s = &t->s[idx];
    want = 128u << (s->n & 3);
    memset(buf, 0, want);
    if (s->flags & FD_NO_DATA)
        return -1;
    n = s->len < want ? s->len : want;
    if (d->bd)
        return d->bd->read(d->bd, s->off / 512u, buf);
    memcpy(buf, d->data + s->off, n);
    return 0;
}

int fd_write(fd_disk *d, int cyl, int head, int idx, const uint8_t *buf)
{
    const fd_track *t = fd_track_at(d, cyl, head);
    const fd_sector *s;
    uint32_t n;
    if (!t || d->readonly || idx < 0 || idx >= t->count)
        return -1;
    s = &t->s[idx];
    if (s->flags & FD_NO_DATA)
        return -1;
    n = 128u << (s->n & 3);
    if (n > s->len)
        n = s->len;
    if (d->bd)
        return d->bd->write(d->bd, s->off / 512u, buf);
    memcpy(d->data + s->off, buf, n);
    return 0;
}

int fd_format(fd_disk *d, int cyl, int head, const fd_sector *ids, int n, const uint8_t *const data[])
{
    fd_track *t = (fd_track *)fd_track_at(d, cyl, head);
    int i, j;
    if (!t || d->readonly || n != t->count)
        return -1;
    for (i = 0; i < n; i++)
        if ((128u << (ids[i].n & 3)) != t->s[i].len)
            return -1;
    if (d->format == FD_EDSK) {
        /* Extended images keep the new IDs in the new order. */
        for (i = 0; i < n; i++) {
            t->s[i].c = ids[i].c;
            t->s[i].h = ids[i].h;
            t->s[i].r = ids[i].r;
            t->s[i].n = ids[i].n;
            t->s[i].flags = 0;
            if (fd_write(d, cyl, head, i, data[i]))
                return -1;
        }
        return 0;
    }
    /* MGT and SAD store sectors 1..n by number: the data goes to its
     * number's place, whatever the interleave. */
    for (i = 0; i < n; i++)
        if (ids[i].r < 1 || ids[i].r > n || ids[i].c != cyl)
            return -1;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++)
            if (t->s[j].r == ids[i].r && fd_write(d, cyl, head, j, data[i]))
                return -1;
    return 0;
}
