/*
 * The Raspberry Pi Zero "accelerator" in the Spectrum Next, running
 * NextPi, as seen from UART 1.
 *
 * NextPi is DietPi Linux with a supervisor shell on the serial console:
 * prompt "SUP>", tty echo, ^C cancels the line and ^D restarts the shell
 * with its "DietPi for the SpecNext" banner (which is how .pisend -q finds
 * the Pi). This emulates that shell and the nextpi-* commands the Next's
 * dot commands use, rather than running Linux. The Pi's file system is a
 * host folder (/ram, /NextPi, ... inside it), one per machine like the SD
 * card.
 *
 * File transfers, as the dot commands speak them:
 *
 *   nextpi-file_stream > 'path'      (.pisend file)
 *       base64 lines after a "begin-base64" line, up to an empty line
 *   nextpi-file_receive -nbn 256     (.piput)
 *       Pi: OK. Next: size, number of 16K blocks, remainder (32/32/16
 *       bits, big endian) and a 0-terminated name; Pi: OK. Then each
 *       block and the remainder, each followed by an 8-bit sum; Pi: OK
 *       after each (ER asks for it again). The file goes to /ram.
 *   nextpi-file_transmit -nbn        (.piget)
 *       Pi: OK. Next: a 0 byte and a 0-terminated path. Pi: size,
 *       blocks and remainder as above (no name). Then for each '!' the
 *       next 16K block (the last one shorter) and its sum; '?' asks for
 *       the same block again.
 */
#define _POSIX_C_SOURCE 200809L
#include "nextpi.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OUT_SIZE   (1 << 16)
#define LINE_MAX   1024
#define TX_BLOCK   256
#define BLOCK      16384       /* .piput's blocks: one 16K bank each */
#define VERSION    "1.92"
#define BANNER     "DietPi for the SpecNext"

enum { SHELL, STREAM_WAIT, STREAM, RECV_HDR, RECV_DATA, TX_NAME, TX_ACK };

typedef struct pi {
    zx_serial s;
    char *root;                 /* host folder holding the Pi's files */
    char cwd[256];
    uint8_t out[OUT_SIZE];
    int out_head, out_len;
    char line[LINE_MAX];
    int line_len;
    int mode;
    FILE *fp;                   /* file being written or read */
    char path[512];             /* its host path, to remove on failure */
    /* NBN-style transfers */
    uint8_t hdr[300];
    int hdr_len;
    uint32_t size, blocks, block;
    uint16_t rem;
    uint32_t got;               /* bytes of the current block */
    uint8_t sum;
    uint8_t buf[BLOCK];
    uint32_t buf_len;
    /* base64 */
    uint32_t b64, b64n;
} pi;

/* ---- output ------------------------------------------------------------ */

static void out_raw(pi *p, const void *v, int n)
{
    const uint8_t *b = v;
    while (n-- > 0 && p->out_len < OUT_SIZE)
        p->out[(p->out_head + p->out_len++) % OUT_SIZE] = *b++;
}

/* Text through the tty: \n goes out as \r\n. */
static void say(pi *p, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            out_raw(p, "\r", 1);
        out_raw(p, s, 1);
    }
}

static void sayf(pi *p, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(p, buf);
}

static void prompt(pi *p)
{
    say(p, "SUP>");
}

/* ---- paths ------------------------------------------------------------- */

