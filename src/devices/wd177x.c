#include "wd177x.h"

#include <string.h>

/* Status bits */
#define ST_BUSY      0x01
#define ST_INDEX     0x02   /* type I */
#define ST_DRQ       0x02   /* type II, III */
#define ST_TRACK0    0x04   /* type I */
#define ST_LOST      0x04   /* type II, III */
#define ST_CRC       0x08
#define ST_RNF       0x10   /* seek error on type I */
#define ST_SPUN      0x20   /* type I */
#define ST_DELETED   0x20   /* read: record type; write: write fault */
#define ST_WPROT     0x40
#define ST_MOTOR     0x80

enum {
    P_IDLE, P_SPINUP, P_STEP, P_SETTLE, P_VERIFY, P_FIND, P_FOUND,
    P_READ, P_READ_CRC, P_WRITE_DRQ, P_WRITE_CHECK, P_WRITE, P_WRITE_CRC,
    P_TRACK_INDEX, P_TRACK_WRITE, P_NOT_FOUND
};

#define NEVER 0xffffffffffffffffull
#define INDEX_BYTES 100            /* the index pulse lasts about 3 ms */
#define LEAD 32                    /* gap before the first sector */
#define MAX_CYL 83

static const uint8_t step_ms[4] = { 6, 12, 2, 3 };

void wd_init(wd177x *w, uint32_t cpu_hz)
{
    memset(w, 0, sizeof *w);
    w->byte_t = cpu_hz / 31250;    /* 32 us */
    w->ms = cpu_hz / 1000;
    wd_reset(w);
}

void wd_reset(wd177x *w)
{
    w->cmd = 0;
    w->status = 0;
    w->type = 1;
    w->track = 0xff;
    w->sector = 1;
    w->data = 0;
    w->phase = P_IDLE;
    w->drq = w->intrq = 0;
    w->motor = w->spun = 0;
    w->when = NEVER;
}

int wd_active(const wd177x *w) { return w->motor; }

/* ---- the turning disk ---------------------------------------------------- */

static uint32_t rev_t(const wd177x *w) { return w->byte_t * WD_TRACK_BYTES; }

/* When the byte at track position p has been read, first time at or after t. */
static uint64_t byte_done(const wd177x *w, uint64_t t, uint32_t p)
{
    uint64_t kmin = (t + w->byte_t - 1) / w->byte_t, k;
    kmin = kmin ? kmin - 1 : 0;
    k = kmin + (p + WD_TRACK_BYTES - kmin % WD_TRACK_BYTES) % WD_TRACK_BYTES;
    return (k + 1) * w->byte_t;
}

static uint64_t next_index(const wd177x *w, const fd_disk *d, uint64_t t, int n)
{
    if (!d)
        return NEVER;
    return (t / rev_t(w) + (uint64_t)n) * rev_t(w);
}

static int index_now(const wd177x *w, const fd_disk *d, uint64_t t)
{
    return d && (t / w->byte_t) % WD_TRACK_BYTES < INDEX_BYTES;
}

/* Where sector i of n sits: its ID field's first byte, then its data. */
static uint32_t sector_base(int i, int n)
{
    return LEAD + (uint32_t)i * ((WD_TRACK_BYTES - LEAD) / (uint32_t)n);
}
#define ID_AT(i, n)   (sector_base(i, n) + 22 + 12 + 4)
#define DATA_AT(i, n) (ID_AT(i, n) + 6 + 22 + 12 + 4)

static uint16_t id_crc(const fd_sector *s)
{
    uint8_t b[8] = { 0xa1, 0xa1, 0xa1, 0xfe, s->c, s->h, s->r, s->n };
    uint16_t crc = fd_crc(b, 8, 0xffff);
    return (s->flags & FD_ID_CRC) ? (uint16_t)~crc : crc;
}

/* ---- command steps ------------------------------------------------------- */

