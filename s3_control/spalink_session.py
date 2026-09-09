"""
SpaLink session layer for the S3 control node (MicroPython).

The codec (link/spalink_codec.py) turns messages into bytes. This adds the parts
that make the link safe to depend on:

  * a transport that can be swapped without touching protocol code — UART today,
    TWAI/CAN once transceivers are fitted;
  * sequence numbers and ACKs for commands, so the P4 knows a setpoint landed;
  * a link watchdog, which is the whole point. The P4 owns the WiFi and the
    touchscreen, so it is the part most likely to crash, be reflashed, or have
    its cable knocked out. When the link goes quiet the S3 must not keep running
    jets and blowers on stale requests, and must not stop protecting the water.
    See failsafe_requests().
"""

import sys

sys.path.append("/link")          # on-device layout: /link/spalink_codec.py
try:
    import spalink_codec as codec
except ImportError:                # host tests / frozen layouts
    from link import spalink_codec as codec

try:
    import utime as time_mod
except ImportError:
    import time as time_mod


def _ticks_ms():
    if hasattr(time_mod, "ticks_ms"):
        return time_mod.ticks_ms()
    return int(time_mod.monotonic() * 1000)


def _ticks_diff(a, b):
    if hasattr(time_mod, "ticks_diff"):
        return time_mod.ticks_diff(a, b)
    return a - b


# If no valid frame arrives from the P4 within this window, the link is down.
# 1.5 s is six missed 4 Hz keepalives — long enough to ride out a reflash blip
# on the P4, short enough that a user watching the jets sees them stop.
LINK_TIMEOUT_MS = 1500


class UartTransport:
    """MicroPython UART transport. Byte stream in, framed messages out.

    Covers both wirings. Point-to-point TTL needs nothing extra. Half-duplex
    RS485 needs the driver enabled only while transmitting, so pass de_pin and
    this class raises it around each frame. Pass de_pin=None for a direct
    connection or an auto-direction transceiver — including the P4's on-board
    MAX485, which drives its own DE/RE from the TX line.
    """

    def __init__(self, uart_id, tx, rx, baud=115200, de_pin=None):
        from machine import UART, Pin
        self.uart = UART(uart_id, baudrate=baud, tx=Pin(tx), rx=Pin(rx),
                         timeout=0, timeout_char=0)
        self._dec = codec.Decoder()
        self._de = Pin(de_pin, Pin.OUT, value=0) if de_pin is not None else None
        # 10 bit-times per byte (start + 8 + stop), in microseconds.
        self._byte_us = (10 * 1000000) // baud

    def send(self, msg_id, hdr, payload=b""):
        data = codec.encode_uart(msg_id, hdr, payload)
        if self._de is None:
            self.uart.write(data)
            return

        # Hold the driver on until the last stop bit has actually left the shift
        # register. Releasing early truncates the frame on the wire — and it
        # decodes as a CRC error at the far end, not as anything obviously
        # timing-related, so it is worth getting right here rather than
        # debugging it on an oscilloscope later.
        self._de.value(1)
        try:
            self.uart.write(data)
            if hasattr(self.uart, "txdone"):
                while not self.uart.txdone():
                    pass
            else:
                # No txdone() on this port: wait out the frame, plus a margin.
                # ~1 ms for a 12-byte frame at 115200, inside the 50 ms scan.
                time_mod.sleep_us(self._byte_us * len(data) + 200)
        finally:
            self._de.value(0)

    def poll(self):
        n = self.uart.any()
        if not n:
            return []
        return self._dec.feed(self.uart.read(n) or b"")

    def stats(self):
        return {"bad_crc": self._dec.bad_crc, "bad_len": self._dec.bad_len}


class CanTransport:
    """TWAI/CAN transport — kept, but no longer the planned upgrade path.

    RS485 replaced it: the HMI board already carries a MAX485 with automatic
    direction control, so a differential link needs no protocol change, no new
    driver, and no custom firmware. CAN would need all three on this end, since
    TWAI is not in mainline MicroPython (micropython/micropython#12331) and
    requires a build with the straga/micropython-esp32-twai USER_C_MODULE to get
    machine.CAN. Use UartTransport with de_pin instead.

    Left in place because the message set already fits a CAN frame (payloads are
    capped at 7 bytes for exactly that reason), so if a third node ever joins the
    bus this is a constructor change rather than a rewrite.
    """

    def __init__(self, tx, rx, bitrate=500000):
        from machine import CAN, Pin      # raises on a stock build — intentional
        self.can = CAN(0, tx=Pin(tx), rx=Pin(rx), mode=CAN.NORMAL, baudrate=bitrate)
        self._stats = {"bad_crc": 0, "bad_len": 0}

    def send(self, msg_id, hdr, payload=b""):
        can_id, data = codec.encode_can(msg_id, hdr, payload)
        self.can.send(data, can_id)

    def poll(self):
        msgs = []
        while self.can.any():
            can_id, _rtr, _fmi, data = self.can.recv()
            m = codec.decode_can(can_id, data)
            if m is not None:
                msgs.append(m)
        return msgs

    def stats(self):
        return dict(self._stats)