/* Host path for a Pi path; NULL if it tries to leave the root. */
static int host_path(pi *p, const char *in, char *out, size_t max)
{
    char abs[600], norm[600];
    char *seg, *save = NULL;
    size_t n = 0;
    if (in[0] == '/')
        snprintf(abs, sizeof abs, "%s", in);
    else
        snprintf(abs, sizeof abs, "%s/%s", p->cwd, in);
    norm[0] = 0;
    for (seg = strtok_r(abs, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (!strcmp(seg, ".") || !*seg)
            continue;
        if (!strcmp(seg, "..")) {
            char *slash = strrchr(norm, '/');
            if (slash)
                *slash = 0;
            n = strlen(norm);
            continue;
        }
        n += (size_t)snprintf(norm + n, sizeof norm - n, "/%s", seg);
        if (n >= sizeof norm)
            return -1;
    }
    return snprintf(out, max, "%s%s", p->root, norm) >= (int)max ? -1 : 0;
}

static void make_parents(const char *path)
{
    char tmp[600];
    char *s;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (s = tmp + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(tmp, 0777);
            *s = '/';
        }
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---- shell words -------------------------------------------------------- */

/* Split a command into words, honouring quotes and \ escapes. */
static int words(char *s, char **w, int max)
{
    int n = 0;
    char *o = s;
    while (*s && n < max) {
        char q = 0;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s)
            break;
        w[n++] = o;
        while (*s && (q || (*s != ' ' && *s != '\t'))) {
            if (!q && (*s == '\'' || *s == '"')) {
                q = *s++;
            } else if (q && *s == q) {
                q = 0;
                s++;
            } else if (*s == '\\' && s[1] && q != '\'') {
                s++;
                *o++ = *s++;
            } else {
                *o++ = *s++;
            }
        }
        if (*s)
            s++;
        *o++ = 0;
    }
    return n;
}

/* ---- commands ------------------------------------------------------------ */

static void start_stream(pi *p, const char *target)
{
    host_path(p, target, p->path, sizeof p->path);
    make_parents(p->path);
    p->fp = fopen(p->path, "wb");
    p->b64 = p->b64n = 0;
    p->mode = STREAM_WAIT;
}

static void cmd_ls(pi *p, char **w, int n)
{
    char hp[600];
    DIR *d;
    struct dirent *e;
    int any = 0;
    host_path(p, n > 1 && w[n - 1][0] != '-' ? w[n - 1] : ".", hp, sizeof hp);
    d = opendir(hp);
    if (!d) {
        struct stat st;
        if (!stat(hp, &st))
            sayf(p, "%s\n", w[n - 1]);
        else
            sayf(p, "ls: cannot access '%s': No such file or directory\n", w[n - 1]);
        return;
    }
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') {
            sayf(p, "%s%s", any ? "  " : "", e->d_name);
            any = 1;
        }
    closedir(d);
    if (any)
        say(p, "\n");
}

