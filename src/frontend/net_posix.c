/*
 * zx_net over POSIX sockets, for native builds. Connections are
 * non-blocking; name lookups block (they are quick and rare).
 */
#define _POSIX_C_SOURCE 200809L
#include "net_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MAX_CONN 16

typedef struct pnet {
    zx_net net;
    int fd[MAX_CONN];
    int state[MAX_CONN];
} pnet;

static int p_open(zx_net *net, int udp, const char *host, int port)
{
    pnet *p = (pnet *)net;
    struct addrinfo hints, *res;
    char ps[16];
    int h, fd;
    for (h = 0; h < MAX_CONN && p->fd[h] >= 0; h++)
        ;
    if (h == MAX_CONN)
        return -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    sprintf(ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res))
        return -1;
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (connect(fd, res->ai_addr, res->ai_addrlen) && errno != EINPROGRESS) {
        freeaddrinfo(res);
        close(fd);
        return -1;
    }
    freeaddrinfo(res);
    p->fd[h] = fd;
    p->state[h] = udp ? ZX_NET_OPEN : ZX_NET_CONNECTING;
    return h;
}

static int p_state(zx_net *net, int h)
{
    pnet *p = (pnet *)net;
    if (h < 0 || h >= MAX_CONN || p->fd[h] < 0)
        return ZX_NET_CLOSED;
    if (p->state[h] == ZX_NET_CONNECTING) {
        struct pollfd pf;
        pf.fd = p->fd[h];
        pf.events = POLLOUT;
        if (poll(&pf, 1, 0) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(p->fd[h], SOL_SOCKET, SO_ERROR, &err, &len);
            p->state[h] = err ? ZX_NET_CLOSED : ZX_NET_OPEN;
        }
    }
    return p->state[h];
}

static int p_send(zx_net *net, int h, const uint8_t *buf, int n)
{
    pnet *p = (pnet *)net;
    ssize_t w;
    if (p_state(net, h) != ZX_NET_OPEN)
        return -1;
    w = send(p->fd[h], buf, (size_t)n, 0);
    if (w < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    return (int)w;
}

static int p_recv(zx_net *net, int h, uint8_t *buf, int max)
{
    pnet *p = (pnet *)net;
    ssize_t r;
    int st = p_state(net, h);
    if (st == ZX_NET_CONNECTING)
        return 0;
    if (st != ZX_NET_OPEN)
        return -1;
    r = recv(p->fd[h], buf, (size_t)max, 0);
    if (r < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    if (r == 0) {
        p->state[h] = ZX_NET_CLOSED;
        return -1;
    }
    return (int)r;
}

static void p_close(zx_net *net, int h)
{
    pnet *p = (pnet *)net;
    if (h < 0 || h >= MAX_CONN || p->fd[h] < 0)
        return;
    close(p->fd[h]);
    p->fd[h] = -1;
}

static int p_resolve(zx_net *net, const char *host, char ip[16])
{
    struct addrinfo hints, *res;
    (void)net;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &res))
        return -1;
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, 16);
    freeaddrinfo(res);
    return 0;
}

zx_net *zx_net_posix_new(void)
{
    pnet *p = calloc(1, sizeof *p);
    int i;
    if (!p)
        return NULL;
    for (i = 0; i < MAX_CONN; i++)
        p->fd[i] = -1;
    p->net.open = p_open;
    p->net.state = p_state;
    p->net.send = p_send;
    p->net.recv = p_recv;
    p->net.close = p_close;
    p->net.resolve = p_resolve;
    return &p->net;
}

void zx_net_posix_free(zx_net *net)
{
    pnet *p = (pnet *)net;
    int i;
    if (!p)
        return;
    for (i = 0; i < MAX_CONN; i++)
        p_close(net, i);
    free(p);
}
