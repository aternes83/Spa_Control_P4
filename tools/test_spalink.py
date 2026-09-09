#!/usr/bin/env python3
"""
SpaLink codec tests. Runs on the host, no hardware needed:

    python3 tools/test_spalink.py

Two jobs. First, the ordinary correctness checks — round-trips, framing edge
cases, corruption rejection. Second, and the reason this exists at all: the S3
runs the Python codec and the P4 runs the C one, so a disagreement between them
would show up as intermittent link failures on the bench, at 2 a.m., with an
oscilloscope. Every vector below is therefore run through *both* implementations
and the bytes compared.
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "link"))

import spalink_codec as py  # noqa: E402


class CCodec:
    """Drives the compiled C codec through tools/spalink_vectors.c."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="spalink-")
        self.bin = os.path.join(self.tmp, "spalink_vec")
        cc = os.environ.get("CC", "cc")
        subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-o", self.bin,
             os.path.join(HERE, "spalink_vectors.c"),
             os.path.join(ROOT, "link", "spalink_codec.c")],
            check=True,
        )

    def run(self, lines):
        out = subprocess.run([self.bin], input="\n".join(lines) + "\n",
                             capture_output=True, text=True, check=True)
        return out.stdout.splitlines()

    def crc16(self, data):
        return int(self.run(["C " + data.hex()])[0], 16)

    def encode(self, msg_id, hdr, payload):
        line = "E %02x %02x %s" % (msg_id, hdr, payload.hex() or "-")
        r = self.run([line])[0]
        return None if r == "ERR" else bytes.fromhex(r)

    def decode(self, stream):
        out = self.run(["D " + stream.hex()])
        msgs = []
        for line in out:
            if line.startswith("bad_crc="):
                bad = dict(kv.split("=") for kv in line.split())
                return msgs, int(bad["bad_crc"]), int(bad["bad_len"])
            mid, hdr, pay = (line.split() + [""])[:3]
            msgs.append((int(mid, 16), int(hdr, 16), bytes.fromhex(pay)))
        return msgs, 0, 0


FAILED = []