static void finish(wd177x *w, uint64_t at)
{
    w->status &= (uint8_t)~ST_BUSY;
    w->phase = P_IDLE;
    w->when = NEVER;
    w->intrq = 1;
    w->motor_off = at + 9ull * rev_t(w);
}

/* After the spin-up (or straight away): the command's first real step. */
static void begin(wd177x *w, fd_disk *d, uint64_t now)
{
    uint8_t c = w->cmd;
    if (c < 0x80) {
        w->phase = P_STEP;
        w->when = now;
        if ((c & 0xf0) == 0x00)
            w->track = 0xff;           /* restore counts down from 255 */
        else if (c >= 0x20)
            w->found = 0;              /* a single step still to take */
        return;
    }
    if (c < 0xc0 || (c & 0xf0) == 0xc0) {
        /* Type II and read address: the E flag adds 30 ms of settling. */
        w->phase = P_SETTLE;
        w->when = now + ((c & 0x04) ? 30ull * w->ms : 0);
        return;
    }
    /* Read track, write track: from the next index pulse. */
    if ((c & 0xf0) == 0xf0) {
        if (d && d->readonly) {
            w->status |= ST_WPROT;
            finish(w, now);
            return;
        }
        w->drq = 1;
    }
    w->phase = P_TRACK_INDEX;
    w->when = next_index(w, d, now, 1);
}

/* Find the next ID field the command wants: its last byte's time, or the
 * search deadline. */
static void search(wd177x *w, fd_disk *d, uint64_t now)
{
    const fd_track *t = fd_track_at(d, w->cyl, w->side);
    uint64_t best = NEVER;
    int i, n, verify = w->cmd < 0x80, any = (w->cmd & 0xf0) == 0xc0;
    if (t && (n = t->count) > 0) {
        for (i = 0; i < n; i++) {
            const fd_sector *s = &t->s[i];
            uint64_t at;
            if (s->flags & FD_ID_CRC)
                continue;
            if (!any && (s->c != w->track || (!verify && s->r != w->sector)))
                continue;
            /* read address takes the ID from its first byte */
            at = byte_done(w, now, any ? ID_AT(i, n) : ID_AT(i, n) + 5);
            if (at < best) {
                best = at;
                w->found = (uint8_t)i;
            }
        }
    }
    if (best != NEVER && best <= w->deadline) {
        w->phase = P_FOUND;
        w->when = best;
    } else {
        w->phase = P_NOT_FOUND;
        w->when = w->deadline;
    }
}

static void start_search(wd177x *w, fd_disk *d, uint64_t now)
{
    w->deadline = next_index(w, d, now, 5);
    search(w, d, now);
}

static void build_track(wd177x *w, fd_disk *d)
{
    const fd_track *t = fd_track_at(d, w->cyl, w->side);
    uint8_t *b = w->buf, sec[1024];
    int i, n;
    memset(b, 0x4e, WD_TRACK_BYTES);
    if (!t || !(n = t->count))
        return;
    for (i = 0; i < n; i++) {
        const fd_sector *s = &t->s[i];
        uint32_t id = ID_AT(i, n), da = DATA_AT(i, n), len = 128u << (s->n & 3), k;
        uint16_t crc = id_crc(s);
        memset(b + id - 16, 0x00, 12);
        memset(b + id - 4, 0xa1, 3);
        b[id - 1] = 0xfe;
        b[id] = s->c; b[id + 1] = s->h; b[id + 2] = s->r; b[id + 3] = s->n;
        b[id + 4] = (uint8_t)(crc >> 8);
        b[id + 5] = (uint8_t)crc;
        if (s->flags & FD_NO_DATA)
            continue;
        memset(b + da - 16, 0x00, 12);
        memset(b + da - 4, 0xa1, 3);
        b[da - 1] = (s->flags & FD_DELETED) ? 0xf8 : 0xfb;
        fd_read(d, w->cyl, w->side, i, sec);
        for (k = 0; k < len && da + k < WD_TRACK_BYTES; k++)
            b[da + k] = sec[k];
        crc = fd_crc(b + da - 4, (int)(len + 4), 0xffff);
        if (s->flags & FD_DATA_CRC)
            crc = (uint16_t)~crc;
        if (da + len + 1 < WD_TRACK_BYTES) {
            b[da + len] = (uint8_t)(crc >> 8);
            b[da + len + 1] = (uint8_t)crc;
        }
    }
}

