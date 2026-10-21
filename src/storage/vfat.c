/*
 * A host folder shown to the machine as a FAT32 volume.
 *
 * Nothing is copied when the folder is opened. The folder is scanned once
 * and laid out as a FAT32 volume with long file names: every file gets a
 * run of clusters, and every directory gets its entries generated in
 * memory. Reads of a file's clusters go straight to the host file, so the
 * machine always sees what is on the host.
 *
 * Writes are copy on write: a written cluster is held in memory. Once the
 * machine has stopped writing for a moment (SYNC_IDLE frames), and when the
 * device is closed, the volume is read back the way the machine left it:
 * files whose clusters, size or name changed are written to the folder,
 * files the machine deleted are deleted, and directories are created and
 * removed to match. Files that did not change are not touched.
 *
 * Host names that start with a dot are hidden from the machine and never
 * touched. The disk has an MBR with one FAT32 partition at sector 2048,
 * like a card formatted by a PC, which is what the Next boot ROM and
 * NextZXOS expect.
 *
 * For systems that do not read FAT32 (Nextor on the MSX) the volume can
 * be FAT16 instead, up to 2 GB. The layout is the same except that the
 * root directory is the fixed area after the FATs rather than a cluster
 * chain; a cluster number of 0 stands for it, as it does in ".." entries.
 */
#define _POSIX_C_SOURCE 200809L
#include "blockdev.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PART_LBA   2048u
#define RESERVED   32u
#define END_CHAIN  0x0FFFFFFFu
#define FAT_MASK   0x0FFFFFFFu
#define SYNC_IDLE  50           /* frames without writes before writing back */
#define MAX_DEPTH  32
#define MAX_LFN    255

typedef struct vnode {
    char *path;                 /* relative to the root, '/' separated */
    uint64_t size;
    uint8_t seen;               /* sync: unchanged in the volume */
    uint8_t doomed;             /* sync: host file goes away or is replaced */
    uint8_t gone;               /* sync: no longer in the volume */
} vnode;

typedef struct gfile {          /* an entry found in the volume at sync */
    char *path;
    uint32_t first;
    uint64_t size;
    uint8_t dir;
    uint8_t changed;
    int node;                   /* matching vnode, or -1 */
} gfile;

typedef struct vfat {
    zx_blockdev bd;
    char *root;
    uint32_t spc;               /* sectors per cluster */
    uint32_t csize;             /* bytes per cluster */
    uint32_t nclus;             /* data clusters, numbered 2 .. nclus + 1 */
    uint32_t fatsz;             /* sectors per FAT */
    uint32_t data_start;        /* first data sector, partition relative */
    uint64_t part_sectors;
    uint32_t *fat;
    uint8_t **buf;              /* cluster held in memory, or NULL */
    uint8_t *dirty;             /* buffer differs from the host file */
    uint32_t *own;              /* backing vnode + 1, or 0 */
    uint32_t *idx;              /* cluster number within that file */
    vnode *node;
    uint32_t nnode, cap_node;
    char **dirs;                /* host directories in the volume */
    uint32_t ndirs, cap_dirs;
    uint8_t mbr[ZX_BLOCK];
    uint8_t rsv[RESERVED * ZX_BLOCK];
    FILE *fp;                   /* open host file */
    uint32_t fp_node;           /* its vnode + 1 */
    uint32_t next_clus;         /* layout cursor */
    int pending, idle, full;
    int fat16;
    uint32_t root_sectors;      /* FAT16: the fixed root directory */
    uint8_t *rootdir;
    const char *label;          /* 11 characters */
} vfat;

/* ---- little helpers ------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static char *join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *p = malloc(la + lb + 2);
    if (!p)
        return NULL;
    memcpy(p, a, la);
    if (la && lb)
        p[la++] = '/';
    memcpy(p + la, b, lb + 1);
    return p;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int ci_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
        int y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static void push_dir(vfat *v, char *rel)
{
    if (v->ndirs == v->cap_dirs) {
        uint32_t cap = v->cap_dirs ? v->cap_dirs * 2 : 64;
        char **d = realloc(v->dirs, cap * sizeof *d);
        if (!d) {
            free(rel);
            return;
        }
        v->dirs = d;
        v->cap_dirs = cap;
    }
    v->dirs[v->ndirs++] = rel;
}

static int add_node(vfat *v, const char *rel, uint64_t size)
{
    vnode *n;
    if (v->nnode == v->cap_node) {
        uint32_t cap = v->cap_node ? v->cap_node * 2 : 256;
        vnode *p = realloc(v->node, cap * sizeof *p);
        if (!p)
            return -1;
        v->node = p;
        v->cap_node = cap;
    }
    n = &v->node[v->nnode];
    memset(n, 0, sizeof *n);
    n->path = xstrdup(rel);
    n->size = size;
    return (int)v->nnode++;
}

/* ---- host files ------------------------------------------------------ */

static void host_close(vfat *v)
{
    if (v->fp)
        fclose(v->fp);
    v->fp = NULL;
    v->fp_node = 0;
}

/* Read part of a vnode's host file; past the end reads as zero. */
static void host_read(vfat *v, uint32_t node, uint64_t off, uint8_t *buf, uint32_t n)
{
    size_t got = 0;
    if (v->fp_node != node + 1) {
        char *abs;
        host_close(v);
        abs = v->node[node].path ? join(v->root, v->node[node].path) : NULL;
        v->fp = abs ? fopen(abs, "rb") : NULL;
        free(abs);
        if (v->fp)
            v->fp_node = node + 1;
    }
    if (v->fp && !fseek(v->fp, (long)off, SEEK_SET))
        got = fread(buf, 1, n, v->fp);
    if (got < n)
        memset(buf + got, 0, n - got);
}

