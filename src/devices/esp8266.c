/*
 * ESP8266 wifi module running Espressif's AT firmware, as fitted to the
 * Spectrum Next on UART 0.
 *
 * The module answers AT commands the way ESP-AT 1.7 does, and its TCP and
 * UDP connections go out through the host's network (zx_net): sockets in a
 * native build, a WebSocket relay in a browser. Joining a network always
 * works and there is one access point, "ZXCradle", since the host is
 * already online.
 *
 * Handled: AT, ATE0/1, AT+RST, AT+RESTORE, AT+GMR, AT+CWMODE, AT+CWJAP,
 * AT+CWQAP, AT+CWLAP, AT+CWAUTOCONN, AT+CWDHCP, AT+CIFSR, AT+CIPSTA,
 * AT+CIPSTAMAC, AT+CIPMUX, AT+CIPMODE (normal and transparent),
 * AT+CIPSTART, AT+CIPSEND, AT+CIPCLOSE, AT+CIPSTATUS, AT+CIPDOMAIN,
 * AT+CIPSTO, AT+CIPDINFO, AT+UART(_CUR/_DEF), AT+PING, AT+SLEEP, AT+SYSRAM,
 * AT+CIPSERVER (refused: no listening). The _CUR/_DEF forms are accepted
 * wherever the firmware has them. Anything else answers ERROR.
 */
#include "esp8266.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINKS     5
#define OUT_SIZE  8192
#define IPD_MAX   1460
#define LINE_MAX  600

typedef struct link {
    int h;                      /* zx_net handle, -1 if unused */
    int open;                   /* CONNECT has been reported */
    int udp;
} link;

typedef struct esp {
    zx_serial s;
    zx_net *net;
    char line[LINE_MAX];
    int line_len;
    uint8_t out[OUT_SIZE];
    int out_head, out_len;
    int echo, mux, mode, cwmode, joined, dinfo;
    link link[LINKS];
    int wait_link;              /* CIPSTART waiting for the connection, or -1 */
    int send_link, send_left;   /* CIPSEND taking data */
    int transparent;            /* CIPMODE=1 passthrough is running */
    int plus;                   /* "+++" seen so far in passthrough */
    uint8_t send[2048];
    int send_len;
    uint32_t new_baud;          /* switch once the OK has gone out */
} esp;

static void out_bytes(esp *e, const void *p, int n)
{
    const uint8_t *b = p;
    while (n-- > 0 && e->out_len < OUT_SIZE)
        e->out[(e->out_head + e->out_len++) % OUT_SIZE] = *b++;
}

static void say(esp *e, const char *s)
{
    out_bytes(e, s, (int)strlen(s));
}

static void sayf(esp *e, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    say(e, buf);
}

static void ok(esp *e) { say(e, "\r\nOK\r\n"); }
static void error(esp *e) { say(e, "\r\nERROR\r\n"); }

/* ---- argument parsing ------------------------------------------------ */

/* Split "a,\"b,c\",3" into up to max fields; quotes are removed. */
static int split(char *s, char **f, int max)
{
    int n = 0;
    while (*s && n < max) {
        char *w;
        if (*s == '"') {
            w = ++s;
            while (*s && *s != '"')
                s++;
            if (*s)
                *s++ = 0;
            f[n++] = w;
            while (*s && *s != ',')
                s++;
        } else {
            w = s;
            while (*s && *s != ',')
                s++;
            f[n++] = w;
        }
        if (*s == ',')
            *s++ = 0;
    }
    return n;
}

static int prefix(const char *cmd, const char *name, const char **rest)
{
    size_t n = strlen(name);
    if (strncmp(cmd, name, n))
        return 0;
    /* AT+CWMODE_CUR and _DEF are the same command here */
    if (!strncmp(cmd + n, "_CUR", 4) || !strncmp(cmd + n, "_DEF", 4))
        n += 4;
    if (cmd[n] && cmd[n] != '=' && cmd[n] != '?')
        return 0;
    *rest = cmd + n;
    return 1;
}

