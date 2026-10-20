/*
 * SD card in SPI mode, backed by a block device: an image file or a host
 * folder shown as a FAT32 volume (../storage/).
 *
 * Behaves as an SDHC card (block addressing, CMD8 and ACMD41 with HCS,
 * CCS set in the OCR), which is what the boot ROM, the firmware and
 * NextZXOS expect.
 */
#include "next.h"
#include "../storage/blockdev.h"

#include <stdlib.h>
#include <string.h>

enum { SD_IDLE, SD_CMD, SD_RESP, SD_READ, SD_WRITE_WAIT, SD_WRITE_DATA, SD_WRITE_RESP };

struct nx_sd {
    zx_blockdev *bd;
    uint64_t blocks;
    int state;
    uint8_t cmd[6];
    int cmd_len;
    uint8_t out[600];       /* bytes queued for MISO */
    int out_len, out_pos;
    int idle;               /* in idle state (before ACMD41 completes) */
    int app;                /* next command is an ACMD */
    int multi;              /* CMD18 / CMD25 in progress */
    uint32_t addr;          /* block for the next read/write */
    uint8_t buf[512 + 2];
    int buf_len;
    int busy;               /* busy bytes to send after a write */
};

nx_sd *nx_sd_open(const char *path)
{
    nx_sd *sd;
    zx_blockdev *bd = zx_bd_open(path);
    if (!bd)
        return NULL;
    sd = calloc(1, sizeof *sd);
    if (!sd) {
        zx_bd_close(bd);
        return NULL;
    }
    sd->bd = bd;
    sd->blocks = bd->blocks;
    sd->idle = 1;
    return sd;
}

void nx_sd_close(nx_sd *sd)
{
    if (!sd)
        return;
    zx_bd_close(sd->bd);
    free(sd);
}

void nx_sd_deselect(nx_sd *sd)
{
    if (!sd)
        return;
    if (sd->state != SD_READ || !sd->multi)
        sd->state = SD_IDLE;
    sd->cmd_len = 0;
}

static void queue(nx_sd *sd, const uint8_t *p, int n)
{
    if (sd->out_len + n > (int)sizeof sd->out)
        n = (int)sizeof sd->out - sd->out_len;
    memcpy(sd->out + sd->out_len, p, (size_t)n);
    sd->out_len += n;
}

static void queue1(nx_sd *sd, uint8_t b) { queue(sd, &b, 1); }

static void queue_block(nx_sd *sd)
{
    uint8_t blk[512];
    memset(blk, 0xff, sizeof blk);
    if (sd->addr < sd->blocks)
        sd->bd->read(sd->bd, sd->addr, blk);
    queue1(sd, 0xff);
    queue1(sd, 0xfe);           /* start block token */
    queue(sd, blk, 512);
    queue1(sd, 0xff);           /* CRC, unchecked */
    queue1(sd, 0xff);
    sd->addr++;
}

