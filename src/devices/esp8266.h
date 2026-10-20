/*
 * ESP8266 wifi module with the AT firmware, on a serial line.
 */
#ifndef ZX_ESP8266_H
#define ZX_ESP8266_H

#include "serial.h"
#include "net.h"

/* net may be NULL: the module then answers but cannot connect anywhere. */
zx_serial *zx_esp8266_new(zx_net *net);

#endif