typedef struct hent {
    char *name;
    uint8_t dir;
    uint64_t size;
    time_t mtime;
} hent;

static int cmp_hent(const void *a, const void *b)
{
    return strcmp(((const hent *)a)->name, ((const hent *)b)->name);
}

/* The visible entries of a host directory, sorted. */
static hent *list_host(const char *abs, int *count)
{
    DIR *d = opendir(abs);
    struct dirent *de;
    hent *e = NULL;
    int n = 0, cap = 0, i, j, k;
    *count = 0;
    if (!d)
        return NULL;
    while ((de = readdir(d))) {
        struct stat st;
        char *p;
        if (de->d_name[0] == '.')
            continue;
        p = join(abs, de->d_name);
        if (!p || stat(p, &st)) {
            free(p);
            continue;
        }
        free(p);
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
            continue;
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size > 0xFFFFFFFFu) {
            fprintf(stderr, "vfat: %s/%s is too big for FAT32, skipped\n", abs, de->d_name);
            continue;
        }
        if (n == cap) {
            hent *t;
            cap = cap ? cap * 2 : 32;
            t = realloc(e, (size_t)cap * sizeof *t);
            if (!t)
                break;
            e = t;
        }
        e[n].name = xstrdup(de->d_name);
        e[n].dir = S_ISDIR(st.st_mode);
        e[n].size = e[n].dir ? 0 : (uint64_t)st.st_size;
        e[n].mtime = st.st_mtime;
        n++;
    }
    closedir(d);
    if (n)
        qsort(e, (size_t)n, sizeof *e, cmp_hent);
    /* FAT names are case-insensitive: keep the first of names that clash */
    for (i = k = 0; i < n; i++) {
        for (j = 0; j < k; j++)
            if (ci_equal(e[j].name, e[i].name))
                break;
        if (j < k) {
            fprintf(stderr, "vfat: %s/%s clashes with %s, skipped\n", abs, e[i].name, e[j].name);
            free(e[i].name);
            continue;
        }
        e[k++] = e[i];
    }
    *count = k;
    return e;
}

static void free_hents(hent *e, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(e[i].name);
    free(e);
}

static void measure(const char *abs, int depth, uint64_t *bytes, uint64_t *count)
{
    int n, i;
    hent *e = list_host(abs, &n);
    for (i = 0; i < n; i++) {
        *count += 1;
        if (e[i].dir) {
            char *p = join(abs, e[i].name);
            if (p && depth < MAX_DEPTH)
                measure(p, depth + 1, bytes, count);
            free(p);
        } else {
            *bytes += e[i].size;
        }
    }
    free_hents(e, n);
}

/* ---- names ----------------------------------------------------------- */

static int sfn_char(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 128 && c != 0xE5)
        || (c && strchr("$%'-_@~`!(){}^#&", c));
}

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/*
 * The 8.3 form of the name: 1 if it is exact (case kept by the NT flags),
 * 2 if it is right apart from mixed case (it still needs a long name, but
 * the short name has no ~N tail, which programs that only know 8.3 names
 * expect), 0 if there is none.
 */
static int sfn_exact(const char *name, uint8_t sfn[11], uint8_t *nt)
{
    const char *dot = strrchr(name, '.');
    size_t lb = dot ? (size_t)(dot - name) : strlen(name);
    size_t le = dot ? strlen(dot + 1) : 0;
    int lower[2] = { 0, 0 }, up[2] = { 0, 0 };
    size_t i;
    if (lb < 1 || lb > 8 || le > 3 || (dot && le == 0))
        return 0;
    memset(sfn, ' ', 11);
    for (i = 0; i < lb + (dot ? 1 + le : 0); i++) {
        int c = (unsigned char)name[i], part = i > lb;
        if (i == lb)
            continue;
        if (c >= 'a' && c <= 'z')
            lower[part] = 1;
        else if (c >= 'A' && c <= 'Z')
            up[part] = 1;
        if (!sfn_char(upper(c)) || c >= 128)
            return 0;
        sfn[part ? 8 + (i - lb - 1) : i] = (uint8_t)upper(c);
    }
    if ((lower[0] && up[0]) || (lower[1] && up[1]))
        return 2;
    *nt = (uint8_t)((lower[0] ? 0x08 : 0) | (lower[1] ? 0x10 : 0));
    return 1;
}

/* A basis name for a long name, to get a ~N tail. */
static void sfn_basis(const char *name, uint8_t sfn[11], int *blen)
{
    const char *dot = strrchr(name, '.');
    const char *p;
    int n = 0;
    if (dot == name)
        dot = NULL;
    memset(sfn, ' ', 11);
    for (p = name; *p && p != dot && n < 6; p++) {
        int c = upper((unsigned char)*p);
        if (c == ' ' || c == '.')
            continue;
        sfn[n++] = (uint8_t)(sfn_char(c) && c < 128 ? c : '_');
    }
    if (!n)
        sfn[n++] = '_';
    *blen = n;
    if (dot) {
        int e = 0;
        for (p = dot + 1; *p && e < 3; p++) {
            int c = upper((unsigned char)*p);
            if (c == ' ' || c == '.')
                continue;
            sfn[8 + e++] = (uint8_t)(sfn_char(c) && c < 128 ? c : '_');
        }
    }
}

static uint8_t sfn_sum(const uint8_t *sfn)
{
    uint8_t s = 0;
    int i;
    for (i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + sfn[i]);
    return s;
}

