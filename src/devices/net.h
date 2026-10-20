/*
 * Network connections for emulated devices (the ESP8266 wifi module).
 *
 * The host supplies this: plain sockets in a native build (net_posix.c),
 * or a WebSocket relay in a browser. Every call returns at once; a
 * connection reports its progress through state().
 */
#ifndef ZX_NET_H
#define ZX_NET_H

#include <stdint.h>

enum { ZX_NET_CONNECTING = 0, ZX_NET_OPEN = 1, ZX_NET_CLOSED = -1 };

typedef struct zx_net zx_net;

struct zx_net {
    /* TCP (udp 0) or UDP (udp 1) to host:port; a handle >= 0, or -1 */
    int (*open)(zx_net *net, int udp, const char *host, int port);
    int (*state)(zx_net *net, int h);
    /* bytes taken (maybe fewer than n), or -1 */
    int (*send)(zx_net *net, int h, const uint8_t *p, int n);
    /* bytes read, 0 for none yet, -1 once closed */
    int (*recv)(zx_net *net, int h, uint8_t *p, int max);
    void (*close)(zx_net *net, int h);
    /* dotted IPv4 address of a host name, 0 on success */
    int (*resolve)(zx_net *net, const char *host, char ip[16]);
};

#endif