/* ---- connections ------------------------------------------------------ */

static void close_link(esp *e, int id, int report)
{
    link *l = &e->link[id];
    if (l->h < 0)
        return;
    e->net->close(e->net, l->h);
    l->h = -1;
    if (report && l->open) {
        if (e->mux)
            sayf(e, "%d,CLOSED\r\n", id);
        else
            say(e, "CLOSED\r\n");
    }
    l->open = 0;
    if (e->transparent && !e->mux)
        e->transparent = 0;
}

static void cip_start(esp *e, char *args)
{
    char *f[6];
    int n = split(args, f, 6), id = 0, k = 0, port, udp;
    if (e->mux) {
        if (n < 1)
            goto bad;
        id = atoi(f[0]);
        k = 1;
    }
    if (n < k + 3 || id < 0 || id >= LINKS)
        goto bad;
    udp = !strcmp(f[k], "UDP");
    if (!udp && strcmp(f[k], "TCP"))
        goto bad;               /* SSL needs a certificate store: not here */
    port = atoi(f[k + 2]);
    if (e->link[id].h >= 0) {
        say(e, "ALREADY CONNECTED\r\n");
        error(e);
        return;
    }
    if (!e->net || (e->link[id].h = e->net->open(e->net, udp, f[k + 1], port)) < 0) {
        e->link[id].h = -1;
        error(e);
        say(e, "CLOSED\r\n");
        return;
    }
    e->link[id].udp = udp;
    e->link[id].open = 0;
    e->wait_link = id;          /* the answer comes when it connects */
    return;
bad:
    error(e);
}

static void cip_send(esp *e, char *args)
{
    char *f[3];
    int n, id = 0, len;
    if (!*args) {               /* AT+CIPSEND alone: passthrough */
        if (e->mode != 1 || e->mux || e->link[0].h < 0) {
            error(e);
            return;
        }
        e->transparent = 1;
        e->plus = 0;
        say(e, "\r\nOK\r\n\r\n>");
        return;
    }
    n = split(args, f, 3);
    if (e->mux) {
        if (n < 2)
            goto bad;
        id = atoi(f[0]);
        len = atoi(f[1]);
    } else {
        len = n ? atoi(f[0]) : 0;
    }
    if (id < 0 || id >= LINKS || len <= 0 || len > (int)sizeof e->send)
        goto bad;
    if (e->link[id].h < 0 || !e->link[id].open) {
        say(e, "link is not valid\r\n");
        goto bad;
    }
    e->send_link = id;
    e->send_left = len;
    e->send_len = 0;
    say(e, "\r\nOK\r\n> ");
    return;
bad:
    error(e);
}

static int link_send(esp *e, int id, const uint8_t *p, int n)
{
    int done = 0;
    while (done < n) {
        int w = e->net->send(e->net, e->link[id].h, p + done, n - done);
        if (w < 0)
            return -1;
        if (w == 0)
            break;              /* the host would block; drop the rest */
        done += w;
    }
    return done;
}

/* ---- commands ----------------------------------------------------------- */

