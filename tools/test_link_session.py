#!/usr/bin/env python3
"""
Link session tests. Runs on the host, no hardware:

    python3 tools/test_link_session.py

The session layer is where the two-board split either is or is not safe. These
checks pin the behaviour the architecture depends on: ACKs answer the right
sequence number, and a silent HMI board drops every user request while leaving
the thermostat's inputs untouched.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "link"))
sys.path.insert(0, os.path.join(ROOT, "s3_control"))

import spalink_codec as codec      # noqa: E402
import spalink_session as session  # noqa: E402

FAILED = []


def check(name, cond, detail=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s %s" % (name, detail))
        FAILED.append(name)


class Clock:
    def __init__(self):
        self.t = 0

    def install(self):
        session._ticks_ms = lambda: self.t
        session._ticks_diff = lambda a, b: a - b


class LoopbackTransport:
    """Stands in for a UART: what the peer 'sends' is what we decode, and what we
    send is captured for inspection. Frames really are encoded and decoded, so
    the framing is exercised rather than bypassed."""

    def __init__(self):
        self.tx = []              # (msg_id, hdr, payload) we transmitted
        self._rx = []             # queued for the next poll()
        self._dec = codec.Decoder()

    def send(self, msg_id, hdr, payload=b""):
        frame = codec.encode_uart(msg_id, hdr, payload)
        got = codec.Decoder().feed(frame)
        assert got == [(msg_id, hdr, payload)], "transmitted frame did not round-trip"
        self.tx.append((msg_id, hdr, payload))

    def peer_sends(self, msg_id, hdr, payload=b""):
        self._rx.append(codec.encode_uart(msg_id, hdr, payload))

    def poll(self):
        msgs = []
        for frame in self._rx:
            msgs.extend(self._dec.feed(frame))
        self._rx = []
        return msgs

    def stats(self):
        return {"bad_crc": self._dec.bad_crc, "bad_len": self._dec.bad_len}


def main():
    clock = Clock()
    clock.install()

    print("liveness")
    t = LoopbackTransport()
    s = session.LinkSession(t, timeout_ms=session.LINK_TIMEOUT_MS)
    check("link starts down before the peer is ever heard", not s.is_up())
    check("silent_ms is None before first contact", s.silent_ms() is None)

    t.peer_sends(codec.MSG_PING, 1)
    s.poll()
    check("link comes up on the first valid frame", s.is_up())

    clock.t += session.LINK_TIMEOUT_MS - 1
    check("link still up just inside the timeout", s.is_up())
    clock.t += 2
    check("link goes down just past the timeout", not s.is_up())

    t.peer_sends(codec.MSG_PING, 2)
    s.poll()
    check("link recovers when the peer returns", s.is_up())

    print("self-echo")
    # The S3 and the P4 share one half-duplex pair, so the S3 can hear its own
    # STATUS come back: a driver-enable stuck on, or a bench loopback. The frame
    # is CRC-valid and decodes perfectly, so only the id gives it away.
    t = LoopbackTransport()
    s = session.LinkSession(t, timeout_ms=session.LINK_TIMEOUT_MS)
    for mid in (codec.MSG_STATUS, codec.MSG_TEMP, codec.MSG_TIMERS, codec.MSG_HELLO):
        # FLAG_ACK on the first one: an echo that asks for an acknowledgement is
        # the pathological case, because answering it puts another frame on the
        # wire to be echoed in turn.
        hdr = 3 | (codec.FLAG_ACK if mid == codec.MSG_STATUS else 0)
        t.peer_sends(mid, hdr, b"\x00\x00\x00")
    msgs = s.poll()
    check("our own frames do not bring the link up", not s.is_up())
    check("...and never reach the dispatch, so they are never NACKed",
          msgs == [], msgs)
    check("...and are not acknowledged, even when they ask to be",
          t.tx == [], t.tx)
    check("...but are counted", s.echo_count == 4, s.echo_count)
    check("...and are kept out of the peer's receive count",
          s.rx_count == 0, s.rx_count)

    # The P4 speaking must still work through the same loopback.
    t.peer_sends(codec.MSG_REQ, 7, bytes([codec.REQ_JETS, 0]))
    msgs = s.poll()
    check("a real REQ on the same wire still connects", s.is_up())
    check("...and still reaches the caller",
          [m[0] for m in msgs] == [codec.MSG_REQ], msgs)

    # The point of the whole exercise: an echo must not hold the watchdog open.
    clock.t += session.LINK_TIMEOUT_MS - 1
    t.peer_sends(codec.MSG_STATUS, 9, b"\x00\x00\x00")
    s.poll()
    clock.t += 2
    check("an echo does not keep a dead link alive", not s.is_up())

    # ACK/NACK are sent by both nodes, so they cannot be attributed by id alone
    # and stay peer traffic. The P4 does not send them today, so this costs
    # nothing now and stays correct if it ever starts.
    t = LoopbackTransport()
    s = session.LinkSession(t)
    t.peer_sends(codec.MSG_ACK, codec.FLAG_RESP, bytes([1]))
    check("an ACK is still treated as the peer", s.poll() and s.is_up())

    print("acknowledgement")
    t = LoopbackTransport()
    s = session.LinkSession(t)
    t.peer_sends(codec.MSG_SETPOINT, 0x2A | codec.FLAG_ACK, codec.pack_i16(1020))
    msgs = s.poll()
    acks = [m for m in t.tx if m[0] == codec.MSG_ACK]
    check("an ACK-requested frame is acknowledged", len(acks) == 1)
    check("the ACK carries the requesting sequence number",
          acks and acks[0][2] == bytes([0x2A]), acks[:1])
    check("the ACK is marked as a response",
          acks and (acks[0][1] & codec.FLAG_RESP), acks[:1])
    check("the message still reaches the caller", len(msgs) == 1)

    t.tx.clear()
    t.peer_sends(codec.MSG_PING, 0x05)
    s.poll()
    check("a frame that did not ask for an ACK gets none",
          not [m for m in t.tx if m[0] == codec.MSG_ACK])

    print("sequence numbers")
    t = LoopbackTransport()
    s = session.LinkSession(t)
    for _ in range(70):
        s.send(codec.MSG_STATUS, b"\x00\x00\x00")
    seqs = [hdr & codec.SEQ_MASK for _, hdr, _ in t.tx]
    check("sequence numbers advance and wrap within 6 bits",
          max(seqs) <= codec.SEQ_MASK and len(set(seqs)) == codec.SEQ_MASK + 1,
          (max(seqs), len(set(seqs))))
    check("want_ack sets the ACK flag", (lambda: (
        s.send(codec.MSG_SETPOINT, codec.pack_i16(1000), want_ack=True),
        bool(t.tx[-1][1] & codec.FLAG_ACK))[1])())

    print("failsafe on link loss")
    fs = session.failsafe_requests()
    check("failsafe drops every user request", not any(fs.values()), fs)
    for key in ("xJetsRequest", "xBlowerRequest", "xPump2Request", "xPump3Request",
                "xPump1HighRequest", "xLightRequest", "xSpaEnable", "xPumpRequest"):
        check("failsafe covers %s" % key, key in fs)
    # The thermostat runs off measured temperature and the persisted setpoint, so
    # neither may appear in the failsafe set — if it did, losing the touchscreen
    # would disable freeze protection.
    check("failsafe does not touch the temperature reading",
          "rWaterTemp_F" not in fs)
    check("failsafe does not touch the sensor health flag",
          "xTempSensorOK" not in fs)

    print("status packing")
    outputs = {"xHeater": True, "xPump1_Low": True, "xJets": False, "xLight": True}
    inputs = {"xFlowSwitch": True, "xHighLimitOK": True, "xRemoteEStopOK": False,
              "xTempSensorOK": True, "xSpaEnable": True}
    p = session.status_payload(outputs, inputs, 4)
    check("outputs byte carries heater, pump1-low and light",
          p[0] == (codec.OUT_HEATER | codec.OUT_PUMP1_LOW | codec.OUT_LIGHT), hex(p[0]))
    check("inputs byte reports the open e-stop", not (p[1] & codec.IN_ESTOP_OK), hex(p[1]))
    check("fault byte sets the active bit and the code",
          p[2] == (codec.FAULT_ACTIVE | 4), hex(p[2]))
    clear = session.status_payload(outputs, inputs, 0)
    check("no fault leaves the active bit clear", clear[2] == 0, hex(clear[2]))
    check("status payload fits a single CAN frame", len(p) + 1 <= 8)

    print("request decoding")
    r = session.requests_from_msg(bytes([codec.REQ_JETS | codec.REQ_LIGHT]))
    check("decoded requests set exactly the asked-for bits",
          r["xJetsRequest"] and r["xLightRequest"] and not r["xBlowerRequest"], r)
    check("a truncated REQ is rejected rather than half-applied",
          session.requests_from_msg(b"") is None)

    print()
    if FAILED:
        print("FAILED: %d" % len(FAILED))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
