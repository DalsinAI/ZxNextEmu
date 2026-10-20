/*
 * The Next's two UARTs (serial/uart.vhd): UART 0 goes to the ESP8266 wifi
 * module, UART 1 to the Raspberry Pi. They share four ports:
 *
 *   0x143B  read: next received byte    write: prescaler bits 6..0 / 13..7
 *   0x153B  select UART (bit 6), prescaler bits 16..14 (bit 4 set)
 *   0x163B  framing: fifo reset, break, flow control, bits, parity, stop
 *   0x133B  read: status                write: byte to send
 *
 * Each has a 512-byte receive FIFO and a 64-byte transmit FIFO. Bytes take
 * as long as their frame does at the prescaler's bit time (master ticks per
 * bit; 243 is 115200 baud).
 */
#include "next.h"
#include <stdio.h>
#include <stdlib.h>
#include "../devices/serial.h"

#define RX_SIZE   512
#define TX_SIZE   64
#define NEAR_FULL (RX_SIZE * 3 / 4)
#define ALMOST    (RX_SIZE - 2)

enum { SRC_UART0_RX = 1, SRC_UART1_RX = 2, SRC_UART0_TX = 12, SRC_UART1_TX = 13 };

static uint64_t frame_bits(uint8_t fr)
{
    return 1 + 5 + (fr >> 3 & 3) + (fr >> 2 & 1) + 1 + (fr & 1);
}

static uint64_t byte_ticks(const nx_uart *u)
{
    return (uint64_t)(u->prescaler ? u->prescaler : 1) * frame_bits(u->framing);
}

/* Master ticks per bit at the device's own speed. */
static uint64_t dev_bit(const nx_uart *u)
{
    uint32_t baud = u->dev && u->dev->baud ? u->dev->baud : 115200;
    return (28000000u + baud / 2) / baud;
}

/* Both ends agree on the speed to within 4%. */
static int in_step(const nx_uart *u)
{
    uint64_t a = u->prescaler, b = dev_bit(u);
    uint64_t d = a > b ? a - b : b - a;
    return d * 25 <= a;
}

/* Master ticks for a byte coming from the device. */
static uint64_t rx_ticks(const nx_uart *u)
{
    return frame_bits(u->framing) * dev_bit(u);
}

static uint8_t garble(uint8_t b)
{
    return (uint8_t)(b * 37 + 11);
}

static int rx_request(const zxn *n, int i)
{
    const nx_uart *u = &n->uart[i];
    int only_near = (n->nr_c6 >> (i ? 5 : 1)) & 1;
    return u->rx_count >= NEAR_FULL || (u->rx_count && !only_near);
}

/* Interrupts fire on a rising request (im2_peripheral.vhd). */
static void update_ints(zxn *n, int i, uint64_t t)
{
    nx_uart *u = &n->uart[i];
    int rx = rx_request(n, i), tx = u->tx_count == 0;
    if (rx && !u->req_rx)
        nx_raise_int(n, i ? SRC_UART1_RX : SRC_UART0_RX, t);
    if (tx && !u->req_tx)
        nx_raise_int(n, i ? SRC_UART1_TX : SRC_UART0_TX, t);
    u->req_rx = (uint8_t)rx;
    u->req_tx = (uint8_t)tx;
}

static void fifo_reset(nx_uart *u)
{
    u->rx_head = u->rx_count = 0;
    u->tx_head = u->tx_count = 0;
    u->tx_busy = 0;
    u->err_overflow = u->err_framing = 0;
}

static void run_one(zxn *n, int i, uint64_t upto)
{
    nx_uart *u = &n->uart[i];
    zx_serial *dev = u->dev;

    if (u->framing & 0x80) {        /* fifo reset held */
        fifo_reset(u);
        if (u->rx_free < upto)
            u->rx_free = upto;
        return;
    }
    for (;;) {
        uint64_t t_tx = u->tx_busy ? u->tx_done : UINT64_MAX;
        uint64_t t_rx = UINT64_MAX;
        int flow = (u->framing & 0x20) != 0;
        if (dev && dev->avail(dev) && !(flow && u->rx_count >= ALMOST))
            t_rx = u->rx_free + rx_ticks(u);
        if (t_tx > upto && t_rx > upto)
            break;
        if (t_tx <= t_rx) {
            /* a byte has gone out; the next one starts at once */
            if (dev)
                dev->put(dev, in_step(u) ? u->tx_byte : garble(u->tx_byte));
            u->tx_busy = 0;
            if (u->tx_count && !(u->framing & 0x40)) {
                u->tx_byte = u->tx[u->tx_head];
                u->tx_head = (uint8_t)((u->tx_head + 1) % TX_SIZE);
                u->tx_count--;
                u->tx_busy = 1;
                u->tx_done = t_tx + byte_ticks(u);
            }
            update_ints(n, i, t_tx);
        } else {
            uint8_t b = dev->get(dev);
            int err = !in_step(u);
            u->rx_free = t_rx;
            if (err) {
                b = garble(b);
                u->err_framing = 1;
            }
            if (u->rx_count < RX_SIZE) {
                int err_bit = u->err_overflow || u->err_framing;
                u->rx[(u->rx_head + u->rx_count) % RX_SIZE] = (uint16_t)(b | err_bit << 8);
                u->rx_count++;
            } else {
                u->err_overflow = 1;
            }
            update_ints(n, i, t_rx);
        }
    }
    /* an idle line: the next byte can only start from now */
    if (!(dev && dev->avail(dev)) && u->rx_free < upto)
        u->rx_free = upto;
}