static void command(esp *e, char *cmd)
{
    const char *r;
    char *p;
    for (p = cmd; *p && *p != '=' && *p != '"'; p++)
        *p = (char)toupper((unsigned char)*p);

    if (e->wait_link >= 0) {
        say(e, "busy p...\r\n");
        return;
    }
    if (!strcmp(cmd, "AT")) {
        ok(e);
    } else if (!strcmp(cmd, "ATE0") || !strcmp(cmd, "ATE1")) {
        e->echo = cmd[3] == '1';
        ok(e);
    } else if (!strcmp(cmd, "AT+RST") || !strcmp(cmd, "AT+RESTORE")) {
        int i;
        ok(e);
        for (i = 0; i < LINKS; i++)
            close_link(e, i, 0);
        e->mux = e->mode = 0;
        e->echo = 1;
        if (cmd[3] == 'E') {
            e->joined = 0;
            e->new_baud = 115200;
        }
        say(e, "\r\nready\r\n");
        if (e->joined)
            say(e, "WIFI CONNECTED\r\nWIFI GOT IP\r\n");
    } else if (!strcmp(cmd, "AT+GMR")) {
        say(e, "AT version:1.7.4.0(May 11 2020 19:13:04)\r\n"
               "SDK version:3.0.4(9532ceb)\r\n"
               "compile time:Oct 20 2026 00:00:00\r\n"
               "Bin version(Wroom 02):1.7.4\r\n");
        ok(e);
    } else if (prefix(cmd, "AT+CWMODE", &r)) {
        if (*r == '?')
            sayf(e, "+CWMODE:%d\r\n", e->cwmode);
        else if (*r == '=' && r[1] >= '1' && r[1] <= '3')
            e->cwmode = r[1] - '0';
        else {
            error(e);
            return;
        }
        ok(e);
    } else if (prefix(cmd, "AT+CWJAP", &r)) {
        if (*r == '?') {
            if (e->joined)
                say(e, "+CWJAP:\"ZXCradle\",\"5a:58:43:52:41:44\",6,-42\r\n");
            else
                say(e, "No AP\r\n");
            ok(e);
        } else if (*r == '=') {
            e->joined = 1;
            say(e, "WIFI CONNECTED\r\nWIFI GOT IP\r\n");
            ok(e);
        } else {
            error(e);
        }
    } else if (!strcmp(cmd, "AT+CWQAP")) {
        int i;
        for (i = 0; i < LINKS; i++)
            close_link(e, i, 1);
        e->joined = 0;
        ok(e);
        say(e, "WIFI DISCONNECT\r\n");
    } else if (!strcmp(cmd, "AT+CWLAP")) {
        say(e, "+CWLAP:(3,\"ZXCradle\",-42,\"5a:58:43:52:41:44\",6,0,0)\r\n");
        ok(e);
    } else if (prefix(cmd, "AT+CWAUTOCONN", &r) || prefix(cmd, "AT+CWDHCP", &r)
               || prefix(cmd, "AT+CIPSTO", &r) || prefix(cmd, "AT+SLEEP", &r)
               || prefix(cmd, "AT+CIPRECVMODE", &r) || prefix(cmd, "AT+CIPSSLSIZE", &r)
               || prefix(cmd, "AT+CWHOSTNAME", &r)) {
        if (*r == '?')
            sayf(e, "%.*s:%s\r\n", (int)(strchr(cmd, '?') - cmd - 2), cmd + 2,
                 !strncmp(cmd, "AT+CIPSTO", 9) ? "180" : "0");
        ok(e);
    } else if (!strcmp(cmd, "AT+CIFSR")) {
        say(e, "+CIFSR:STAIP,\"192.168.4.2\"\r\n+CIFSR:STAMAC,\"5a:58:4e:58:00:01\"\r\n");
        ok(e);
    } else if (prefix(cmd, "AT+CIPSTAMAC", &r)) {
        say(e, "+CIPSTAMAC:\"5a:58:4e:58:00:01\"\r\n");
        ok(e);
    } else if (prefix(cmd, "AT+CIPSTA", &r)) {
        say(e, "+CIPSTA:ip:\"192.168.4.2\"\r\n+CIPSTA:gateway:\"192.168.4.1\"\r\n"
               "+CIPSTA:netmask:\"255.255.255.0\"\r\n");
        ok(e);
    } else if (prefix(cmd, "AT+CIPMUX", &r)) {
        if (*r == '?') {
            sayf(e, "+CIPMUX:%d\r\n", e->mux);
        } else if (*r == '=' && (r[1] == '0' || r[1] == '1')) {
            int i, any = 0;
            for (i = 0; i < LINKS; i++)
                any |= e->link[i].h >= 0;
            if (any) {
                say(e, "link is builded\r\n");
                error(e);
                return;
            }
            e->mux = r[1] - '0';
        } else {
            error(e);
            return;
        }
        ok(e);
    } else if (prefix(cmd, "AT+CIPMODE", &r)) {
        if (*r == '?')
            sayf(e, "+CIPMODE:%d\r\n", e->mode);
        else if (*r == '=' && (r[1] == '0' || (r[1] == '1' && !e->mux)))
            e->mode = r[1] - '0';
        else {
            error(e);
            return;
        }
        ok(e);
    } else if (prefix(cmd, "AT+CIPDINFO", &r)) {
        if (*r == '=')
            e->dinfo = r[1] == '1';
        ok(e);
    } else if (prefix(cmd, "AT+CIPSTART", &r) && *r == '=') {
        cip_start(e, (char *)r + 1);
    } else if (prefix(cmd, "AT+CIPSEND", &r)) {
        cip_send(e, *r == '=' ? (char *)r + 1 : (char *)"");
    } else if (prefix(cmd, "AT+CIPCLOSE", &r)) {
        int id = *r == '=' ? atoi(r + 1) : 0, i;
        if (id == 5) {
            for (i = 0; i < LINKS; i++)
                close_link(e, i, 1);
        } else if (id < 0 || id >= LINKS || e->link[id].h < 0) {
            error(e);
            return;
        } else {
            close_link(e, id, 1);
        }
        ok(e);
    } else if (!strcmp(cmd, "AT+CIPSTATUS")) {
        int i, any = 0;
        for (i = 0; i < LINKS; i++)
            any |= e->link[i].open;
        sayf(e, "STATUS:%d\r\n", !e->joined ? 5 : any ? 3 : 2);
        for (i = 0; i < LINKS; i++)
            if (e->link[i].open)
                sayf(e, "+CIPSTATUS:%d,\"%s\",\"0.0.0.0\",0,0,0\r\n", i, e->link[i].udp ? "UDP" : "TCP");
        ok(e);
    } else if (prefix(cmd, "AT+CIPDOMAIN", &r) && *r == '=') {
        char *f[1], ip[16];
        if (split((char *)r + 1, f, 1) == 1 && e->net && e->net->resolve
            && !e->net->resolve(e->net, f[0], ip)) {
            sayf(e, "+CIPDOMAIN:%s\r\n", ip);
            ok(e);
        } else {
            say(e, "DNS Fail\r\n");
            error(e);
        }
    } else if (prefix(cmd, "AT+PING", &r) && *r == '=') {
        say(e, "+5\r\n");
        ok(e);
    } else if (prefix(cmd, "AT+UART", &r)) {
        if (*r == '?') {
            sayf(e, "+UART:%u,8,1,0,0\r\n", (unsigned)e->s.baud);
            ok(e);
        } else if (*r == '=' && atol(r + 1) >= 300 && atol(r + 1) <= 4608000) {
            e->new_baud = (uint32_t)atol(r + 1);
            ok(e);
        } else {
            error(e);
        }
    } else if (prefix(cmd, "AT+SYSRAM", &r)) {
        say(e, "+SYSRAM:46220\r\n");
        ok(e);
    } else {
        error(e);
    }
}