static void command(nx_sd *sd)
{
    uint8_t c = sd->cmd[0] & 0x3f;
    uint32_t arg = (uint32_t)sd->cmd[1] << 24 | (uint32_t)sd->cmd[2] << 16 | (uint32_t)sd->cmd[3] << 8 | sd->cmd[4];
    uint8_t r1 = sd->idle ? 0x01 : 0x00;
    int app = sd->app;

    sd->app = 0;
    sd->out_len = sd->out_pos = 0;
    queue1(sd, 0xff);           /* NCR: one byte before the response */
    sd->state = SD_RESP;

    if (app) {
        switch (c) {
        case 41: /* SD_SEND_OP_COND */
            sd->idle = 0;
            queue1(sd, 0x00);
            return;
        case 13: /* SD_STATUS */
            queue1(sd, r1);
            queue1(sd, 0x00);
            return;
        case 51: { /* SEND_SCR */
            static const uint8_t scr[8] = { 0x02, 0x35, 0x80, 0x00, 0, 0, 0, 0 };
            queue1(sd, r1);
            queue1(sd, 0xff);
            queue1(sd, 0xfe);
            queue(sd, scr, 8);
            queue1(sd, 0xff);
            queue1(sd, 0xff);
            return;
        }
        default:
            break;  /* falls through to the normal command set */
        }
    }

    switch (c) {
    case 0: /* GO_IDLE_STATE */
        sd->idle = 1;
        sd->multi = 0;
        queue1(sd, 0x01);
        break;
    case 1: /* SEND_OP_COND (MMC) */
        sd->idle = 0;
        queue1(sd, 0x00);
        break;
    case 8: /* SEND_IF_COND */
        queue1(sd, r1);
        queue1(sd, 0x00);
        queue1(sd, 0x00);
        queue1(sd, (uint8_t)(arg >> 8 & 0x0f));
        queue1(sd, (uint8_t)arg);
        break;
    case 9: { /* SEND_CSD: version 2.0, capacity from the image size */
        uint8_t csd[16] = { 0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0x00,
                            0x00, 0x00, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0x01 };
        uint32_t c_size = (uint32_t)(sd->blocks / 1024);
        if (c_size)
            c_size--;
        csd[7] = (uint8_t)(c_size >> 16 & 0x3f);
        csd[8] = (uint8_t)(c_size >> 8);
        csd[9] = (uint8_t)c_size;
        queue1(sd, r1);
        queue1(sd, 0xff);
        queue1(sd, 0xfe);
        queue(sd, csd, 16);
        queue1(sd, 0xff);
        queue1(sd, 0xff);
        break;
    }
    case 10: { /* SEND_CID */
        static const uint8_t cid[16] = { 0x03, 'Z', 'X', 'N', 'E', 'X', 'T', 'E',
                                         0x10, 0, 0, 0, 1, 0x01, 0x6a, 0x01 };
        queue1(sd, r1);
        queue1(sd, 0xff);
        queue1(sd, 0xfe);
        queue(sd, cid, 16);
        queue1(sd, 0xff);
        queue1(sd, 0xff);
        break;
    }
    case 12: /* STOP_TRANSMISSION */
        sd->multi = 0;
        queue1(sd, 0xff);       /* stuff byte */
        queue1(sd, r1);
        break;
    case 13: /* SEND_STATUS */
        queue1(sd, r1);
        queue1(sd, 0x00);
        break;
    case 16: /* SET_BLOCKLEN */
        queue1(sd, r1);
        break;
    case 17: /* READ_SINGLE_BLOCK */
    case 18: /* READ_MULTIPLE_BLOCK */
        if (arg >= sd->blocks) {
            queue1(sd, (uint8_t)(r1 | 0x40));   /* address error */
            break;
        }
        queue1(sd, r1);
        sd->addr = arg;
        sd->multi = c == 18;
        queue_block(sd);
        sd->state = sd->multi ? SD_READ : SD_RESP;
        break;
    case 24: /* WRITE_BLOCK */
    case 25: /* WRITE_MULTIPLE_BLOCK */
        queue1(sd, r1);
        sd->addr = arg;
        sd->multi = c == 25;
        sd->state = SD_WRITE_WAIT;
        break;
    case 55: /* APP_CMD */
        sd->app = 1;
        queue1(sd, r1);
        break;
    case 58: /* READ_OCR: powered up, SDHC (CCS) */
        queue1(sd, r1);
        queue1(sd, 0xc0);
        queue1(sd, 0xff);
        queue1(sd, 0x80);
        queue1(sd, 0x00);
        break;
    case 59: /* CRC_ON_OFF */
        queue1(sd, r1);
        break;
    default:
        queue1(sd, (uint8_t)(r1 | 0x04));       /* illegal command */
        break;
    }
}

static uint8_t next_out(nx_sd *sd)
{
    if (sd->out_pos < sd->out_len)
        return sd->out[sd->out_pos++];
    if (sd->state == SD_READ && sd->multi) {
        sd->out_len = sd->out_pos = 0;
        queue_block(sd);
        return sd->out[sd->out_pos++];
    }
    return 0xff;
}

uint8_t nx_sd_xfer(nx_sd *sd, uint8_t in)
{
    uint8_t r;
    if (!sd)
        return 0xff;

    switch (sd->state) {
    case SD_WRITE_WAIT:
        r = next_out(sd);
        if (in == 0xfe || in == 0xfc) {         /* start block token */
            sd->state = SD_WRITE_DATA;
            sd->buf_len = 0;
        } else if (in == 0xfd && sd->multi) {   /* stop tran token */
            sd->multi = 0;
            sd->state = SD_RESP;
            sd->out_len = sd->out_pos = 0;
            queue1(sd, 0xff);
            queue1(sd, 0x00);                   /* one busy byte */
        }
        return r;
    case SD_WRITE_DATA:
        sd->buf[sd->buf_len++] = in;
        if (sd->buf_len == 514) {               /* data + CRC */
            if (sd->addr < sd->blocks)
                sd->bd->write(sd->bd, sd->addr, sd->buf);
            sd->addr++;
            sd->out_len = sd->out_pos = 0;
            queue1(sd, 0x05);                   /* data accepted */
            queue1(sd, 0x00);                   /* busy */
            sd->state = sd->multi ? SD_WRITE_WAIT : SD_RESP;
        }
        return 0xff;
    default:
        break;
    }

    /* Command framing: a command starts with 01xxxxxx. */
    if (sd->cmd_len == 0 && (in & 0xc0) == 0x40) {
        if (sd->state == SD_READ && (in & 0x3f) != 12) {
            /* only STOP_TRANSMISSION may interrupt a multi-block read */
        }
        sd->cmd[sd->cmd_len++] = in;
        return next_out(sd);
    }
    if (sd->cmd_len > 0) {
        sd->cmd[sd->cmd_len++] = in;
        r = next_out(sd);
        if (sd->cmd_len == 6) {
            sd->cmd_len = 0;
            command(sd);
        }
        return r;
    }
    return next_out(sd);
}

void nx_sd_tick(nx_sd *sd)
{
    if (sd && sd->bd->tick)
        sd->bd->tick(sd->bd);
}

int nx_sd_flush(nx_sd *sd)
{
    return sd && sd->bd->flush ? sd->bd->flush(sd->bd) : 0;
}