/* UTF-8 to UCS-2; returns the length, or -1 if too long. */
static int to_ucs2(const char *s, uint16_t *out)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (c >= 0x80) {
            c &= 0x3F >> more;
            while (more-- && (*p & 0xC0) == 0x80)
                c = c << 6 | (*p++ & 0x3F);
        }
        if (n == MAX_LFN)
            return -1;
        out[n++] = (uint16_t)(c > 0xFFFF ? '_' : c);
    }
    return n;
}

static void from_ucs2(const uint16_t *s, int n, char *out)
{
    int i;
    for (i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c < 0x80) {
            *out++ = (char)c;
        } else if (c < 0x800) {
            *out++ = (char)(0xC0 | c >> 6);
            *out++ = (char)(0x80 | (c & 0x3F));
        } else {
            *out++ = (char)(0xE0 | c >> 12);
            *out++ = (char)(0x80 | (c >> 6 & 0x3F));
            *out++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *out = 0;
}

static void fat_datetime(time_t t, uint32_t *date, uint32_t *time_)
{
    struct tm *tm = localtime(&t);
    if (!tm || tm->tm_year < 80) {
        *date = 1 << 5 | 1;     /* 1980-01-01 */
        *time_ = 0;
        return;
    }
    *date = (uint32_t)((tm->tm_year - 80) << 9 | (tm->tm_mon + 1) << 5 | tm->tm_mday);
    *time_ = (uint32_t)(tm->tm_hour << 11 | tm->tm_min << 5 | tm->tm_sec / 2);
}

static void put_entry(uint8_t *e, const uint8_t sfn[11], uint8_t attr, uint8_t nt,
                      uint32_t clus, uint32_t size, time_t mtime)
{
    uint32_t date, tm;
    fat_datetime(mtime, &date, &tm);
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    if (e[0] == 0xE5)
        e[0] = 0x05;
    e[11] = attr;
    e[12] = nt;
    put16(e + 14, tm);
    put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 20, clus >> 16);
    put16(e + 22, tm);
    put16(e + 24, date);
    put16(e + 26, clus & 0xFFFF);
    put32(e + 28, size);
}

/* ---- layout ----------------------------------------------------------- */

static uint32_t alloc_run(vfat *v, uint64_t bytes)
{
    uint32_t n = (uint32_t)((bytes + v->csize - 1) / v->csize), first = v->next_clus, i;
    if (!n)
        return 0;
    if ((uint64_t)first + n > (uint64_t)v->nclus + 2) {
        v->full = 1;
        return 0;
    }
    for (i = 0; i < n; i++)
        v->fat[first + i] = i + 1 < n ? first + i + 1 : END_CHAIN;
    v->next_clus += n;
    return first;
}

typedef struct lname {
    uint8_t sfn[11];
    uint8_t nt;
    int nlfn;                   /* long name entries */
    uint16_t lfn[MAX_LFN];
    int len;
} lname;

static int sfn_used(const lname *ln, int n, const uint8_t *sfn)
{
    int i;
    for (i = 0; i < n; i++)
        if (!memcmp(ln[i].sfn, sfn, 11))
            return 1;
    return 0;
}

