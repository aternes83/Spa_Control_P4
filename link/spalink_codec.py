"""
SpaLink v1 codec — pure Python, MicroPython-safe (no f-strings, no typing, no enum).

This is the *reference* implementation of the wire format described in
docs/PROTOCOL.md. The C implementation in link/spalink_codec.c must stay
byte-for-byte identical to it; tools/test_spalink.py compiles the C and
cross-checks both against the same vectors.

The codec knows nothing about UARTs, CAN controllers or the spa. It turns
(msg_id, hdr, payload) into bytes and back. Transports live elsewhere:
s3_control/spalink_uart.py (MicroPython) and p4_hmi/main/spalink_port.c (ESP-IDF).
"""

PROTO_VERSION = 1

# ── Framing constants (UART transport) ───────────────────────────────────────
SOF        = 0x7E   # start of frame
ESC        = 0x7D   # escape prefix
ESC_XOR    = 0x20   # escaped byte is XORed with this
MAX_PAYLOAD = 7     # 7 so that (hdr + payload) fits one 8-byte CAN frame

# ── Header bits ──────────────────────────────────────────────────────────────
SEQ_MASK   = 0x3F   # bits 0-5: sequence number, wraps at 64
FLAG_ACK   = 0x40   # bit 6: sender wants an ACK for this frame
FLAG_RESP  = 0x80   # bit 7: this frame is itself a response (ACK/NACK)

# ── Message ids ──────────────────────────────────────────────────────────────
# Both directions
MSG_ACK        = 0x01   # [acked_seq]
MSG_NACK       = 0x02   # [acked_seq, reason]
# S3 -> P4 (state; sent periodically and immediately on change)
MSG_STATUS     = 0x10   # [outputs, inputs, fault]
MSG_TEMP       = 0x11   # [water_dF:i16le, setpoint_dF:i16le]
MSG_TIMERS     = 0x12   # [spa_remain_s:u16le, light_remain_s:u16le]
MSG_HELLO      = 0x13   # [proto, fw_major, fw_minor, board_id]
# P4 -> S3 (commands)
MSG_REQ        = 0x20   # [requests, modes]
MSG_SETPOINT   = 0x21   # [setpoint_dF:i16le]
MSG_PING       = 0x22   # []
MSG_CLEAR_FAULT = 0x23  # []

# NACK reasons
NACK_BAD_LEN    = 1
NACK_UNKNOWN_ID = 2
NACK_REFUSED    = 3     # understood, but rejected (e.g. setpoint out of range)

# ── Which node originates each id ────────────────────────────────────────────
# The link is half-duplex and both nodes share one pair, so a frame arriving at
# a node can be that node's own transmission coming back off the wire: a
# transceiver whose driver-enable is stuck on, an auto-direction circuit that
# releases too early, or simply a direct TTL loopback while bringing a bench up.
#
# An echoed frame is CRC-valid and decodes perfectly, which is exactly what
# makes it dangerous: without this table each side counts its own voice as proof
# the peer is alive. See LinkSession.poll() and p4_hmi/main/spa_state.c for the
# two places that rule is applied.
DIR_UNKNOWN = 0   # not a SpaLink id — a foreign device, or a future message
DIR_ANY     = 1   # ACK/NACK: either node may send these
DIR_CONTROL = 2   # S3 -> P4
DIR_HMI     = 3   # P4 -> S3

_DIR = {
    MSG_ACK:         DIR_ANY,
    MSG_NACK:        DIR_ANY,
    MSG_STATUS:      DIR_CONTROL,
    MSG_TEMP:        DIR_CONTROL,
    MSG_TIMERS:      DIR_CONTROL,
    MSG_HELLO:       DIR_CONTROL,
    MSG_REQ:         DIR_HMI,
    MSG_SETPOINT:    DIR_HMI,
    MSG_PING:        DIR_HMI,
    MSG_CLEAR_FAULT: DIR_HMI,
}


def msg_dir(msg_id):
    """Which node sends this id. DIR_UNKNOWN for anything not in the protocol."""
    return _DIR.get(msg_id, DIR_UNKNOWN)

# ── Bitfields ────────────────────────────────────────────────────────────────
# MSG_STATUS byte 0 — physical outputs, mirrors the S3's OUT dict
OUT_PUMP1_LOW  = 0x01
OUT_PUMP1_HIGH = 0x02
OUT_PUMP2      = 0x04
OUT_PUMP3      = 0x08
OUT_HEATER     = 0x10
OUT_JETS       = 0x20
OUT_BLOWER     = 0x40
OUT_LIGHT      = 0x80

# MSG_STATUS byte 1 — safety-relevant inputs, so the HMI can explain a fault
IN_FLOW_SWITCH   = 0x01
IN_HIGH_LIMIT_OK = 0x02
IN_ESTOP_OK      = 0x04
IN_TEMP_SENSOR_OK = 0x08
IN_SPA_ENABLE    = 0x10

# MSG_STATUS byte 2 — fault: bit7 set = faulted, low nibble = code (see spa_core)
FAULT_ACTIVE = 0x80
FAULT_CODE   = 0x0F

# MSG_REQ byte 0 — what the user is asking for
REQ_SPA_ENABLE  = 0x01
REQ_PUMP        = 0x02   # pump 1 low
REQ_PUMP1_HIGH  = 0x04
REQ_PUMP2       = 0x08
REQ_PUMP3       = 0x10
REQ_JETS        = 0x20
REQ_BLOWER      = 0x40
REQ_LIGHT       = 0x80

