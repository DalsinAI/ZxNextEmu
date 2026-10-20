/*
 * Serial devices: whatever sits on the other end of a machine's UART
 * (the Next's ESP8266 wifi module and Raspberry Pi, a modem, a printer).
 *
 * Bytes move whole: the UART asks the device for its next byte when its
 * receiver is free, and hands it each byte the machine sends. The device
 * runs at its own baud rate; if that does not match the UART's, both
 * sides see garbage, as with real hardware.
 */
#ifndef ZX_SERIAL_H
#define ZX_SERIAL_H

#include <stdint.h>

typedef struct zx_serial zx_serial;

struct zx_serial {
    uint32_t baud;                              /* the device's line speed */
    void (*put)(zx_serial *s, uint8_t b);       /* byte from the machine */
    int (*avail)(zx_serial *s);                 /* a byte is ready for the machine */
    uint8_t (*get)(zx_serial *s);               /* take that byte */
    void (*poll)(zx_serial *s, uint64_t usec);  /* once per frame: time since power on */
    void (*close)(zx_serial *s);
};

#endif