/* Lay out one directory and everything below it; returns its cluster. */
static uint32_t build_dir(vfat *v, const char *rel, uint32_t parent, int depth)
{
    char *abs = join(v->root, rel);
    int n, i, slots;
    hent *e = abs ? list_host(abs, &n) : NULL;
    lname *ln;
    uint8_t *dir, *p;
    uint32_t first, ncl, c;

    if (!abs || (!e && n)) {
        free(abs);
        return 0;
    }
    ln = calloc((size_t)n + 1, sizeof *ln);
    if (!ln) {
        free_hents(e, n);
        free(abs);
        return 0;
    }

    /* exact 8.3 names first, so that ~N tails avoid them */
    for (i = 0; i < n; i++) {
        int x = sfn_exact(e[i].name, ln[i].sfn, &ln[i].nt);
        ln[i].nlfn = x == 1 ? 0 : x == 2 ? -3 : -1;
        if (x && sfn_used(ln, i, ln[i].sfn))
            ln[i].nlfn = -1;
        else if (!x)
            memset(ln[i].sfn, 0, 11);
    }
    for (i = 0; i < n; i++) {
        int blen, k, keep = ln[i].nlfn == -3;
        if (ln[i].nlfn == 0)
            continue;
        ln[i].len = to_ucs2(e[i].name, ln[i].lfn);
        if (ln[i].len < 0) {
            fprintf(stderr, "vfat: %s/%s: name too long, skipped\n", abs, e[i].name);
            ln[i].nlfn = -2;
            continue;
        }
        ln[i].nlfn = (ln[i].len + 12) / 13;
        if (keep)
            continue;
        sfn_basis(e[i].name, ln[i].sfn, &blen);
        for (k = 1; k < 1000000; k++) {
            char tail[9];
            int tl = sprintf(tail, "~%d", k), at = blen + tl > 8 ? 8 - tl : blen, j;
            uint8_t s[11];
            memcpy(s, ln[i].sfn, 11);
            for (j = 0; j < tl; j++)
                s[at + j] = (uint8_t)tail[j];
            for (j = at + tl; j < 8; j++)
                s[j] = ' ';
            /* clashes are checked against every name, long or exact */
            if (!sfn_used(ln, n, s)) {
                memcpy(ln[i].sfn, s, 11);
                break;
            }
        }
    }

    slots = 2 + 1;              /* dot entries or the volume label, end */
    for (i = 0; i < n; i++)
        if (ln[i].nlfn >= 0)
            slots += 1 + ln[i].nlfn;
    if (v->fat16 && !*rel) {
        ncl = first = 0;
        dir = (uint64_t)slots * 32 <= (uint64_t)v->root_sectors * ZX_BLOCK ? v->rootdir : NULL;
        if (!dir)
            v->full = 1;
    } else {
        ncl = (uint32_t)(((uint64_t)slots * 32 + v->csize - 1) / v->csize);
        first = alloc_run(v, (uint64_t)ncl * v->csize);
        dir = first ? calloc(ncl, v->csize) : NULL;
    }
    if (!dir) {
        free(ln);
        free_hents(e, n);
        free(abs);
        return 0;
    }

    p = dir;
    if (*rel) {
        uint8_t dot[11], dotdot[11];
        memset(dot, ' ', 11);
        memset(dotdot, ' ', 11);
        dot[0] = dotdot[0] = dotdot[1] = '.';
        put_entry(p, dot, 0x10, 0, first, 0, time(NULL));
        put_entry(p + 32, dotdot, 0x10, 0, parent, 0, time(NULL));
        p += 64;
    } else {
        put_entry(p, (const uint8_t *)v->label, 0x08, 0, 0, 0, time(NULL));
        p += 32;
    }
    for (i = 0; i < n; i++) {
        char *crel;
        uint32_t clus = 0;
        int k;
        if (ln[i].nlfn < 0 || v->full)
            continue;
        crel = join(rel, e[i].name);
        if (!crel)
            continue;
        if (e[i].dir) {
            if (depth < MAX_DEPTH)
                clus = build_dir(v, crel, *rel ? first : 0, depth + 1);
            if (!clus) {            /* an entry with cluster 0 means the root */
                free(crel);
                continue;
            }
            push_dir(v, crel);
        } else {
            int node;
            clus = alloc_run(v, e[i].size);
            node = add_node(v, crel, e[i].size);
            for (c = 0; clus && node >= 0 && c * (uint64_t)v->csize < e[i].size; c++) {
                v->own[clus + c] = (uint32_t)node + 1;
                v->idx[clus + c] = c;
            }
            free(crel);
        }
        /* long name entries, last part first */
        for (k = ln[i].nlfn; k >= 1; k--) {
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            int j;
            memset(p, 0, 32);
            p[0] = (uint8_t)(k | (k == ln[i].nlfn ? 0x40 : 0));
            p[11] = 0x0F;
            p[13] = sfn_sum(ln[i].sfn);
            for (j = 0; j < 13; j++) {
                int at = (k - 1) * 13 + j;
                uint32_t ch = at < ln[i].len ? ln[i].lfn[at] : at == ln[i].len ? 0 : 0xFFFF;
                put16(p + pos[j], ch);
            }
            p += 32;
        }
        put_entry(p, ln[i].sfn, e[i].dir ? 0x10 : 0x20, ln[i].nt, clus,
                  (uint32_t)e[i].size, e[i].mtime);
        p += 32;
    }
    for (c = 0; c < ncl; c++) {
        uint8_t *b = malloc(v->csize);
        if (b)
            memcpy(b, dir + (size_t)c * v->csize, v->csize);
        v->buf[first + c] = b;
    }
    if (dir != v->rootdir)
        free(dir);
    free(ln);
    free_hents(e, n);
    free(abs);
    return first;
}

/* ---- the volume ------------------------------------------------------- */

static void make_boot(vfat *v)
{
    uint8_t *b = v->rsv, *fs = v->rsv + ZX_BLOCK;
    uint32_t used = v->next_clus - 2;
    static const uint8_t jmp[3] = { 0xEB, 0x58, 0x90 };

    memset(v->mbr, 0, sizeof v->mbr);
    v->mbr[446 + 1] = 0xFE;
    v->mbr[446 + 2] = 0xFF;
    v->mbr[446 + 3] = 0xFF;
    v->mbr[446 + 4] = v->fat16 ? 0x06 : 0x0C;   /* FAT16 over 32 MB, FAT32 with LBA */
    v->mbr[446 + 5] = 0xFE;
    v->mbr[446 + 6] = 0xFF;
    v->mbr[446 + 7] = 0xFF;
    put32(v->mbr + 446 + 8, PART_LBA);
    put32(v->mbr + 446 + 12, (uint32_t)v->part_sectors);
    v->mbr[510] = 0x55;
    v->mbr[511] = 0xAA;

    memset(v->rsv, 0, sizeof v->rsv);
    memcpy(b, jmp, 3);
    memcpy(b + 3, "ZXCRADLE", 8);
    put16(b + 11, ZX_BLOCK);
    b[13] = (uint8_t)v->spc;
    put16(b + 14, RESERVED);
    b[16] = 2;
    b[21] = 0xF8;
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 28, PART_LBA);
    put32(b + 32, (uint32_t)v->part_sectors);
    if (v->fat16) {
        b[1] = 0x3C;
        put16(b + 17, v->root_sectors * (ZX_BLOCK / 32));
        put16(b + 22, v->fatsz);
        b[36] = 0x80;
        b[38] = 0x29;
        put32(b + 39, 0x5A584E58);
        memcpy(b + 43, v->label, 11);
        memcpy(b + 54, "FAT16   ", 8);
        b[510] = 0x55;
        b[511] = 0xAA;
        return;
    }
    put32(b + 36, v->fatsz);
    put32(b + 44, 2);                           /* root directory cluster */
    put16(b + 48, 1);                           /* FSInfo sector */
    put16(b + 50, 6);                           /* backup boot sector */
    b[64] = 0x80;
    b[66] = 0x29;
    put32(b + 67, 0x5A584E58);
    memcpy(b + 71, v->label, 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55;
    b[511] = 0xAA;

    put32(fs, 0x41615252);
    put32(fs + 484, 0x61417272);
    put32(fs + 488, v->nclus - used);
    put32(fs + 492, v->next_clus);
    put32(fs + 508, 0xAA550000);

    memcpy(v->rsv + 6 * ZX_BLOCK, b, ZX_BLOCK);
    memcpy(v->rsv + 7 * ZX_BLOCK, fs, ZX_BLOCK);
}

