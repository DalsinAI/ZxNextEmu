#ifndef ZX_NET_POSIX_H
#define ZX_NET_POSIX_H

#include "../devices/net.h"

zx_net *zx_net_posix_new(void);
void zx_net_posix_free(zx_net *net);

#endif