/* Read back what a Write Track laid down and reformat the track. */
static int write_track(wd177x *w, fd_disk *d)
{
    fd_sector ids[FD_MAX_SECTORS];
    const uint8_t *data[FD_MAX_SECTORS];
    const uint8_t *p = w->buf, *end = w->buf + WD_TRACK_BYTES;
    int n = 0;
    while (p < end && n < FD_MAX_SECTORS) {
        /* F5 writes an A1 sync mark; F5 F5 F5 FE is an ID, F5 F5 F5 FB/F8 data. */
        if (p + 8 < end && p[0] == 0xf5 && p[1] == 0xf5 && p[2] == 0xf5 && p[3] == 0xfe) {
            memset(&ids[n], 0, sizeof ids[n]);
            ids[n].c = p[4]; ids[n].h = p[5]; ids[n].r = p[6]; ids[n].n = p[7];
            data[n] = NULL;
            p += 8;
            continue;
        }
        if (p + 4 < end && p[0] == 0xf5 && p[1] == 0xf5 && p[2] == 0xf5 && (p[3] == 0xfb || p[3] == 0xf8)) {
            if (n < FD_MAX_SECTORS && p + 4 + (128 << (ids[n].n & 3)) <= end) {
                data[n] = p + 4;
                p += 4 + (128 << (ids[n].n & 3));
                n++;
                continue;
            }
        }
        p++;
    }
    return fd_format(d, w->cyl, w->side, ids, n, data);
}