static void cluster_read(vfat *v, uint32_t c, uint32_t off, uint8_t *buf, uint32_t n)
{
    if (c < 2 || c >= v->nclus + 2)
        memset(buf, 0, n);
    else if (v->buf[c])
        memcpy(buf, v->buf[c] + off, n);
    else if (v->own[c])
        host_read(v, v->own[c] - 1, (uint64_t)v->idx[c] * v->csize + off, buf, n);
    else
        memset(buf, 0, n);
}

static int vf_read(zx_blockdev *bd, uint64_t lba, uint8_t *buf)
{
    vfat *v = (vfat *)bd;
    uint64_t r;
    if (lba >= bd->blocks) {
        memset(buf, 0, ZX_BLOCK);
        return -1;
    }
    if (lba < PART_LBA) {
        if (lba == 0)
            memcpy(buf, v->mbr, ZX_BLOCK);
        else
            memset(buf, 0, ZX_BLOCK);
        return 0;
    }
    r = lba - PART_LBA;
    if (r < RESERVED) {
        memcpy(buf, v->rsv + r * ZX_BLOCK, ZX_BLOCK);
    } else if (r < RESERVED + 2ull * v->fatsz && v->fat16) {
        uint32_t f = (uint32_t)((r - RESERVED) % v->fatsz) * (ZX_BLOCK / 2), i;
        for (i = 0; i < ZX_BLOCK / 2; i++)
            put16(buf + i * 2, f + i < v->nclus + 2 ? v->fat[f + i] & 0xFFFF : 0);
    } else if (r < RESERVED + 2ull * v->fatsz) {
        uint32_t f = (uint32_t)((r - RESERVED) % v->fatsz) * (ZX_BLOCK / 4), i;
        for (i = 0; i < ZX_BLOCK / 4; i++)
            put32(buf + i * 4, f + i < v->nclus + 2 ? v->fat[f + i] : 0);
    } else if (r < v->data_start) {
        memcpy(buf, v->rootdir + (r - RESERVED - 2ull * v->fatsz) * ZX_BLOCK, ZX_BLOCK);
    } else {
        uint64_t d = r - v->data_start;
        cluster_read(v, (uint32_t)(d / v->spc) + 2, (uint32_t)(d % v->spc) * ZX_BLOCK, buf, ZX_BLOCK);
    }
    return 0;
}

static int vf_write(zx_blockdev *bd, uint64_t lba, const uint8_t *buf)
{
    vfat *v = (vfat *)bd;
    uint64_t r;
    if (lba >= bd->blocks)
        return -1;
    v->pending = 1;
    v->idle = 0;
    if (lba < PART_LBA) {
        if (lba == 0)
            memcpy(v->mbr, buf, ZX_BLOCK);
        return 0;
    }
    r = lba - PART_LBA;
    if (r < RESERVED) {
        memcpy(v->rsv + r * ZX_BLOCK, buf, ZX_BLOCK);
    } else if (r < RESERVED + 2ull * v->fatsz && v->fat16) {
        uint32_t f = (uint32_t)((r - RESERVED) % v->fatsz) * (ZX_BLOCK / 2), i;
        for (i = 0; i < ZX_BLOCK / 2; i++)
            if (f + i < v->nclus + 2) {
                uint32_t n = get16(buf + i * 2);
                v->fat[f + i] = n >= 0xFFF7 ? n | 0x0FFF0000 : n;
            }
    } else if (r < RESERVED + 2ull * v->fatsz) {
        uint32_t f = (uint32_t)((r - RESERVED) % v->fatsz) * (ZX_BLOCK / 4), i;
        for (i = 0; i < ZX_BLOCK / 4; i++)
            if (f + i < v->nclus + 2)
                v->fat[f + i] = get32(buf + i * 4) & FAT_MASK;
    } else if (r < v->data_start) {
        memcpy(v->rootdir + (r - RESERVED - 2ull * v->fatsz) * ZX_BLOCK, buf, ZX_BLOCK);
    } else {
        uint64_t d = r - v->data_start;
        uint32_t c = (uint32_t)(d / v->spc) + 2;
        if (!v->buf[c]) {
            uint8_t *b = malloc(v->csize);
            if (!b)
                return -1;
            cluster_read(v, c, 0, b, v->csize);
            v->buf[c] = b;
        }
        memcpy(v->buf[c] + (d % v->spc) * ZX_BLOCK, buf, ZX_BLOCK);
        v->dirty[c] = 1;
    }
    return 0;
}

/* ---- writing back ------------------------------------------------------ */

typedef struct glist {
    gfile *f;
    uint32_t n, cap;
} glist;

static void glist_add(glist *g, char *path, uint32_t first, uint64_t size, int dir)
{
    if (g->n == g->cap) {
        uint32_t cap = g->cap ? g->cap * 2 : 256;
        gfile *p = realloc(g->f, cap * sizeof *p);
        if (!p) {
            free(path);
            return;
        }
        g->f = p;
        g->cap = cap;
    }
    g->f[g->n].path = path;
    g->f[g->n].first = first;
    g->f[g->n].size = size;
    g->f[g->n].dir = (uint8_t)dir;
    g->f[g->n].changed = 0;
    g->f[g->n].node = -1;
    g->n++;
}

static uint32_t fat_next(vfat *v, uint32_t c)
{
    uint32_t n = v->fat[c];
    return n >= 2 && n < v->nclus + 2 ? n : 0;
}

