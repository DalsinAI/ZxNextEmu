/*
 * esp: the Next's UART 0 and the ESP8266 module, driven through the ports
 * as Next software does: AT commands, a baud rate change, and a TCP
 * connection to an echo server on 127.0.0.1:port.
 *
 *   esp romdir port
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/zx/zx.h"
#include "../src/next/next.h"
#include "../src/devices/esp8266.h"
#include "../src/frontend/net_posix.h"

static zx m;
static int failures;

static void send_str(const char *s)
{
    zxn *n = m.next;
    while (*s) {
        while (nx_io_read(n, 0x133b) & 2)       /* Tx FIFO full */
            zx_run_frame(&m);
        nx_io_write(n, 0x133b, (uint8_t)*s++);
    }
}

/* Collect what comes back over a number of frames. */
static void receive(char *buf, int max, int frames)
{
    zxn *n = m.next;
    int len = 0;
    while (frames-- > 0) {
        zx_run_frame(&m);
        while ((nx_io_read(n, 0x133b) & 1) && len < max - 1)
            buf[len++] = (char)nx_io_read(n, 0x143b);
    }
    buf[len] = 0;
}

static void expect(const char *what, const char *got, const char *want)
{
    if (!strstr(got, want)) {
        printf("esp: FAIL: %s: wanted \"%s\" in:\n%s\n", what, want, got);
        failures++;
    }
}

static void set_baud(uint32_t baud)
{
    zxn *n = m.next;
    uint32_t p = 28000000 / baud;
    nx_io_write(n, 0x153b, (uint8_t)(0x10 | (p >> 14 & 7)));
    nx_io_write(n, 0x143b, (uint8_t)(p & 0x7f));
    nx_io_write(n, 0x143b, (uint8_t)(0x80 | (p >> 7 & 0x7f)));
}

int main(int argc, char **argv)
{
    char buf[8192], cmd[64];
    zx_model model;
    zx_net *net = zx_net_posix_new();

    if (argc != 3)
        return 2;
    zx_model_from_name("next", &model);
    if (zx_init(&m, model, argv[1]))
        return 1;
    nx_attach_serial(&m, 0, zx_esp8266_new(net));
    zx_run_frame(&m);

    send_str("AT\r\n");
    receive(buf, sizeof buf, 5);
    expect("AT", buf, "AT\r\n\r\nOK\r\n");

    send_str("ATE0\r\nAT+GMR\r\n");
    receive(buf, sizeof buf, 5);
    expect("AT+GMR", buf, "AT version:");

    /* both ends must agree on the speed */
    send_str("AT+UART_CUR=2000000,8,1,0,0\r\n");
    receive(buf, sizeof buf, 3);
    expect("AT+UART_CUR", buf, "OK");
    send_str("AT\r\n");
    receive(buf, sizeof buf, 3);
    if (strstr(buf, "OK"))
        printf("esp: FAIL: understood at the wrong speed\n"), failures++;
    set_baud(2000000);
    send_str("\r\nAT\r\n");                 /* flush the garbled line first */
    receive(buf, sizeof buf, 3);
    expect("AT at 2000000", buf, "OK");

    send_str("AT+CWJAP=\"x\",\"y\"\r\nAT+CIFSR\r\n");
    receive(buf, sizeof buf, 5);
    expect("AT+CWJAP", buf, "WIFI GOT IP");
    expect("AT+CIFSR", buf, "+CIFSR:STAIP");

    sprintf(cmd, "AT+CIPSTART=\"TCP\",\"127.0.0.1\",%s\r\n", argv[2]);
    send_str(cmd);
    receive(buf, sizeof buf, 25);
    expect("AT+CIPSTART", buf, "CONNECT\r\n\r\nOK");

    send_str("AT+CIPSEND=13\r\n");
    receive(buf, sizeof buf, 2);
    expect("AT+CIPSEND", buf, ">");
    send_str("Hello, Next!\n");
    receive(buf, sizeof buf, 25);
    expect("SEND OK", buf, "SEND OK");
    expect("+IPD", buf, "+IPD,13:Hello, Next!\n");

    send_str("AT+CIPCLOSE\r\n");
    receive(buf, sizeof buf, 3);
    expect("AT+CIPCLOSE", buf, "CLOSED");

    zx_free(&m);
    zx_net_posix_free(net);
    if (!failures)
        printf("esp: all checks passed\n");
    return failures != 0;
}