class LinkSession:
    """Owns sequence numbers, ACKs and liveness for one peer."""

    def __init__(self, transport, timeout_ms=LINK_TIMEOUT_MS):
        self.t = transport
        self.timeout_ms = timeout_ms
        self._seq = 0
        self._last_rx_ms = None      # None = never heard from the peer since boot
        self.rx_count = 0
        self.tx_count = 0

    # ── Outbound ─────────────────────────────────────────────────────────────
    def _next_seq(self):
        self._seq = (self._seq + 1) & codec.SEQ_MASK
        return self._seq

    def send(self, msg_id, payload=b"", want_ack=False):
        hdr = self._next_seq() | (codec.FLAG_ACK if want_ack else 0)
        self.t.send(msg_id, hdr, payload)
        self.tx_count += 1

    def _send_ack(self, seq):
        self.t.send(codec.MSG_ACK, codec.FLAG_RESP, bytes(bytearray([seq])))

    def _send_nack(self, seq, reason):
        self.t.send(codec.MSG_NACK, codec.FLAG_RESP, bytes(bytearray([seq, reason])))

    # ── Inbound ──────────────────────────────────────────────────────────────
    def poll(self):
        """Returns the messages received this cycle, after answering any that
        asked for an ACK. Marks the link alive on any valid frame."""
        out = []
        for msg_id, hdr, payload in self.t.poll():
            self._last_rx_ms = _ticks_ms()
            self.rx_count += 1
            if hdr & codec.FLAG_ACK:
                self._send_ack(hdr & codec.SEQ_MASK)
            out.append((msg_id, hdr, payload))
        return out

    def nack(self, hdr, reason):
        self._send_nack(hdr & codec.SEQ_MASK, reason)

    # ── Liveness ─────────────────────────────────────────────────────────────
    def is_up(self):
        if self._last_rx_ms is None:
            return False
        return _ticks_diff(_ticks_ms(), self._last_rx_ms) < self.timeout_ms

    def silent_ms(self):
        if self._last_rx_ms is None:
            return None
        return _ticks_diff(_ticks_ms(), self._last_rx_ms)


def failsafe_requests():
    """What the control core is told to do when the HMI board is not talking.

    Everything the user asked for is dropped: jets, blower, extra pumps, the
    high-speed winding, the light. What is NOT dropped is the thermostat — it
    does not appear here at all, because heating is driven by the S3's own
    measured temperature against its own persisted setpoint, under the freeze
    permissive. Losing the touchscreen must never mean losing freeze protection
    on a winter night.
    """
    return {
        "xSpaEnable": False,
        "xPumpRequest": False,
        "xPump1HighRequest": False,
        "xPump2Request": False,
        "xPump3Request": False,
        "xJetsRequest": False,
        "xBlowerRequest": False,
        "xLightRequest": False,
    }


def requests_from_msg(payload):
    """Decode MSG_REQ into the control core's input names."""
    if len(payload) < 1:
        return None
    r = payload[0]
    return {
        "xSpaEnable": bool(r & codec.REQ_SPA_ENABLE),
        "xPumpRequest": bool(r & codec.REQ_PUMP),
        "xPump1HighRequest": bool(r & codec.REQ_PUMP1_HIGH),
        "xPump2Request": bool(r & codec.REQ_PUMP2),
        "xPump3Request": bool(r & codec.REQ_PUMP3),
        "xJetsRequest": bool(r & codec.REQ_JETS),
        "xBlowerRequest": bool(r & codec.REQ_BLOWER),
        "xLightRequest": bool(r & codec.REQ_LIGHT),
    }


def status_payload(outputs, inputs, fault_code):
    """Pack MSG_STATUS: outputs, safety inputs, fault."""
    o = 0
    for bit, key in ((codec.OUT_PUMP1_LOW, "xPump1_Low"),
                     (codec.OUT_PUMP1_HIGH, "xPump1_High"),
                     (codec.OUT_PUMP2, "xPump2"),
                     (codec.OUT_PUMP3, "xPump3"),
                     (codec.OUT_HEATER, "xHeater"),
                     (codec.OUT_JETS, "xJets"),
                     (codec.OUT_BLOWER, "xBlower"),
                     (codec.OUT_LIGHT, "xLight")):
        if outputs.get(key):
            o |= bit
    i = 0
    for bit, key in ((codec.IN_FLOW_SWITCH, "xFlowSwitch"),
                     (codec.IN_HIGH_LIMIT_OK, "xHighLimitOK"),
                     (codec.IN_ESTOP_OK, "xRemoteEStopOK"),
                     (codec.IN_TEMP_SENSOR_OK, "xTempSensorOK"),
                     (codec.IN_SPA_ENABLE, "xSpaEnable")):
        if inputs.get(key):
            i |= bit
    f = (codec.FAULT_ACTIVE if fault_code else 0) | (fault_code & codec.FAULT_CODE)
    return bytes(bytearray([o, i, f]))
