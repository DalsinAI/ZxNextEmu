/*
 * A host folder as the core sees it: the few file operations the
 * folder-backed devices (vfat.c, the Pi's files) need, behind callbacks.
 * Paths are relative to the folder, '/' separated, "" for the folder
 * itself. Natively it is the real file system (hostfs_posix.c); in the
 * browser the page supplies it.
 */
#ifndef ZX_HOSTFS_H
#define ZX_HOSTFS_H

#include <stdint.h>

typedef struct zx_hostfs zx_hostfs;

typedef struct zx_hostent {
    const char *name;
    uint8_t dir;
    uint64_t size;
    int64_t mtime;              /* seconds since 1970, in local time */
} zx_hostent;

typedef void zx_hostfs_each(void *ctx, const zx_hostent *e);

struct zx_hostfs {
    /* Call each() for every entry of a directory except those whose names
     * start with a dot; 0, or -1 if it cannot be read. */
    int (*list)(zx_hostfs *fs, const char *rel, zx_hostfs_each *each, void *ctx);
    /* Bytes read (fewer at the end of the file), or -1. */
    int32_t (*read)(zx_hostfs *fs, const char *rel, uint64_t off, uint8_t *buf, uint32_t n);
    /* Write n bytes at off; off 0 creates or empties the file first. Writes
     * to one file come in order. 0 or -1. */
    int (*write)(zx_hostfs *fs, const char *rel, uint64_t off, const uint8_t *buf, uint32_t n);
    int (*remove)(zx_hostfs *fs, const char *rel);
    int (*mkdir)(zx_hostfs *fs, const char *rel);
    int (*rmdir)(zx_hostfs *fs, const char *rel);
    /* Replace `to` if it exists. */
    int (*rename)(zx_hostfs *fs, const char *from, const char *to);
    /* The time now, as mtime counts it. */
    int64_t (*now)(zx_hostfs *fs);
    /* A message for the user (may be NULL). */
    void (*log)(zx_hostfs *fs, const char *msg);
    /* Done with it: close what is open (may be NULL). */
    void (*close)(zx_hostfs *fs);
};

/* Host side: a folder of the real file system, or NULL if it is not one. */
zx_hostfs *zx_hostfs_posix_new(const char *root);

#endif