# MSG_REQ byte 1 — UI modes that shape control but are not raw outputs
MODE_ECO     = 0x01
MODE_MAX_JET = 0x02


def crc16(data):
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout).

    Chosen over a CRC-8 because a stuck/noisy UART line next to pump contactors
    is exactly where a weak check lets a corrupt frame through, and over CRC-32
    because 2 bytes keep the frame inside one CAN payload when the transport
    changes.
    """
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def _stuff(out, byte):
    if byte == SOF or byte == ESC:
        out.append(ESC)
        out.append(byte ^ ESC_XOR)
    else:
        out.append(byte)


def encode_uart(msg_id, hdr, payload=b""):
    """Frame one message for a byte-stream transport.

    Layout:  SOF | msg_id | hdr | len | payload... | crc_hi | crc_lo
    Every byte after SOF is stuffed, so SOF appears only at a frame start and a
    receiver can always resynchronise on it after line noise.
    """
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    body = bytearray()
    body.append(msg_id & 0xFF)
    body.append(hdr & 0xFF)
    body.append(len(payload))
    body.extend(payload)
    c = crc16(body)
    body.append((c >> 8) & 0xFF)
    body.append(c & 0xFF)

    out = bytearray()
    out.append(SOF)
    for b in body:
        _stuff(out, b)
    return bytes(out)


class Decoder:
    """Incremental UART frame decoder. Feed it whatever bytes arrived; it returns
    the complete, CRC-valid messages found. Garbage between frames is discarded
    silently — resynchronisation happens on the next SOF."""

    def __init__(self):
        self._buf = bytearray()
        self._in_frame = False
        self._escaped = False
        self.bad_crc = 0        # counters worth surfacing on the HMI diag screen
        self.bad_len = 0

    def reset(self):
        self._buf = bytearray()
        self._in_frame = False
        self._escaped = False

    def feed(self, data):
        """Returns a list of (msg_id, hdr, payload) tuples."""
        msgs = []
        for b in data:
            if b == SOF:
                # A SOF always starts a new frame, even mid-frame: a truncated
                # frame must never swallow the good one that follows it.
                self._buf = bytearray()
                self._in_frame = True
                self._escaped = False
                continue
            if not self._in_frame:
                continue
            if self._escaped:
                self._buf.append(b ^ ESC_XOR)
                self._escaped = False
            elif b == ESC:
                self._escaped = True
            else:
                self._buf.append(b)
            m = self._try_complete()
            if m is not None:
                msgs.append(m)
        return msgs

    def _try_complete(self):
        buf = self._buf
        if len(buf) < 3:
            return None
        length = buf[2]
        if length > MAX_PAYLOAD:
            self.bad_len += 1
            self._in_frame = False
            return None
        total = 3 + length + 2          # header + payload + crc
        if len(buf) < total:
            return None
        body = buf[:3 + length]
        got = (buf[3 + length] << 8) | buf[4 + length]
        self._in_frame = False
        if crc16(body) != got:
            self.bad_crc += 1
            return None
        return (body[0], body[1], bytes(body[3:3 + length]))


# ── CAN mapping ──────────────────────────────────────────────────────────────
# The same messages ride CAN unchanged: the message id becomes part of the 11-bit
# identifier (so hardware filtering and priority come for free), and the header
# byte leads the payload. CAN supplies its own CRC and ACK, so no framing or
# CRC16 is added — that is the whole reason payloads are capped at 7 bytes.
CAN_ID_BASE = 0x100


def encode_can(msg_id, hdr, payload=b""):
    """Returns (can_id, data) for a classic 11-bit CAN frame."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    return (CAN_ID_BASE + (msg_id & 0xFF), bytes(bytearray([hdr & 0xFF]) + bytearray(payload)))


def decode_can(can_id, data):
    """Inverse of encode_can. Returns (msg_id, hdr, payload) or None if the id is
    outside the SpaLink range or the frame is empty."""
    if not data:
        return None
    msg_id = can_id - CAN_ID_BASE
    if msg_id < 0 or msg_id > 0xFF:
        return None
    return (msg_id, data[0], bytes(data[1:]))


# ── Payload helpers ──────────────────────────────────────────────────────────
# Temperatures cross the link as deci-Fahrenheit in a signed 16-bit little-endian
# word: 0.1 °F resolution, -3276.8..3276.7 °F range. Floats are deliberately kept
# off the wire so the C and MicroPython sides cannot disagree about rounding.

def pack_i16(v):
    iv = int(round(v)) & 0xFFFF
    return bytes(bytearray([iv & 0xFF, (iv >> 8) & 0xFF]))


def unpack_i16(data, off=0):
    v = data[off] | (data[off + 1] << 8)
    return v - 0x10000 if v & 0x8000 else v


def pack_u16(v):
    iv = int(v) & 0xFFFF
    return bytes(bytearray([iv & 0xFF, (iv >> 8) & 0xFF]))


def unpack_u16(data, off=0):
    return data[off] | (data[off + 1] << 8)


def f_to_dF(f):
    return int(round(float(f) * 10.0))


def dF_to_f(d):
    return d / 10.0