static void schedule(zxn *n)
{
    int i;
    n->uart_next = UINT64_MAX;
    for (i = 0; i < 2; i++) {
        nx_uart *u = &n->uart[i];
        if (u->tx_busy && u->tx_done < n->uart_next)
            n->uart_next = u->tx_done;
        if (u->dev && u->dev->avail(u->dev) && !(u->framing & 0x80)) {
            uint64_t t = u->rx_free + rx_ticks(u);
            if (t < n->uart_next)
                n->uart_next = t;
        }
    }
}

void nx_uart_run(zxn *n, uint64_t upto)
{
    run_one(n, 0, upto);
    run_one(n, 1, upto);
    schedule(n);
}

/* Once per frame: let the devices do their work (network, timers). */
void nx_uart_poll(zxn *n, uint64_t now)
{
    int i;
    nx_uart_run(n, now);
    for (i = 0; i < 2; i++)
        if (n->uart[i].dev && n->uart[i].dev->poll)
            n->uart[i].dev->poll(n->uart[i].dev, now / 28);
    schedule(n);
}

uint8_t nx_uart_read(zxn *n, int reg)
{
    nx_uart *u = &n->uart[n->uart_sel];
    uint8_t r;
    nx_uart_run(n, n->clk);
    switch (reg) {
    case 0:                         /* 0x143B: received byte */
        if (!u->rx_count)
            return 0;
        r = (uint8_t)u->rx[u->rx_head];
        if (getenv("UART_DEBUG")) fprintf(stderr, "{R%02x}", r);
        u->rx_head = (uint16_t)((u->rx_head + 1) % RX_SIZE);
        u->rx_count--;
        update_ints(n, n->uart_sel, n->clk);
        schedule(n);
        return r;
    case 1:                         /* 0x153B */
        return (uint8_t)(n->uart_sel << 6 | u->prescaler >> 14);
    case 2:                         /* 0x163B */
        return u->framing;
    default:                        /* 0x133B: status */
        r = (uint8_t)((u->err_framing << 6)
            | ((u->rx_count && (u->rx[u->rx_head] >> 8)) << 5)
            | ((!u->tx_count && !u->tx_busy) << 4)
            | ((u->rx_count >= NEAR_FULL) << 3)
            | (u->err_overflow << 2)
            | ((u->tx_count == TX_SIZE) << 1)
            | (u->rx_count != 0));
        u->err_overflow = u->err_framing = 0;
        if (getenv("UART_DEBUG") && n->uart_sel) { static int k; if (k++ < 400) fprintf(stderr, "{S%02x}", r); }
        return r;
    }
}

void nx_uart_write(zxn *n, int reg, uint8_t v)
{
    nx_uart *u;
    nx_uart_run(n, n->clk);
    u = &n->uart[n->uart_sel];
    switch (reg) {
    case 0:                         /* prescaler low 14 bits, 7 at a time */
        if (v & 0x80)
            u->prescaler = (u->prescaler & ~(0x7fu << 7)) | (uint32_t)(v & 0x7f) << 7;
        else
            u->prescaler = (u->prescaler & ~0x7fu) | (v & 0x7fu);
        break;
    case 1:
        n->uart_sel = v >> 6 & 1;
        u = &n->uart[n->uart_sel];
        if (v & 0x10)
            u->prescaler = (u->prescaler & 0x3fff) | (uint32_t)(v & 7) << 14;
        break;
    case 2:
        u->framing = v;
        if (v & 0x80)
            fifo_reset(u);
        break;
    default:
        if (u->tx_count < TX_SIZE && !(u->framing & 0x80)) {
            u->tx[(u->tx_head + u->tx_count) % TX_SIZE] = v;
            u->tx_count++;
            if (!u->tx_busy && !(u->framing & 0x40)) {
                u->tx_byte = u->tx[u->tx_head];
                u->tx_head = (uint8_t)((u->tx_head + 1) % TX_SIZE);
                u->tx_count--;
                u->tx_busy = 1;
                u->tx_done = n->clk + byte_ticks(u);
            }
            update_ints(n, n->uart_sel, n->clk);
        }
        break;
    }
    schedule(n);
}

/* reset: the FIFOs and the select; the speed and framing stay. */
void nx_uart_reset(zxn *n)
{
    int i;
    n->uart_sel = 0;
    for (i = 0; i < 2; i++) {
        fifo_reset(&n->uart[i]);
        n->uart[i].rx_free = n->clk;
        n->uart[i].req_rx = 0;
        n->uart[i].req_tx = 1;
    }
    schedule(n);
}

void nx_uart_power_on(zxn *n)
{
    int i;
    for (i = 0; i < 2; i++) {
        n->uart[i].prescaler = 243;     /* 115200 baud */
        n->uart[i].framing = 0x18;      /* 8N1 */
    }
    nx_uart_reset(n);
}
