/*
 * SpaLink v1 codec — portable C, no ESP-IDF dependency.
 *
 * Byte-for-byte equivalent to link/spalink_codec.py; the two are cross-checked
 * against shared vectors by tools/test_spalink.py. Wire format: docs/PROTOCOL.md.
 *
 * The codec is transport-agnostic and allocation-free: the caller owns every
 * buffer. UART and TWAI plumbing lives in p4_hmi/main/spalink_port.c.
 */
#ifndef SPALINK_CODEC_H
#define SPALINK_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SPALINK_PROTO_VERSION 1

/* Framing (UART transport) */
#define SPALINK_SOF         0x7E
#define SPALINK_ESC         0x7D
#define SPALINK_ESC_XOR     0x20
#define SPALINK_MAX_PAYLOAD 7
/* SOF + worst-case fully-escaped (id, hdr, len, 7 payload, 2 crc) */
#define SPALINK_MAX_FRAME   (1 + 2 * (3 + SPALINK_MAX_PAYLOAD + 2))

/* Header bits */
#define SPALINK_SEQ_MASK  0x3F
#define SPALINK_FLAG_ACK  0x40
#define SPALINK_FLAG_RESP 0x80

/* Message ids — both directions */
#define SPALINK_MSG_ACK         0x01
#define SPALINK_MSG_NACK        0x02
/* S3 -> P4 */
#define SPALINK_MSG_STATUS      0x10
#define SPALINK_MSG_TEMP        0x11
#define SPALINK_MSG_TIMERS      0x12
#define SPALINK_MSG_HELLO       0x13
/* P4 -> S3 */
#define SPALINK_MSG_REQ         0x20
#define SPALINK_MSG_SETPOINT    0x21
#define SPALINK_MSG_PING        0x22
#define SPALINK_MSG_CLEAR_FAULT 0x23

/* NACK reasons */
#define SPALINK_NACK_BAD_LEN    1
#define SPALINK_NACK_UNKNOWN_ID 2
#define SPALINK_NACK_REFUSED    3

/* Which node originates each id. The link is half-duplex and both nodes share
 * one pair, so a frame arriving at a node can be that node's own transmission
 * coming back off the wire — a stuck driver-enable, an auto-direction circuit
 * that releases too early, or a TTL loopback on the bench. An echo is CRC-valid
 * and decodes perfectly, which is what makes it dangerous: without this a node
 * counts its own voice as proof the peer is alive. See spa_state_apply(). */
#define SPALINK_DIR_UNKNOWN 0   /* not a SpaLink id */
#define SPALINK_DIR_ANY     1   /* ACK/NACK: either node may send these */
#define SPALINK_DIR_CONTROL 2   /* S3 -> P4 */
#define SPALINK_DIR_HMI     3   /* P4 -> S3 */

/* STATUS byte 0 — outputs */
#define SPALINK_OUT_PUMP1_LOW  0x01
#define SPALINK_OUT_PUMP1_HIGH 0x02
#define SPALINK_OUT_PUMP2      0x04
#define SPALINK_OUT_PUMP3      0x08
#define SPALINK_OUT_HEATER     0x10
#define SPALINK_OUT_JETS       0x20
#define SPALINK_OUT_BLOWER     0x40
#define SPALINK_OUT_LIGHT      0x80

/* STATUS byte 1 — safety inputs */
#define SPALINK_IN_FLOW_SWITCH    0x01
#define SPALINK_IN_HIGH_LIMIT_OK  0x02
#define SPALINK_IN_ESTOP_OK       0x04
#define SPALINK_IN_TEMP_SENSOR_OK 0x08
#define SPALINK_IN_SPA_ENABLE     0x10

/* STATUS byte 2 — fault */
#define SPALINK_FAULT_ACTIVE 0x80
#define SPALINK_FAULT_CODE   0x0F

/* STATUS byte 3 — loads the controller is holding off because their own runtime
 * ceiling expired, not because an interlock said no. Same bit positions as
 * byte 0. Optional: a frame of len 3 is a controller that predates this byte and
 * reads as "nothing timed out". The panel releases the requests named here —
 * that release is also what restarts the ceiling on the controller. */

/* REQ byte 0 — user requests */
#define SPALINK_REQ_SPA_ENABLE 0x01
#define SPALINK_REQ_PUMP       0x02
#define SPALINK_REQ_PUMP1_HIGH 0x04
#define SPALINK_REQ_PUMP2      0x08
#define SPALINK_REQ_PUMP3      0x10
#define SPALINK_REQ_JETS       0x20
#define SPALINK_REQ_BLOWER     0x40
#define SPALINK_REQ_LIGHT      0x80

/* REQ byte 1 — UI modes */
#define SPALINK_MODE_ECO     0x01
#define SPALINK_MODE_MAX_JET 0x02

/* CAN mapping */
#define SPALINK_CAN_ID_BASE 0x100

typedef struct {
    uint8_t msg_id;
    uint8_t hdr;
    uint8_t len;
    uint8_t payload[SPALINK_MAX_PAYLOAD];
} spalink_msg_t;

/* Incremental UART decoder state. Zero-initialise before first use. */
typedef struct {
    uint8_t  buf[3 + SPALINK_MAX_PAYLOAD + 2];
    uint8_t  used;
    bool     in_frame;
    bool     escaped;
    uint32_t bad_crc;   /* surfaced on the HMI diagnostics screen */
    uint32_t bad_len;
} spalink_decoder_t;

uint16_t spalink_crc16(const uint8_t *data, size_t len);

/* Which node sends this id; SPALINK_DIR_UNKNOWN for anything not in the
 * protocol. Must agree with spalink_codec.msg_dir() — tools/test_spalink.py
 * checks every id in both implementations. */
uint8_t spalink_msg_dir(uint8_t msg_id);

/* Encode one message into out (>= SPALINK_MAX_FRAME bytes).
 * Returns the framed length, or 0 if payload_len exceeds SPALINK_MAX_PAYLOAD. */
size_t spalink_encode_uart(uint8_t msg_id, uint8_t hdr,
                           const uint8_t *payload, size_t payload_len,
                           uint8_t *out);

void spalink_decoder_init(spalink_decoder_t *d);

/* Push one received byte. Returns true and fills *out when a CRC-valid frame
 * completes on this byte. */
bool spalink_decoder_feed(spalink_decoder_t *d, uint8_t byte, spalink_msg_t *out);

/* CAN mapping: identifier carries the message id, header byte leads the data. */
uint32_t spalink_can_id(uint8_t msg_id);
size_t   spalink_encode_can(uint8_t hdr, const uint8_t *payload, size_t payload_len,
                            uint8_t *out);
bool     spalink_decode_can(uint32_t can_id, const uint8_t *data, size_t len,
                            spalink_msg_t *out);

/* Payload helpers — little-endian, matching the Python side exactly. */
void    spalink_pack_i16(int16_t v, uint8_t *out);
int16_t spalink_unpack_i16(const uint8_t *data);
void    spalink_pack_u16(uint16_t v, uint8_t *out);
uint16_t spalink_unpack_u16(const uint8_t *data);

#endif /* SPALINK_CODEC_H */
