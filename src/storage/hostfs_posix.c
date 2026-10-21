/*
 * zx_hostfs over the real file system, for native builds.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "hostfs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct pfs {
    zx_hostfs fs;
    char *root;
    FILE *rfp, *wfp;            /* the file last read and last written */
    char *rpath, *wpath;
} pfs;

static char *abs_path(pfs *p, const char *rel)
{
    size_t la = strlen(p->root), lb = strlen(rel);
    char *s = malloc(la + lb + 2);
    if (!s)
        return NULL;
    memcpy(s, p->root, la);
    if (lb)
        s[la++] = '/';
    memcpy(s + la, rel, lb + 1);
    return s;
}

static void drop(FILE **fp, char **path)
{
    if (*fp)
        fclose(*fp);
    *fp = NULL;
    free(*path);
    *path = NULL;
}

static void close_files(pfs *p)
{
    drop(&p->rfp, &p->rpath);
    drop(&p->wfp, &p->wpath);
}

static int64_t local_secs(time_t t)
{
    struct tm *tm = localtime(&t);
    return (int64_t)t + (tm ? tm->tm_gmtoff : 0);
}

static int p_list(zx_hostfs *fs, const char *rel, zx_hostfs_each *each, void *ctx)
{
    pfs *p = (pfs *)fs;
    char *dir = abs_path(p, rel);
    DIR *d = dir ? opendir(dir) : NULL;
    struct dirent *de;
    if (!d) {
        free(dir);
        return -1;
    }
    while ((de = readdir(d))) {
        struct stat st;
        zx_hostent e;
        char *path;
        if (de->d_name[0] == '.')
            continue;
        path = malloc(strlen(dir) + strlen(de->d_name) + 2);
        if (!path)
            continue;
        sprintf(path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
            free(path);
            continue;
        }
        free(path);
        e.name = de->d_name;
        e.dir = S_ISDIR(st.st_mode) != 0;
        e.size = e.dir ? 0 : (uint64_t)st.st_size;
        e.mtime = local_secs(st.st_mtime);
        each(ctx, &e);
    }
    closedir(d);
    free(dir);
    return 0;
}

static int32_t p_read(zx_hostfs *fs, const char *rel, uint64_t off, uint8_t *buf, uint32_t n)
{
    pfs *p = (pfs *)fs;
    if (!p->rpath || strcmp(p->rpath, rel)) {
        char *a = abs_path(p, rel);
        drop(&p->rfp, &p->rpath);
        p->rfp = a ? fopen(a, "rb") : NULL;
        free(a);
        if (!p->rfp)
            return -1;
        p->rpath = strdup(rel);
    }
    if (fseeko(p->rfp, (off_t)off, SEEK_SET))
        return -1;
    return (int32_t)fread(buf, 1, n, p->rfp);
}

static int p_write(zx_hostfs *fs, const char *rel, uint64_t off, const uint8_t *buf, uint32_t n)
{
    pfs *p = (pfs *)fs;
    if (off == 0 || !p->wpath || strcmp(p->wpath, rel)) {
        char *a = abs_path(p, rel);
        drop(&p->wfp, &p->wpath);
        p->wfp = a ? fopen(a, off ? "r+b" : "wb") : NULL;
        free(a);
        if (!p->wfp)
            return -1;
        p->wpath = strdup(rel);
    }
    if (fseeko(p->wfp, (off_t)off, SEEK_SET) || fwrite(buf, 1, n, p->wfp) != n)
        return -1;
    return 0;
}

#define WITH_PATH(rel, call) do { \
        char *a; int r; close_files(p); \
        if (!(a = abs_path(p, rel))) return -1; \
        r = (call); free(a); return r ? -1 : 0; } while (0)

static int p_remove(zx_hostfs *fs, const char *rel) { pfs *p = (pfs *)fs; WITH_PATH(rel, remove(a)); }
static int p_mkdir(zx_hostfs *fs, const char *rel) { pfs *p = (pfs *)fs; WITH_PATH(rel, mkdir(a, 0777)); }
static int p_rmdir(zx_hostfs *fs, const char *rel) { pfs *p = (pfs *)fs; WITH_PATH(rel, rmdir(a)); }

static int p_rename(zx_hostfs *fs, const char *from, const char *to)
{
    pfs *p = (pfs *)fs;
    char *a, *b;
    int r = -1;
    close_files(p);
    a = abs_path(p, from);
    b = abs_path(p, to);
    if (a && b) {
        r = rename(a, b);
        if (r) {
            remove(b);
            r = rename(a, b);
        }
    }
    free(a);
    free(b);
    return r ? -1 : 0;
}

static int64_t p_now(zx_hostfs *fs)
{
    (void)fs;
    return local_secs(time(NULL));
}

static void p_log(zx_hostfs *fs, const char *msg)
{
    (void)fs;
    fprintf(stderr, "%s\n", msg);
}

static void p_close(zx_hostfs *fs)
{
    pfs *p = (pfs *)fs;
    close_files(p);
    free(p->root);
    free(p);
}

zx_hostfs *zx_hostfs_posix_new(const char *root)
{
    struct stat st;
    pfs *p;
    size_t len;
    if (stat(root, &st) || !S_ISDIR(st.st_mode))
        return NULL;
    p = calloc(1, sizeof *p);
    if (!p || !(p->root = strdup(root))) {
        free(p);
        return NULL;
    }
    len = strlen(p->root);
    while (len > 1 && p->root[len - 1] == '/')
        p->root[--len] = 0;
    p->fs.list = p_list;
    p->fs.read = p_read;
    p->fs.write = p_write;
    p->fs.remove = p_remove;
    p->fs.mkdir = p_mkdir;
    p->fs.rmdir = p_rmdir;
    p->fs.rename = p_rename;
    p->fs.now = p_now;
    p->fs.log = p_log;
    p->fs.close = p_close;
    return &p->fs;
}