/* Read the volume's directory tree as the machine left it. */
static void parse_dir(vfat *v, uint32_t first, const char *rel, int depth, glist *g)
{
    uint8_t *cl = malloc(v->csize);
    uint16_t lfn[260];
    int lfn_parts = 0, lfn_seen = 0;
    uint8_t lfn_sum = 0;
    uint32_t c = first, steps = 0, start = g->n, i;
    int root = v->fat16 && !first;

    if (!cl)
        return;
    while ((c || root) && steps++ <= v->nclus) {
        uint32_t span = root ? v->root_sectors * ZX_BLOCK : v->csize;
        for (i = 0; i < span; i += 32) {
            uint8_t *e;
            if (root)
                memcpy(cl, v->rootdir + i, 32);
            else
                cluster_read(v, c, i, cl, 32);
            e = cl;
            if (e[0] == 0)
                goto done;
            if (e[0] == 0xE5) {
                lfn_parts = 0;
                continue;
            }
            if ((e[11] & 0x3F) == 0x0F) {
                int k = e[0] & 0x1F, j;
                static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                if (e[0] & 0x40) {
                    lfn_parts = k;
                    lfn_seen = 0;
                    lfn_sum = e[13];
                    memset(lfn, 0xFF, sizeof lfn);
                }
                if (k < 1 || k > 20 || k > lfn_parts || e[13] != lfn_sum) {
                    lfn_parts = 0;
                    continue;
                }
                for (j = 0; j < 13; j++)
                    lfn[(k - 1) * 13 + j] = (uint16_t)get16(e + pos[j]);
                lfn_seen |= 1 << (k - 1);
                continue;
            }
            if (e[11] & 0x08) {                 /* volume label */
                lfn_parts = 0;
                continue;
            }
            {
                char name[MAX_LFN * 3 + 1];
                uint32_t clus = get16(e + 20) << 16 | get16(e + 26);
                int dir = (e[11] & 0x10) != 0, len, j;
                if (lfn_parts && lfn_seen == (1 << lfn_parts) - 1 && sfn_sum(e) == lfn_sum) {
                    for (len = 0; len < lfn_parts * 13 && lfn[len] && lfn[len] != 0xFFFF; len++)
                        ;
                    from_ucs2(lfn, len, name);
                } else {
                    int n = 0;
                    for (j = 0; j < 8 && e[j] != ' '; j++)
                        name[n++] = (char)((e[12] & 0x08) && e[j] >= 'A' && e[j] <= 'Z' ? e[j] + 32 : e[j]);
                    if (n && (uint8_t)name[0] == 0x05)
                        name[0] = (char)0xE5;
                    if (e[8] != ' ') {
                        name[n++] = '.';
                        for (j = 8; j < 11 && e[j] != ' '; j++)
                            name[n++] = (char)((e[12] & 0x10) && e[j] >= 'A' && e[j] <= 'Z' ? e[j] + 32 : e[j]);
                    }
                    name[n] = 0;
                }
                lfn_parts = 0;
                if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')
                    || strchr(name, '\\'))
                    continue;
                {
                    char *p = join(rel, name);
                    if (!p)
                        continue;
                    /* the same name twice: keep the first (entries from
                       subdirectories are in the list too, but their paths
                       are longer) */
                    for (j = (int)start; j < (int)g->n; j++)
                        if (ci_equal(g->f[j].path, p))
                            break;
                    if (j < (int)g->n) {
                        free(p);
                        continue;
                    }
                    glist_add(g, p, clus, dir ? 0 : get32(e + 28), dir);
                    if (dir && clus >= 2 && clus < v->nclus + 2 && depth < MAX_DEPTH)
                        parse_dir(v, clus, p, depth + 1, g);
                }
            }
        }
        if (root)
            break;
        c = fat_next(v, c);
    }
done:
    free(cl);
}

typedef struct byname {
    const char *path;           /* NULL sorts last */
    uint32_t node;
} byname;

static int cmp_byname(const void *a, const void *b)
{
    const char *x = ((const byname *)a)->path, *y = ((const byname *)b)->path;
    if (!x || !y)
        return !x - !y;
    return strcmp(x, y);
}