/* ---- the serial line ---------------------------------------------------- */

static void put(zx_serial *s, uint8_t b)
{
    esp *e = (esp *)s;

    if (e->transparent) {
        /* "+++" on its own ends passthrough */
        if (b == '+' && e->plus < 3) {
            if (++e->plus == 3) {
                e->transparent = 0;
                e->plus = 0;
            }
            return;
        }
        if (e->plus) {
            static const uint8_t pluses[3] = { '+', '+', '+' };
            link_send(e, 0, pluses, e->plus);
            e->plus = 0;
        }
        if (e->link[0].h >= 0)
            link_send(e, 0, &b, 1);
        return;
    }
    if (e->send_left) {
        e->send[e->send_len++] = b;
        if (--e->send_left == 0) {
            int ok_ = e->link[e->send_link].h >= 0
                && link_send(e, e->send_link, e->send, e->send_len) == e->send_len;
            sayf(e, "\r\nRecv %d bytes\r\n", e->send_len);
            say(e, ok_ ? "\r\nSEND OK\r\n" : "\r\nSEND FAIL\r\n");
        }
        return;
    }
    if (e->echo)
        out_bytes(e, &b, 1);
    if (b == '\n') {
        while (e->line_len && (e->line[e->line_len - 1] == '\r' || e->line[e->line_len - 1] == ' '))
            e->line_len--;
        e->line[e->line_len] = 0;
        if (e->line_len)
            command(e, e->line);
        e->line_len = 0;
    } else if (e->line_len < LINE_MAX - 1) {
        e->line[e->line_len++] = (char)b;
    }
}