static void step(wd177x *w, fd_disk *d)
{
    uint64_t now = w->when;
    uint8_t c = w->cmd;
    switch (w->phase) {
    case P_SPINUP:
        w->spun = 1;
        begin(w, d, now);
        return;

    case P_STEP: {
        uint64_t rate = (uint64_t)step_ms[c & 3] * w->ms;
        int done = 0;
        switch (c & 0xe0) {
        case 0x00:                     /* restore, seek */
            if ((c & 0x10) == 0) {
                if (w->cyl == 0) {
                    w->track = 0;
                    done = 1;
                } else if (w->track == 0) {
                    w->status |= ST_RNF; /* 255 steps without track 0 */
                    finish(w, now);
                    return;
                } else {
                    w->step_in = 0;
                }
            } else if (w->track == w->data) {
                done = 1;
            } else {
                w->step_in = w->data > w->track;
            }
            if (!done)
                w->track = (uint8_t)(w->track + (w->step_in ? 1 : -1));
            break;
        default:                       /* step, step in, step out */
            if (w->found) {
                done = 1;
                break;
            }
            if (c & 0x40)
                w->step_in = !(c & 0x20);
            if (c & 0x10)
                w->track = (uint8_t)(w->track + (w->step_in ? 1 : -1));
            w->found = 1;
            break;
        }
        if (done) {
            if (c & 0x04) {            /* verify after 15 ms settling */
                w->phase = P_VERIFY;
                w->when = now + 15ull * w->ms;
            } else {
                finish(w, now);
            }
            return;
        }
        if (w->step_in) {
            if (w->cyl < MAX_CYL)
                w->cyl++;
        } else if (w->cyl > 0) {
            w->cyl--;
        }
        w->when = now + rate;
        return;
    }

    case P_VERIFY:
        start_search(w, d, now);
        return;

    case P_SETTLE:
        if ((c & 0xe0) == 0xa0 && d && d->readonly) {
            w->status |= ST_WPROT;
            finish(w, now);
            return;
        }
        start_search(w, d, now);
        return;

    case P_NOT_FOUND:
        w->status |= ST_RNF;
        finish(w, now);
        return;

    case P_FOUND: {
        const fd_track *t = fd_track_at(d, w->cyl, w->side);
        const fd_sector *s;
        int n;
        if (!t || w->found >= t->count) {
            search(w, d, now);
            return;
        }
        s = &t->s[w->found];
        n = t->count;
        if (c < 0x80) {                /* verify found its track */
            finish(w, now);
            return;
        }
        if ((c & 0xf0) == 0xc0) {      /* read address */
            uint16_t crc = id_crc(s);
            w->buf[0] = s->c; w->buf[1] = s->h; w->buf[2] = s->r; w->buf[3] = s->n;
            w->buf[4] = (uint8_t)(crc >> 8);
            w->buf[5] = (uint8_t)crc;
            w->sector = s->c;
            w->pos = 0;
            w->len = 6;
            w->phase = P_READ;
            return;                    /* the first byte is ready now */
        }
        if (s->flags & FD_NO_DATA) {
            search(w, d, now + w->byte_t);
            return;
        }
        w->len = (uint16_t)(128u << (s->n & 3));
        w->pos = 0;
        if (c < 0xa0) {                /* read sector */
            fd_read(d, w->cyl, w->side, w->found, w->buf);
            w->phase = P_READ;
            w->when = byte_done(w, now, DATA_AT(w->found, n));
        } else {                       /* write sector: DRQ two bytes on */
            w->phase = P_WRITE_DRQ;
            w->when = now + 2ull * w->byte_t;
        }
        return;
    }

    case P_READ:
        if (w->drq)
            w->status |= ST_LOST;
        w->data = w->buf[w->pos++];
        w->drq = 1;
        if (w->pos < w->len) {
            w->when = now + w->byte_t;
        } else if ((c & 0xf0) == 0xc0 || (c & 0xf0) == 0xe0) {
            w->phase = P_IDLE;
            finish(w, now + w->byte_t);
        } else {
            w->phase = P_READ_CRC;
            w->when = now + 2ull * w->byte_t;
        }
        return;

    case P_READ_CRC: {
        const fd_track *t = fd_track_at(d, w->cyl, w->side);
        if (t && w->found < t->count) {
            if (t->s[w->found].flags & FD_DATA_CRC)
                w->status |= ST_CRC;
            if (t->s[w->found].flags & FD_DELETED)
                w->status |= ST_DELETED;
        }
        if ((c & 0x10) && !(w->status & (ST_CRC | ST_LOST))) {
            w->sector++;               /* multiple sectors: on to the next */
            start_search(w, d, now);
            return;
        }
        finish(w, now);
        return;
    }

    case P_WRITE_DRQ:
        w->drq = 1;
        w->phase = P_WRITE_CHECK;
        w->when = now + 9ull * w->byte_t;
        return;

    case P_WRITE_CHECK: {
        const fd_track *t = fd_track_at(d, w->cyl, w->side);
        if (w->drq) {                  /* no data in time: nothing written */
            w->status |= ST_LOST;
            w->drq = 0;
            finish(w, now);
            return;
        }
        w->buf[w->pos++] = w->data;
        w->drq = 1;
        w->phase = P_WRITE;
        w->when = t ? byte_done(w, now, DATA_AT(w->found, t->count) + 1) : now + w->byte_t;
        return;
    }

    case P_WRITE:
        if (w->drq) {
            w->status |= ST_LOST;
            w->data = 0;
        }
        w->buf[w->pos++] = w->data;
        if (w->pos < w->len) {
            w->drq = 1;
            w->when = now + w->byte_t;
        } else {
            w->drq = 0;
            w->phase = P_WRITE_CRC;
            w->when = now + 2ull * w->byte_t;
        }
        return;

    case P_WRITE_CRC:
        if (fd_write(d, w->cyl, w->side, w->found, w->buf))
            w->status |= ST_DELETED;   /* write fault */
        if ((c & 0x10) && !(w->status & (ST_LOST | ST_DELETED))) {
            w->sector++;
            start_search(w, d, now);
            return;
        }
        finish(w, now);
        return;

    case P_TRACK_INDEX:
        if ((c & 0xf0) == 0xe0) {      /* read track */
            build_track(w, d);
            w->pos = 0;
            w->len = WD_TRACK_BYTES;
            w->phase = P_READ;
            w->when = now + w->byte_t;
            return;
        }
        if (w->drq) {                  /* write track: no first byte */
            w->status |= ST_LOST;
            w->drq = 0;
            finish(w, now);
            return;
        }
        w->pos = 0;
        w->phase = P_TRACK_WRITE;
        w->when = now;
        return;

    case P_TRACK_WRITE:
        if (w->drq) {
            w->status |= ST_LOST;
            w->data = 0;
        }
        w->buf[w->pos++] = w->data;
        if (w->pos < WD_TRACK_BYTES) {
            w->drq = 1;
            w->when = now + w->byte_t;
        } else {
            w->drq = 0;
            if (write_track(w, d))
                w->status |= ST_DELETED;
            finish(w, now);
        }
        return;

    default:
        w->when = NEVER;
        return;
    }
}