static int find_node(const byname *order, uint32_t n, const char *path)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const char *p = order[mid].path;
        int r = p ? strcmp(p, path) : 1;
        if (!r)
            return (int)order[mid].node;
        if (r < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

static int cmp_len_desc(const void *a, const void *b)
{
    size_t x = strlen(*(char *const *)a), y = strlen(*(char *const *)b);
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Write a volume file's content to a host file. */
static int write_file(vfat *v, const gfile *g, const char *abs)
{
    FILE *fp = fopen(abs, "wb");
    uint8_t *b = malloc(v->csize);
    uint64_t left = g->size;
    uint32_t c = g->first, steps = 0;
    int ok = fp && b;
    while (ok && left) {
        uint32_t n = left < v->csize ? (uint32_t)left : v->csize;
        if (c && steps++ <= v->nclus)
            cluster_read(v, c, 0, b, n);
        else
            memset(b, 0, n);
        ok = fwrite(b, 1, n, fp) == n;
        left -= n;
        c = c ? fat_next(v, c) : 0;
    }
    free(b);
    if (fp && fclose(fp))
        ok = 0;
    return ok ? 0 : -1;
}

static int vf_flush(zx_blockdev *bd)
{
    vfat *v = (vfat *)bd;
    glist g = { NULL, 0, 0 };
    byname *order;
    uint32_t i, c;
    char **tmp = NULL, **gdirs = NULL;
    uint32_t ngdirs = 0;
    int err = 0;

    if (!v->pending)
        return 0;
    host_close(v);
    parse_dir(v, v->fat16 ? 0 : 2, "", 0, &g);

    order = malloc((v->nnode + 1) * sizeof *order);
    tmp = calloc(g.n + 1, sizeof *tmp);
    gdirs = malloc((g.n + 1) * sizeof *gdirs);
    if (!order || !tmp || !gdirs) {
        err = -1;
        goto out;
    }
    for (i = 0; i < v->nnode; i++) {
        order[i].path = v->node[i].path;
        order[i].node = i;
        v->node[i].seen = v->node[i].doomed = 0;
        v->node[i].gone = 1;
    }
    qsort(order, v->nnode, sizeof *order, cmp_byname);

    /* what changed */
    for (i = 0; i < g.n; i++) {
        gfile *f = &g.f[i];
        int n;
        uint32_t k, need;
        if (f->dir) {
            gdirs[ngdirs++] = f->path;
            continue;
        }
        n = find_node(order, v->nnode, f->path);
        f->node = n;
        if (n >= 0)
            v->node[n].gone = 0;
        f->changed = 1;
        if (n >= 0 && v->node[n].size == f->size) {
            need = (uint32_t)((f->size + v->csize - 1) / v->csize);
            for (k = 0, c = f->first; k < need; k++, c = fat_next(v, c))
                if (!c || v->own[c] != (uint32_t)n + 1 || v->idx[c] != k || v->dirty[c])
                    break;
            f->changed = k < need;
        }
        if (n >= 0) {
            if (f->changed)
                v->node[n].doomed = 1;
            else
                v->node[n].seen = 1;
        }
    }
    for (i = 0; i < v->nnode; i++)
        if (v->node[i].path && v->node[i].gone)
            v->node[i].doomed = 1;

    /* keep what doomed host files still back, then let go of them */
    for (c = 2; c < v->nclus + 2; c++) {
        if (!v->own[c] || !v->node[v->own[c] - 1].doomed)
            continue;
        if (v->fat[c] && !v->buf[c]) {
            uint8_t *b = malloc(v->csize);
            if (!b) {
                err = -1;
                goto out;
            }
            cluster_read(v, c, 0, b, v->csize);
            v->buf[c] = b;
        }
        host_close(v);
        v->own[c] = 0;
        v->dirty[c] = 1;
    }

    /* new contents go to temporary files first */
    for (i = 0; i < g.n && !err; i++) {
        char name[32];
        if (g.f[i].dir || !g.f[i].changed)
            continue;
        sprintf(name, ".zxc-sync-%u.tmp", i);
        tmp[i] = join(v->root, name);
        if (!tmp[i] || write_file(v, &g.f[i], tmp[i]))
            err = -1;
    }
    if (err)
        goto out;

    /* delete, then directories, then move the new files in */
    for (i = 0; i < v->nnode; i++) {
        vnode *n = &v->node[i];
        if (n->path && n->doomed) {
            char *abs = join(v->root, n->path);
            if (abs && n->gone)
                remove(abs);
            free(abs);
            if (n->gone) {
                free(n->path);
                n->path = NULL;
            }
        }
    }
    qsort(gdirs, ngdirs, sizeof *gdirs, cmp_str);
    qsort(v->dirs, v->ndirs, sizeof *v->dirs, cmp_len_desc);
    for (i = 0; i < v->ndirs; i++)
        if (!bsearch(&v->dirs[i], gdirs, ngdirs, sizeof *gdirs, cmp_str)) {
            char *abs = join(v->root, v->dirs[i]);
            if (abs)
                rmdir(abs);
            free(abs);
        }
    for (i = 0; i < ngdirs; i++) {          /* sorted, so parents come first */
        char *abs = join(v->root, gdirs[i]);
        if (abs)
            mkdir(abs, 0777);
        free(abs);
    }
    for (i = 0; i < g.n; i++) {
        char *abs;
        if (!tmp[i])
            continue;
        abs = join(v->root, g.f[i].path);
        if (abs && rename(tmp[i], abs)) {
            remove(abs);
            if (rename(tmp[i], abs)) {
                fprintf(stderr, "vfat: cannot write %s\n", abs);
                remove(tmp[i]);
                err = -1;
            }
        }
        free(abs);
    }

    /* the new files now back their clusters */
    for (i = 0; i < g.n; i++) {
        gfile *f = &g.f[i];
        int n = f->node;
        uint32_t k, need;
        if (!tmp[i])
            continue;
        if (n < 0)
            n = add_node(v, f->path, f->size);
        if (n < 0)
            continue;
        v->node[n].size = f->size;
        v->node[n].doomed = 0;
        need = (uint32_t)((f->size + v->csize - 1) / v->csize);
        for (k = 0, c = f->first; k < need && c; k++, c = fat_next(v, c)) {
            free(v->buf[c]);
            v->buf[c] = NULL;
            v->dirty[c] = 0;
            v->own[c] = (uint32_t)n + 1;
            v->idx[c] = k;
        }
    }

    for (i = 0; i < v->ndirs; i++)
        free(v->dirs[i]);
    v->ndirs = 0;
    for (i = 0; i < ngdirs; i++) {
        push_dir(v, gdirs[i]);
        gdirs[i] = NULL;
    }
    for (i = 0; i < g.n; i++)
        if (g.f[i].dir)
            g.f[i].path = NULL;            /* moved to v->dirs */
    if (!err)
        v->pending = 0;
out:
    for (i = 0; i < g.n; i++) {
        if (tmp && tmp[i])
            free(tmp[i]);
        free(g.f[i].path);
    }
    free(g.f);
    free(tmp);
    free(gdirs);
    free(order);
    return err;
}

static void vf_tick(zx_blockdev *bd)
{
    vfat *v = (vfat *)bd;
    if (v->pending && ++v->idle >= SYNC_IDLE) {
        v->idle = 0;
        vf_flush(bd);
    }
}

static void vf_close(zx_blockdev *bd)
{
    vfat *v = (vfat *)bd;
    uint32_t i;
    host_close(v);
    if (v->buf)
        for (i = 0; i < v->nclus + 2; i++)
            free(v->buf[i]);
    for (i = 0; i < v->nnode; i++)
        free(v->node[i].path);
    for (i = 0; i < v->ndirs; i++)
        free(v->dirs[i]);
    free(v->dirs);
    free(v->node);
    free(v->buf);
    free(v->dirty);
    free(v->own);
    free(v->idx);
    free(v->fat);
    free(v->rootdir);
    free(v->root);
    free(v);
}

/* Root directory entries a FAT16 volume needs for the folder's top level,
 * with long names (counted generously), at least the usual 512. */
static uint32_t root_entries(const char *path)
{
    int n, i;
    hent *e = list_host(path, &n);
    uint64_t need = 2;
    for (i = 0; i < n; i++)
        need += 1 + (strlen(e[i].name) + 12) / 13;
    free_hents(e, n);
    need = (need + 15) & ~15ull;
    return need < 512 ? 512 : need > 0xFFF0 ? 0xFFF0 : (uint32_t)need;
}

static zx_blockdev *open_folder(const char *path, uint32_t size_mb, int fat16)
{
    vfat *v;
    struct stat st;
    uint64_t bytes = 0, count = 0, sectors, total, max;
    size_t len;

    if (stat(path, &st) || !S_ISDIR(st.st_mode))
        return NULL;
    v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->root = xstrdup(path);
    len = v->root ? strlen(v->root) : 0;
    while (len > 1 && v->root[len - 1] == '/')
        v->root[--len] = 0;

    v->fat16 = fat16;
    v->label = fat16 ? "ZXCRADLE   " : "NEXT       ";

    /* size: what was asked, else at least 2 GB (FAT16: 1 GB) with room to
       grow. FAT16 stops short of 2 GB: 65524 clusters of 32K at most. */
    measure(path, 0, &bytes, &count);
    if (size_mb)
        total = (uint64_t)size_mb << 20;
    else {
        total = (uint64_t)(fat16 ? 1 : 2) << 30;
        if (bytes * 2 + ((uint64_t)(fat16 ? 256 : 1024) << 20) > total)
            total = bytes * 2 + ((uint64_t)(fat16 ? 256 : 1024) << 20);
    }
    max = fat16 ? (uint64_t)2047 << 20 : (uint64_t)32 << 30;
    if (total > max)
        total = max;
    if (total < (uint64_t)(fat16 ? 34 : 64) << 20)
        total = (uint64_t)(fat16 ? 34 : 64) << 20;
    if (fat16)
        for (v->spc = 1; v->spc < 64 && total / ((uint64_t)v->spc * ZX_BLOCK) > 65000; v->spc *= 2)
            ;
    else
        for (v->spc = 8; v->spc < 64 && total / ((uint64_t)v->spc * ZX_BLOCK) > (1u << 20); v->spc *= 2)
            ;
    v->csize = v->spc * ZX_BLOCK;
    v->root_sectors = fat16 ? root_entries(path) * 32 / ZX_BLOCK : 0;
    sectors = total / ZX_BLOCK - PART_LBA;
    v->nclus = (uint32_t)((sectors - RESERVED - v->root_sectors) / v->spc);
    if (fat16 && v->nclus > 65524)
        v->nclus = 65524;
    for (;;) {
        v->fatsz = (uint32_t)(((uint64_t)v->nclus + 2) * (fat16 ? 2 : 4) + ZX_BLOCK - 1) / ZX_BLOCK;
        if (RESERVED + 2ull * v->fatsz + v->root_sectors + (uint64_t)v->nclus * v->spc <= sectors)
            break;
        v->nclus--;
    }
    v->data_start = RESERVED + 2 * v->fatsz + v->root_sectors;
    v->part_sectors = v->data_start + (uint64_t)v->nclus * v->spc;
    v->bd.blocks = PART_LBA + v->part_sectors;

    v->fat = calloc(v->nclus + 2, sizeof *v->fat);
    v->buf = calloc(v->nclus + 2, sizeof *v->buf);
    v->dirty = calloc(v->nclus + 2, 1);
    v->own = calloc(v->nclus + 2, sizeof *v->own);
    v->idx = calloc(v->nclus + 2, sizeof *v->idx);
    v->rootdir = fat16 ? calloc(v->root_sectors, ZX_BLOCK) : NULL;
    if (!v->root || !v->fat || !v->buf || !v->dirty || !v->own || !v->idx || (fat16 && !v->rootdir)) {
        vf_close(&v->bd);
        return NULL;
    }
    v->fat[0] = 0x0FFFFFF8;
    v->fat[1] = END_CHAIN;
    v->next_clus = 2;
    if (build_dir(v, "", 0, 0) != (fat16 ? 0u : 2u) || v->full) {
        fprintf(stderr, "vfat: %s does not fit in a %lu MB volume\n", path,
                (unsigned long)(total >> 20));
        vf_close(&v->bd);
        return NULL;
    }
    make_boot(v);

    v->bd.read = vf_read;
    v->bd.write = vf_write;
    v->bd.tick = vf_tick;
    v->bd.flush = vf_flush;
    v->bd.close = vf_close;
    return &v->bd;
}

zx_blockdev *zx_bd_open_folder(const char *path, uint32_t size_mb)
{
    return open_folder(path, size_mb, 0);
}

zx_blockdev *zx_bd_open_folder16(const char *path, uint32_t size_mb)
{
    return open_folder(path, size_mb, 1);
}