def check(name, cond, detail=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s %s" % (name, detail))
        FAILED.append(name)


def main():
    c = CCodec()
    rng = random.Random(20260909)

    print("crc16 — C vs Python")
    vectors = [b"", b"\x00", b"123456789", bytes(range(256))]
    vectors += [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 40))) for _ in range(50)]
    mismatch = [v for v in vectors if py.crc16(v) != c.crc16(v)]
    check("crc16 agrees on %d vectors" % len(vectors), not mismatch,
          mismatch[:1])
    # Known-answer test, so a matching pair of *wrong* implementations still fails.
    check("crc16(\"123456789\") == 0x29B1 (CCITT-FALSE KAT)",
          py.crc16(b"123456789") == 0x29B1, hex(py.crc16(b"123456789")))

    print("encode — C vs Python")
    cases = []
    for msg_id in (py.MSG_ACK, py.MSG_STATUS, py.MSG_TEMP, py.MSG_REQ, py.MSG_PING, 0xFF):
        for hdr in (0x00, 0x3F, py.FLAG_ACK | 7, py.FLAG_RESP | py.FLAG_ACK | 0x2A):
            for n in range(py.MAX_PAYLOAD + 1):
                cases.append((msg_id, hdr, bytes(rng.randrange(256) for _ in range(n))))
    # Payloads that force byte stuffing in every position.
    for n in range(1, py.MAX_PAYLOAD + 1):
        cases.append((py.MSG_STATUS, 0x01, bytes([py.SOF] * n)))
        cases.append((py.MSG_STATUS, 0x01, bytes([py.ESC] * n)))
    cases.append((py.SOF, py.SOF, bytes([py.SOF, py.ESC, py.SOF])))
    bad = [(m, h, p) for (m, h, p) in cases if py.encode_uart(m, h, p) != c.encode(m, h, p)]
    check("encode agrees on %d cases" % len(cases), not bad, bad[:1])

    print("round-trip — every case, through both decoders")
    bad_py, bad_c = [], []
    for msg_id, hdr, payload in cases:
        frame = py.encode_uart(msg_id, hdr, payload)
        got = py.Decoder().feed(frame)
        if got != [(msg_id, hdr, payload)]:
            bad_py.append((msg_id, hdr, payload, got))
        got_c, _, _ = c.decode(frame)
        if got_c != [(msg_id, hdr, payload)]:
            bad_c.append((msg_id, hdr, payload, got_c))
    check("python decoder round-trips %d frames" % len(cases), not bad_py, bad_py[:1])
    check("c decoder round-trips %d frames" % len(cases), not bad_c, bad_c[:1])

    print("framing robustness")
    frame = py.encode_uart(py.MSG_TEMP, 3, py.pack_i16(1014) + py.pack_i16(1000))

    # Payload longer than a CAN frame can carry must be refused, not truncated.
    try:
        py.encode_uart(py.MSG_TEMP, 0, b"\x00" * (py.MAX_PAYLOAD + 1))
        over = False
    except ValueError:
        over = True
    check("python rejects oversized payload", over)
    check("c rejects oversized payload",
          c.encode(py.MSG_TEMP, 0, b"\x00" * (py.MAX_PAYLOAD + 1)) is None)

    # Line noise before, between and after frames.
    noise = bytes([0x00, 0xFF, 0x55, 0xAA])
    stream = noise + frame + noise + frame + noise
    d = py.Decoder()
    check("python recovers 2 frames from noise", len(d.feed(stream)) == 2)
    msgs, _, _ = c.decode(stream)
    check("c recovers 2 frames from noise", len(msgs) == 2)

    # A truncated frame must not swallow the good frame behind it.
    truncated = frame[:len(frame) - 3] + frame
    d = py.Decoder()
    check("python resyncs after truncated frame", len(d.feed(truncated)) == 1)
    msgs, _, _ = c.decode(truncated)
    check("c resyncs after truncated frame", len(msgs) == 1)

    # Every single-byte corruption of the payload/CRC region must be caught.
    escaped_py = escaped_c = 0
    for i in range(1, len(frame)):
        for bit in range(8):
            bad_frame = bytearray(frame)
            bad_frame[i] ^= (1 << bit)
            if py.Decoder().feed(bytes(bad_frame)) == [(py.MSG_TEMP, 3, frame[4:8])]:
                escaped_py += 1
            got, _, _ = c.decode(bytes(bad_frame))
            if got == [(py.MSG_TEMP, 3, frame[4:8])]:
                escaped_c += 1
    check("python catches every single-bit corruption", escaped_py == 0, escaped_py)
    check("c catches every single-bit corruption", escaped_c == 0, escaped_c)

    # Fragmented delivery — the decoder is fed one byte at a time, as a UART does.
    d = py.Decoder()
    out = []
    for b in bytes(frame):
        out.extend(d.feed(bytes([b])))
    check("python decodes across byte-at-a-time delivery", len(out) == 1)

    print("CAN mapping")
    ok = True
    for msg_id, hdr, payload in cases:
        cid, data = py.encode_can(msg_id, hdr, payload)
        if py.decode_can(cid, data) != (msg_id, hdr, payload):
            ok = False
            break
        if len(data) > 8:
            ok = False
            break
    check("CAN frames round-trip and fit 8 bytes", ok)
    check("CAN ids stay inside the 11-bit space",
          py.encode_can(0xFF, 0, b"")[0] <= 0x7FF)

    print("payload helpers")
    check("i16 round-trips across the range",
          all(py.unpack_i16(py.pack_i16(v)) == v for v in (-32768, -1, 0, 1, 1014, 32767)))
    check("deci-F conversion holds 0.1 F resolution",
          py.dF_to_f(py.f_to_dF(101.4)) == 101.4 and py.dF_to_f(py.f_to_dF(-4.0)) == -4.0)

    print()
    if FAILED:
        print("FAILED: %d" % len(FAILED))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