void wd_run(wd177x *w, fd_disk *d, uint64_t now)
{
    while (w->phase != P_IDLE && w->when <= now)
        step(w, d);
    if (w->phase == P_IDLE && w->motor && now >= w->motor_off)
        w->motor = w->spun = 0;
}

static void command(wd177x *w, fd_disk *d, uint8_t v, uint64_t now)
{
    if ((v & 0xf0) == 0xd0) {          /* force interrupt */
        if (w->phase != P_IDLE) {
            w->status &= (uint8_t)~ST_BUSY;
            w->phase = P_IDLE;
            w->when = NEVER;
            w->motor_off = now + 9ull * rev_t(w);
        } else {
            w->type = 1;
        }
        w->drq = 0;
        w->intrq = (v & 0x08) != 0;
        w->cmd = v;
        return;
    }
    if (w->status & ST_BUSY)
        return;
    w->cmd = v;
    w->type = v < 0x80 ? 1 : (v < 0xc0 ? 2 : 3);
    w->status = ST_BUSY;
    w->drq = w->intrq = 0;
    if (!w->motor) {
        w->motor = 1;
        if (!(v & 0x08)) {             /* spin up over six index pulses */
            w->spun = 0;
            w->phase = P_SPINUP;
            w->when = next_index(w, d, now, 6);
            return;
        }
    }
    begin(w, d, now);
}

uint8_t wd_read(wd177x *w, fd_disk *d, int reg, uint64_t now)
{
    uint8_t s;
    wd_run(w, d, now);
    switch (reg & 3) {
    case 0:
        w->intrq = 0;
        s = (uint8_t)((w->status & ~(ST_MOTOR | ST_DRQ)) | (w->motor ? ST_MOTOR : 0));
        if (w->type == 1) {
            s &= (uint8_t)~(ST_INDEX | ST_TRACK0 | ST_SPUN | ST_WPROT);
            if (index_now(w, d, now))
                s |= ST_INDEX;
            if (w->cyl == 0)
                s |= ST_TRACK0;
            if (w->spun)
                s |= ST_SPUN;
            if (d && d->readonly)
                s |= ST_WPROT;
        } else if (w->drq) {
            s |= ST_DRQ;
        }
        return s;
    case 1:
        return w->track;
    case 2:
        return w->sector;
    default:
        w->drq = 0;
        return w->data;
    }
}

void wd_write(wd177x *w, fd_disk *d, int reg, uint8_t v, uint64_t now)
{
    wd_run(w, d, now);
    switch (reg & 3) {
    case 0:
        command(w, d, v, now);
        break;
    case 1:
        if (!(w->status & ST_BUSY))
            w->track = v;
        break;
    case 2:
        if (!(w->status & ST_BUSY))
            w->sector = v;
        break;
    default:
        w->data = v;
        w->drq = 0;
        break;
    }
}