/* Run one simple command; returns its exit status. */
static int run(pi *p, char *cmd)
{
    char *w[32], *redir = NULL, *r;
    char hp[600], hp2[1300];
    int n, i = 0, append = 0;
    FILE *saved = NULL;
    int status = 0;

    /* "> file" or ">> file" sends output to a file (echo, cat) */
    if ((r = strchr(cmd, '>'))) {
        append = r[1] == '>';
        *r = 0;
        redir = r + 1 + append;
        while (*redir == ' ')
            redir++;
    }
    n = words(cmd, w, 32);
    if (!n)
        return 0;
    if (redir) {
        char *rw[2];
        if (words(redir, rw, 2) < 1)
            return 1;
        if (!strcmp(w[0], "nextpi-file_stream")) {
            start_stream(p, rw[0]);
            return 0;
        }
        host_path(p, rw[0], hp, sizeof hp);
        saved = fopen(hp, append ? "ab" : "wb");
        if (!saved) {
            sayf(p, "-bash: %s: No such file or directory\n", rw[0]);
            return 1;
        }
    }

    if (!strcmp(w[0], "nextpi-admin_version")) {
        say(p, VERSION "\n");
    } else if (!strcmp(w[0], "nextpi-file_receive")) {
        p->hdr_len = 0;
        p->mode = RECV_HDR;
        say(p, "OK\n");
    } else if (!strcmp(w[0], "nextpi-file_transmit")) {
        p->hdr_len = 0;
        p->mode = TX_NAME;
        say(p, "OK\n");
    } else if (!strcmp(w[0], "echo")) {
        char text[1024];
        size_t k = 0;
        int nl = 1;
        i = 1;
        if (n > 1 && !strcmp(w[1], "-n")) {
            nl = 0;
            i = 2;
        }
        text[0] = 0;
        for (; i < n; i++)
            k += (size_t)snprintf(text + k, sizeof text - k, "%s%s", k ? " " : "", w[i]);
        if (saved)
            fprintf(saved, "%s%s", text, nl ? "\n" : "");
        else
            sayf(p, "%s%s", text, nl ? "\n" : "");
    } else if (!strcmp(w[0], "cat")) {
        for (i = 1; i < n; i++) {
            FILE *fp;
            int c;
            host_path(p, w[i], hp, sizeof hp);
            fp = fopen(hp, "rb");
            if (!fp) {
                sayf(p, "cat: %s: No such file or directory\n", w[i]);
                status = 1;
                continue;
            }
            while ((c = fgetc(fp)) != EOF) {
                char ch[2] = { (char)c, 0 };
                if (saved)
                    fputc(c, saved);
                else
                    say(p, ch);
            }
            fclose(fp);
        }
    } else if (!strcmp(w[0], "ls")) {
        cmd_ls(p, w, n);
    } else if (!strcmp(w[0], "rm")) {
        for (i = 1; i < n; i++)
            if (w[i][0] != '-' && !host_path(p, w[i], hp, sizeof hp))
                remove(hp);
    } else if (!strcmp(w[0], "mkdir")) {
        for (i = 1; i < n; i++)
            if (w[i][0] != '-' && !host_path(p, w[i], hp, sizeof hp)) {
                make_parents(hp);
                mkdir(hp, 0777);
            }
    } else if ((!strcmp(w[0], "mv") || !strcmp(w[0], "cp")) && n >= 3) {
        struct stat st;
        host_path(p, w[n - 2], hp, sizeof hp);
        host_path(p, w[n - 1], hp2, sizeof hp2);
        if (!stat(hp2, &st) && S_ISDIR(st.st_mode)) {
            size_t l = strlen(hp2);
            snprintf(hp2 + l, sizeof hp2 - l, "/%s", base_name(hp));
        }
        if (w[0][0] == 'm') {
            status = rename(hp, hp2) ? 1 : 0;
        } else {
            FILE *a = fopen(hp, "rb"), *b = a ? fopen(hp2, "wb") : NULL;
            int c;
            while (a && b && (c = fgetc(a)) != EOF)
                fputc(c, b);
            status = !(a && b);
            if (a)
                fclose(a);
            if (b)
                fclose(b);
        }
        if (status)
            sayf(p, "%s: cannot stat '%s': No such file or directory\n", w[0], w[n - 2]);
    } else if (!strcmp(w[0], "cd")) {
        struct stat st;
        char np[600];
        if (n < 2)
            snprintf(np, sizeof np, "/root");
        else if (w[1][0] == '/')
            snprintf(np, sizeof np, "%s", w[1]);
        else
            snprintf(np, sizeof np, "%s/%s", strcmp(p->cwd, "/") ? p->cwd : "", w[1]);
        host_path(p, np, hp, sizeof hp);
        if (!stat(hp, &st) && S_ISDIR(st.st_mode)) {
            const char *rel = hp + strlen(p->root);
            snprintf(p->cwd, sizeof p->cwd, "%s", *rel ? rel : "/");
        } else {
            sayf(p, "-bash: cd: %s: No such file or directory\n", n > 1 ? w[1] : "");
            status = 1;
        }
    } else if (!strcmp(w[0], "pwd")) {
        sayf(p, "%s\n", p->cwd);
    } else if (!strcmp(w[0], "true") || !strcmp(w[0], "chmod") || !strcmp(w[0], "killall")
               || !strcmp(w[0], "sync") || !strcmp(w[0], "clear")) {
        /* nothing to do here */
    } else if (!strcmp(w[0], "false")) {
        status = 1;
    } else if (!strcmp(w[0], "gunzip")) {
        sayf(p, "gunzip: not available in this emulation\n");
        status = 1;
    } else if (!strncmp(w[0], "nextpi-", 7)) {
        sayf(p, "%s: not available in this emulation\n", w[0]);
        status = 1;
    } else {
        sayf(p, "-bash: %s: command not found\n", w[0]);
        status = 127;
    }
    if (saved)
        fclose(saved);
    return status;
}

/* A command line: commands joined by ;, && and ||, with & ignored. */
static void line(pi *p, char *s)
{
    int last = 0;
    char op = ';';
    while (*s) {
        char *e = s, next = 0;
        int q = 0;
        for (; *e; e++) {
            if (*e == '\'' || *e == '"')
                q = !q;
            if (q)
                continue;
            if (*e == ';' || (*e == '&' && e[1] == '&') || (*e == '|' && e[1] == '|')) {
                next = *e;
                *e = 0;
                if (next != ';')
                    *++e = 0;
                e++;
                break;
            }
        }
        {
            size_t l = strlen(s);
            while (l && (s[l - 1] == ' ' || s[l - 1] == '&'))
                s[--l] = 0;
        }
        if (op == ';' || (op == '&' && !last) || (op == '|' && last))
            last = run(p, s);
        if (p->mode != SHELL)
            return;             /* a transfer has taken over the line */
        op = next ? next : ';';
        s = e;
        if (!next)
            break;
    }
}

/* ---- transfers ---------------------------------------------------------- */

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

