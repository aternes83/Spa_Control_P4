#include "spalink_codec.h"

#include <string.h>

uint16_t spalink_crc16(const uint8_t *data, size_t len)
{
    /* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout. */
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static size_t stuff(uint8_t *out, size_t n, uint8_t byte)
{
    if (byte == SPALINK_SOF || byte == SPALINK_ESC) {
        out[n++] = SPALINK_ESC;
        out[n++] = byte ^ SPALINK_ESC_XOR;
    } else {
        out[n++] = byte;
    }
    return n;
}

size_t spalink_encode_uart(uint8_t msg_id, uint8_t hdr,
                           const uint8_t *payload, size_t payload_len,
                           uint8_t *out)
{
    if (payload_len > SPALINK_MAX_PAYLOAD) {
        return 0;
    }
    uint8_t body[3 + SPALINK_MAX_PAYLOAD + 2];
    size_t n = 0;
    body[n++] = msg_id;
    body[n++] = hdr;
    body[n++] = (uint8_t)payload_len;
    if (payload_len) {
        memcpy(&body[n], payload, payload_len);
        n += payload_len;
    }
    uint16_t crc = spalink_crc16(body, n);
    body[n++] = (uint8_t)(crc >> 8);
    body[n++] = (uint8_t)(crc & 0xFF);

    size_t o = 0;
    out[o++] = SPALINK_SOF;
    for (size_t i = 0; i < n; i++) {
        o = stuff(out, o, body[i]);
    }
    return o;
}

void spalink_decoder_init(spalink_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
}

bool spalink_decoder_feed(spalink_decoder_t *d, uint8_t byte, spalink_msg_t *out)
{
    if (byte == SPALINK_SOF) {
        /* A SOF always starts a new frame, even mid-frame: a truncated frame
         * must never swallow the good one that follows it. */
        d->used = 0;
        d->in_frame = true;
        d->escaped = false;
        return false;
    }
    if (!d->in_frame) {
        return false;
    }
    if (d->escaped) {
        byte ^= SPALINK_ESC_XOR;
        d->escaped = false;
    } else if (byte == SPALINK_ESC) {
        d->escaped = true;
        return false;
    }

    if (d->used >= sizeof(d->buf)) {
        /* Longer than any legal frame — drop and wait for the next SOF. */
        d->in_frame = false;
        d->bad_len++;
        return false;
    }
    d->buf[d->used++] = byte;

    if (d->used < 3) {
        return false;
    }
    uint8_t len = d->buf[2];
    if (len > SPALINK_MAX_PAYLOAD) {
        d->bad_len++;
        d->in_frame = false;
        return false;
    }
    size_t total = 3u + len + 2u;
    if (d->used < total) {
        return false;
    }

    d->in_frame = false;
    uint16_t got = (uint16_t)(d->buf[3 + len] << 8) | d->buf[4 + len];
    if (spalink_crc16(d->buf, 3u + len) != got) {
        d->bad_crc++;
        return false;
    }
    out->msg_id = d->buf[0];
    out->hdr    = d->buf[1];
    out->len    = len;
    if (len) {
        memcpy(out->payload, &d->buf[3], len);
    }
    return true;
}

uint32_t spalink_can_id(uint8_t msg_id)
{
    return (uint32_t)SPALINK_CAN_ID_BASE + msg_id;
}

size_t spalink_encode_can(uint8_t hdr, const uint8_t *payload, size_t payload_len,
                          uint8_t *out)
{
    if (payload_len > SPALINK_MAX_PAYLOAD) {
        return 0;
    }
    out[0] = hdr;
    if (payload_len) {
        memcpy(&out[1], payload, payload_len);
    }
    return payload_len + 1;
}

bool spalink_decode_can(uint32_t can_id, const uint8_t *data, size_t len,
                        spalink_msg_t *out)
{
    if (len == 0 || len > SPALINK_MAX_PAYLOAD + 1u) {
        return false;
    }
    if (can_id < SPALINK_CAN_ID_BASE || can_id > SPALINK_CAN_ID_BASE + 0xFF) {
        return false;
    }
    out->msg_id = (uint8_t)(can_id - SPALINK_CAN_ID_BASE);
    out->hdr    = data[0];
    out->len    = (uint8_t)(len - 1);
    if (out->len) {
        memcpy(out->payload, &data[1], out->len);
    }
    return true;
}

void spalink_pack_i16(int16_t v, uint8_t *out)
{
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
}

int16_t spalink_unpack_i16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

void spalink_pack_u16(uint16_t v, uint8_t *out)
{
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
}

uint16_t spalink_unpack_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}
