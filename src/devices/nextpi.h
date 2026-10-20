/*
 * The Next's Raspberry Pi Zero running NextPi, on a serial line.
 */
#ifndef ZX_NEXTPI_H
#define ZX_NEXTPI_H

#include "serial.h"

/* folder holds the Pi's files (/ram, /NextPi ...); it is created if needed. */
zx_serial *zx_nextpi_new(const char *folder);

#endif