static void end_transfer(pi *p)
{
    if (p->fp)
        fclose(p->fp);
    p->fp = NULL;
    p->mode = SHELL;
    prompt(p);
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void stream_byte(pi *p, uint8_t b)
{
    if (p->mode == STREAM_WAIT) {
        /* skip to the end of the "begin-base64 ..." line */
        if (b == '\n')
            p->mode = STREAM, p->line_len = 0;
        return;
    }
    if (b == '\n') {
        if (p->line_len == 0) {         /* an empty line ends the file */
            say(p, "\n");
            end_transfer(p);
        }
        p->line_len = 0;
        return;
    }
    if (b == '\r')
        return;
    p->line_len++;
    if (b == '=') {
        /* padding ends a group early: 2 symbols make 1 byte, 3 make 2 */
        if (p->b64n >= 2 && p->fp) {
            uint32_t v = p->b64 << (6 * (4 - p->b64n));
            uint8_t o[2] = { (uint8_t)(v >> 16), (uint8_t)(v >> 8) };
            fwrite(o, 1, p->b64n - 1, p->fp);
        }
        p->b64 = p->b64n = 0;
        return;
    }
    {
        int v = b64val(b);
        if (v < 0)
            return;
        p->b64 = p->b64 << 6 | (uint32_t)v;
        if (++p->b64n == 4) {
            uint8_t o[3] = { (uint8_t)(p->b64 >> 16), (uint8_t)(p->b64 >> 8), (uint8_t)p->b64 };
            if (p->fp)
                fwrite(o, 1, 3, p->fp);
            p->b64 = p->b64n = 0;
        }
    }
}

/* Header: size, blocks, remainder, 0-terminated name. */
static int header_done(pi *p)
{
    return p->hdr_len > 10 && p->hdr[p->hdr_len - 1] == 0;
}

static void recv_byte(pi *p, uint8_t b)
{
    if (p->mode == RECV_HDR) {
        if (p->hdr_len < (int)sizeof p->hdr)
            p->hdr[p->hdr_len++] = b;
        if (!header_done(p))
            return;
        p->size = be32(p->hdr);
        p->blocks = be32(p->hdr + 4);
        p->rem = (uint16_t)(p->hdr[8] << 8 | p->hdr[9]);
        snprintf(p->line, sizeof p->line, "/ram/%s", base_name((char *)p->hdr + 10));
        host_path(p, p->line, p->path, sizeof p->path);
        make_parents(p->path);
        p->fp = fopen(p->path, "wb");
        p->buf_len = 0;             /* bytes stored so far */
        p->got = 0;
        p->sum = 0;
        p->mode = RECV_DATA;
        say(p, "OK\n");
        if (!p->size)
            end_transfer(p);
        return;
    }
    {
        uint32_t left = p->size - p->buf_len;
        uint32_t want = left < BLOCK ? left : BLOCK;
        if (p->got < want) {
            p->buf[p->got++] = b;
            p->sum = (uint8_t)(p->sum + b);
            return;
        }
        /* the checksum byte */
        if (b == p->sum) {
            if (p->fp)
                fwrite(p->buf, 1, want, p->fp);
            p->buf_len += want;
            say(p, "OK\n");
        } else {
            say(p, "ER\n");             /* the Next sends it again */
        }
        p->got = 0;
        p->sum = 0;
        if (p->buf_len >= p->size)
            end_transfer(p);
    }
}

static void send_block(pi *p)
{
    uint32_t want = p->block < p->blocks ? BLOCK : p->rem;
    uint8_t sum = 0;
    uint32_t i;
    p->buf_len = p->fp ? (uint32_t)fread(p->buf, 1, want, p->fp) : 0;
    while (p->buf_len < want)
        p->buf[p->buf_len++] = 0;
    for (i = 0; i < want; i++)
        sum = (uint8_t)(sum + p->buf[i]);
    out_raw(p, p->buf, (int)want);
    out_raw(p, &sum, 1);
}

static void tx_byte(pi *p, uint8_t b)
{
    if (p->mode == TX_NAME) {
        uint8_t h[10];
        struct stat st;
        const char *name;
        if (p->hdr_len < (int)sizeof p->hdr - 1)
            p->hdr[p->hdr_len++] = b;
        if (p->hdr_len < 2 || b != 0)
            return;
        p->hdr[p->hdr_len] = 0;
        name = (char *)p->hdr + 1;      /* after the leading 0 */
        host_path(p, name, p->path, sizeof p->path);
        p->fp = fopen(p->path, "rb");
        p->size = p->fp && !stat(p->path, &st) ? (uint32_t)st.st_size : 0;
        p->blocks = p->size / BLOCK;
        p->rem = (uint16_t)(p->size % BLOCK);
        h[0] = (uint8_t)(p->size >> 24); h[1] = (uint8_t)(p->size >> 16);
        h[2] = (uint8_t)(p->size >> 8); h[3] = (uint8_t)p->size;
        h[4] = (uint8_t)(p->blocks >> 24); h[5] = (uint8_t)(p->blocks >> 16);
        h[6] = (uint8_t)(p->blocks >> 8); h[7] = (uint8_t)p->blocks;
        h[8] = (uint8_t)(p->rem >> 8); h[9] = (uint8_t)p->rem;
        out_raw(p, h, 10);
        p->block = 0;
        p->buf_len = 0;
        p->mode = TX_ACK;
        return;
    }
    if (b == '?' && p->buf_len) {       /* bad sum: that block again */
        if (p->fp)
            fseek(p->fp, -(long)p->buf_len, SEEK_CUR);
        p->block--;
    } else if (b != '!') {
        return;
    }
    if (p->block < p->blocks || (p->block == p->blocks && p->rem)) {
        send_block(p);
        p->block++;
    }
    if (p->block > p->blocks || (p->block == p->blocks && !p->rem)) {
        /* all sent: the shell is back (a '?' after this is too late) */
        if (p->fp)
            fclose(p->fp);
        p->fp = NULL;
        p->mode = SHELL;
        prompt(p);
    }
}

/* ---- the serial line ------------------------------------------------------ */

static void put(zx_serial *s, uint8_t b)
{
    pi *p = (pi *)s;

    switch (p->mode) {
    case STREAM_WAIT:
    case STREAM:
        stream_byte(p, b);
        return;
    case RECV_HDR:
    case RECV_DATA:
        recv_byte(p, b);
        return;
    case TX_NAME:
    case TX_ACK:
        tx_byte(p, b);
        return;
    default:
        break;
    }

    if (b == 3) {                       /* ^C */
        say(p, "^C\n");
        p->line_len = 0;
        prompt(p);
    } else if (b == 4) {                /* ^D: the supervisor restarts */
        if (!p->line_len) {
            say(p, "\n" BANNER "\n");
            prompt(p);
        }
    } else if (b == '\r' || b == '\n') {
        say(p, "\n");
        p->line[p->line_len] = 0;
        p->line_len = 0;
        line(p, p->line);
        if (p->mode == SHELL)
            prompt(p);
    } else if (b == 8 || b == 127) {
        if (p->line_len) {
            p->line_len--;
            say(p, "\b \b");
        }
    } else if (b >= 32 && p->line_len < LINE_MAX - 1) {
        p->line[p->line_len++] = (char)b;
        out_raw(p, &b, 1);              /* tty echo */
    }
}

static int avail(zx_serial *s)
{
    return ((pi *)s)->out_len > 0;
}

static uint8_t get(zx_serial *s)
{
    pi *p = (pi *)s;
    uint8_t b;
    if (!p->out_len)
        return 0xff;
    b = p->out[p->out_head];
    p->out_head = (p->out_head + 1) % OUT_SIZE;
    p->out_len--;
    return b;
}

static void pi_close(zx_serial *s)
{
    pi *p = (pi *)s;
    if (p->fp)
        fclose(p->fp);
    free(p->root);
    free(p);
}

zx_serial *zx_nextpi_new(const char *folder)
{
    pi *p = calloc(1, sizeof *p);
    char hp[600];
    size_t n;
    if (!p)
        return NULL;
    n = strlen(folder);
    p->root = malloc(n + 1);
    if (!p->root) {
        free(p);
        return NULL;
    }
    memcpy(p->root, folder, n + 1);
    while (n > 1 && p->root[n - 1] == '/')
        p->root[--n] = 0;
    mkdir(p->root, 0777);
    snprintf(hp, sizeof hp, "%s/ram", p->root);
    mkdir(hp, 0777);
    snprintf(hp, sizeof hp, "%s/NextPi", p->root);
    mkdir(hp, 0777);
    snprintf(p->cwd, sizeof p->cwd, "/root");
    snprintf(hp, sizeof hp, "%s/root", p->root);
    mkdir(hp, 0777);
    p->s.baud = 115200;
    p->s.put = put;
    p->s.avail = avail;
    p->s.get = get;
    p->s.close = pi_close;
    return &p->s;
}