static int avail(zx_serial *s)
{
    return ((esp *)s)->out_len > 0;
}

static uint8_t get(zx_serial *s)
{
    esp *e = (esp *)s;
    uint8_t b;
    if (!e->out_len)
        return 0xff;
    b = e->out[e->out_head];
    e->out_head = (e->out_head + 1) % OUT_SIZE;
    e->out_len--;
    if (!e->out_len && e->new_baud) {
        e->s.baud = e->new_baud;
        e->new_baud = 0;
    }
    return b;
}

static void poll(zx_serial *s, uint64_t usec)
{
    esp *e = (esp *)s;
    int i;
    (void)usec;
    if (!e->out_len && e->new_baud) {
        e->s.baud = e->new_baud;
        e->new_baud = 0;
    }
    if (!e->net)
        return;
    for (i = 0; i < LINKS; i++) {
        link *l = &e->link[i];
        int st;
        if (l->h < 0)
            continue;
        st = e->net->state(e->net, l->h);
        if (!l->open) {
            if (st == ZX_NET_OPEN) {
                l->open = 1;
                if (e->mux)
                    sayf(e, "%d,CONNECT\r\n", i);
                else
                    say(e, "CONNECT\r\n");
                if (e->wait_link == i) {
                    ok(e);
                    e->wait_link = -1;
                }
            } else if (st == ZX_NET_CLOSED) {
                e->net->close(e->net, l->h);
                l->h = -1;
                if (e->wait_link == i) {
                    error(e);
                    say(e, "CLOSED\r\n");
                    e->wait_link = -1;
                }
            }
            continue;
        }
        /* data in: only as much as the line can take */
        while (l->h >= 0 && OUT_SIZE - e->out_len > IPD_MAX + 32) {
            uint8_t buf[IPD_MAX];
            int n = e->net->recv(e->net, l->h, buf, IPD_MAX);
            if (n < 0) {
                close_link(e, i, 1);
                break;
            }
            if (n == 0)
                break;
            if (e->transparent && !e->mux) {
                out_bytes(e, buf, n);
                continue;
            }
            if (e->mux)
                sayf(e, "\r\n+IPD,%d,%d:", i, n);
            else
                sayf(e, "\r\n+IPD,%d:", n);
            out_bytes(e, buf, n);
        }
    }
}

static void esp_close(zx_serial *s)
{
    esp *e = (esp *)s;
    int i;
    for (i = 0; i < LINKS; i++)
        close_link(e, i, 0);
    free(e);
}

zx_serial *zx_esp8266_new(zx_net *net)
{
    esp *e = calloc(1, sizeof *e);
    int i;
    if (!e)
        return NULL;
    e->net = net;
    e->echo = 1;
    e->cwmode = 1;
    e->joined = 1;              /* it remembers the last network, like a real one */
    e->wait_link = -1;
    for (i = 0; i < LINKS; i++)
        e->link[i].h = -1;
    e->s.baud = 115200;
    e->s.put = put;
    e->s.avail = avail;
    e->s.get = get;
    e->s.poll = poll;
    e->s.close = esp_close;
    return &e->s;
}
