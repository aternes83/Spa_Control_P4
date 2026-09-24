"""
Spa_Control_P4 — ESP32-S3 control node.

This board owns the plant and nothing else: it reads the sensors and the wired
panel inputs, runs the control core, and drives the relays. It has no radio, no
display and no network stack. The P4 HMI board asks it for things over SpaLink;
it decides whether to honour them.

The ordering inside the loop matters and is the safety argument for the whole
two-board split:

  1. read hardware
  2. fold in the P4's requests, or the failsafe set if the link is down
  3. step the control core
  4. write outputs
  5. tell the P4 what happened

Step 3 never depends on the link being up. If the P4 is unplugged, mid-reflash,
or its cable is cut, the S3 keeps its thermostat, its interlocks and its freeze
protection; it just stops honouring user requests. Nothing here blocks.
"""

import sys

sys.path.append("/link")

import board_pins as bp
import spalink_session as link
from spa_core import SpaController, ticks_ms, ticks_diff, ticks_add
from sensors import NTCSensor, NTC_DEFAULT_CAL

try:
    import spalink_codec as codec
except ImportError:
    from link import spalink_codec as codec

from machine import Pin, WDT

CONFIG_PATH = "config.json"
CONTROL_LOOP_MS = 50            # 20 Hz scan, as on the single-board firmware
STATE_TX_MS = 200               # 5 Hz state broadcast to the HMI
HELLO_TX_MS = 2000              # slow identity beacon, so a rebooted P4 resyncs
WDT_TIMEOUT_MS = 20000          # control-loop hang → reboot rather than a dead tub

SETPOINT_MIN_F = 60.0
SETPOINT_MAX_F = 104.0          # above the 105 °F max_safe_temp_f trip point
FW_VERSION = (0, 1)


def _load_config():
    try:
        import json
        with open(CONFIG_PATH) as f:
            return json.load(f)
    except Exception:
        return {}


def _save_config(cfg):
    """Best-effort persistence. A failed write must never stop the control loop —
    a stale setpoint on the next boot is survivable, a crashed heater is not."""
    try:
        import json
        with open(CONFIG_PATH, "w") as f:
            json.dump(cfg, f)
        return True
    except Exception as e:
        print("config: save failed: %s" % e)
        return False


def _init_io():
    """Request inputs pull DOWN, interlocks pull UP. The difference is
    deliberate.

    The panel switches source 3V3 (docs/WIRING.md section 5), so an open or
    absent switch has to read as "not requested". With a pull-up it reads
    asserted whether the switch is open or closed, which means a fitted switch
    cannot do anything at all and a board with nothing wired boots asking for
    every output at once — which is exactly what it did on the bench.

    The interlocks keep the pull-up inherited from v2.0, so a cut wire still
    reads OK. That is not safe, and it is why the hardware chain in section 6 is
    not optional; flipping it is a deliberate change that also requires the
    interlock contacts to source 3V3, so it stays with whoever wires them."""
    ins = {}
    for name, gpio in bp.INPUT_PINS.items():
        pull = Pin.PULL_UP if name in bp.SAFETY_INPUTS else Pin.PULL_DOWN
        ins[name] = Pin(gpio, Pin.IN, pull)
    outs = {name: Pin(gpio, Pin.OUT, value=0)
            for name, gpio in bp.OUTPUT_PINS.items()}
    return ins, outs


def _read_inputs(ins):
    return {name: bool(pin.value()) for name, pin in ins.items()}


def _open_link():
    """The link is a UART either way — direct TTL on the bench, RS485 for the
    install. Only board_pins.LINK_PINS["RS485_DE"] changes: set it to the DE pin
    of the transceiver on this board, or None for a direct connection or an
    auto-direction module. See docs/HARDWARE.md."""
    return link.LinkSession(link.UartTransport(
        bp.LINK_UART_ID, bp.LINK_PINS["UART_TX"], bp.LINK_PINS["UART_RX"],
        bp.LINK_BAUD, de_pin=bp.LINK_PINS.get("RS485_DE")))


def main():
    print("board: %s  fw %d.%d" % (bp.BOARD_REVISION, FW_VERSION[0], FW_VERSION[1]))

    cfg = _load_config()
    ctrl = SpaController()
    ctrl.temp_setpoint_f = float(cfg.get("setpoint_f", ctrl.temp_setpoint_f))

    # No ceiling on a spa-enable session. The P4 is the only control surface and
    # has no off switch, so it asserts xSpaEnable from boot and never withdraws
    # it; with the ceiling in place the permissive would close four hours later
    # and every control on the panel would go dead with no way to recover from
    # the screen.
    #
    # What the ceiling was protecting against is still covered, and better. It
    # guarded a tub left enabled by someone who walked away — but here enable
    # comes from a board, not a person, and the moment that board stops talking
    # the link failsafe drops enable along with every other request, inside
    # 1.5 s, whether the HMI crashed, was unplugged or is being reflashed. That
    # is a guard nobody has to remember. The thermostat is unaffected either way:
    # it runs under the freeze permissive, which never consulted this timer.
    #
    # Put the ceiling back if a wired panel switch is ever fitted in parallel,
    # because then a person can hold enable and the failsafe cannot.
    ctrl.default_run_ms = None

    ins, outs = _init_io()
    sensor = NTCSensor(bp.SENSOR_PINS["NTC_ADC"], cfg.get("ntc_cal", NTC_DEFAULT_CAL))
    session = _open_link()

    wdt = WDT(timeout=WDT_TIMEOUT_MS)

    # Requests owned by the HMI. Seeded to the failsafe set so a boot with a dead
    # link starts safe rather than starting with whatever the last session wanted.
    hmi_requests = link.failsafe_requests()
    modes = 0
    link_was_up = False
    echo_warned = False
    setpoint_dirty = False

    now = ticks_ms()
    next_state_ms = now
    next_hello_ms = now
    next_loop_ms = now

    while True:
        now = ticks_ms()
        wdt.feed()

        # ── 1. Hardware ──────────────────────────────────────────────────────
        inputs = _read_inputs(ins)
        sensor.poll(now)
        temp_f = sensor.read_f()
        inputs["rWaterTemp_F"] = temp_f if temp_f is not None else 70.0
        inputs["xTempSensorOK"] = not sensor.faulted()

        # ── 2. HMI requests, or the failsafe set ─────────────────────────────
        for msg_id, hdr, payload in session.poll():
            if msg_id == codec.MSG_REQ:
                r = link.requests_from_msg(payload)
                if r is None:
                    session.nack(hdr, codec.NACK_BAD_LEN)
                else:
                    hmi_requests = r
                    modes = payload[1] if len(payload) > 1 else 0
            elif msg_id == codec.MSG_SETPOINT:
                if len(payload) < 2:
                    session.nack(hdr, codec.NACK_BAD_LEN)
                    continue
                sp = codec.dF_to_f(codec.unpack_i16(payload))
                if not (SETPOINT_MIN_F <= sp <= SETPOINT_MAX_F):
                    # Refuse rather than clamp: a clamped setpoint the HMI does not
                    # know about would leave the two boards showing different numbers.
                    session.nack(hdr, codec.NACK_REFUSED)
                    continue
                if sp != ctrl.temp_setpoint_f:
                    ctrl.temp_setpoint_f = sp
                    setpoint_dirty = True
            elif msg_id == codec.MSG_CLEAR_FAULT:
                # Faults here are all recomputed from live conditions, so there is
                # no latch to clear. ACK so the HMI's button feels answered.
                pass
            elif msg_id in (codec.MSG_PING, codec.MSG_ACK, codec.MSG_NACK):
                pass
            else:
                session.nack(hdr, codec.NACK_UNKNOWN_ID)

        link_up = session.is_up()
        if link_up != link_was_up:
            # The counters go out with the state change because this board has
            # no screen: "DOWN" with rx=0 is a link that never worked, "DOWN"
            # with rx climbing and crc climbing is a link that is failing now,
            # and the two want different tools.
            st = session.t.stats()
            print("link: %s  rx=%d tx=%d echo=%d bad_crc=%d bad_len=%d" % (
                "up" if link_up else "DOWN — failsafe requests",
                session.rx_count, session.tx_count, session.echo_count,
                st.get("bad_crc", 0), st.get("bad_len", 0)))
            link_was_up = link_up
        if session.echo_count and not echo_warned:
            # Once, loudly. Hearing our own frames means the pair is looping
            # back — a driver-enable stuck on, or a bench loopback — and the P4
            # cannot be heard at all while that is true. This used to present as
            # a working link, because the echo refreshed the liveness clock.
            echo_warned = True
            print("link: hearing our own frames (%d). The pair is looping back; "
                  "check the transceiver's DE/EN pin. The P4 is not being "
                  "heard." % session.echo_count)
        if not link_up:
            hmi_requests = link.failsafe_requests()
            modes = 0

        # A wired panel switch and the touchscreen are both valid ways to ask.
        for key, value in hmi_requests.items():
            inputs[key] = bool(inputs.get(key, False)) or bool(value)

        # ── 3. Control core — never gated on the link ────────────────────────
        # requests_valid says whether hmi_requests is the panel's actual wish or
        # the failsafe substitute. The core needs the difference: a dropped link
        # must stop the outputs, but must not read as the user releasing a
        # button and hand the runtime ceilings a fresh 20 minutes.
        outputs = ctrl.step(inputs, requests_valid=link_up)

        # ── 4. Drive the plant ───────────────────────────────────────────────
        for name, pin in outs.items():
            pin.value(1 if outputs.get(name) else 0)

        # ── 5. Report ────────────────────────────────────────────────────────
        if ticks_diff(now, next_state_ms) >= 0:
            next_state_ms = ticks_add(now, STATE_TX_MS)
            session.send(codec.MSG_STATUS,
                         link.status_payload(outputs, inputs, outputs["iFaultCode"]))
            session.send(codec.MSG_TEMP,
                         codec.pack_i16(codec.f_to_dF(inputs["rWaterTemp_F"]))
                         + codec.pack_i16(codec.f_to_dF(ctrl.temp_setpoint_f)))
            session.send(codec.MSG_TIMERS,
                         codec.pack_u16(ctrl.spa_remaining_s())
                         + codec.pack_u16(ctrl.light_remaining_s()))

        if ticks_diff(now, next_hello_ms) >= 0:
            next_hello_ms = ticks_add(now, HELLO_TX_MS)
            session.send(codec.MSG_HELLO,
                         bytes(bytearray([codec.PROTO_VERSION,
                                          FW_VERSION[0], FW_VERSION[1], 0x01])))

        # Persist the setpoint outside the hot path, and only when it changed.
        if setpoint_dirty:
            cfg["setpoint_f"] = ctrl.temp_setpoint_f
            if _save_config(cfg):
                setpoint_dirty = False

        # ── Pace the loop ────────────────────────────────────────────────────
        next_loop_ms = ticks_add(next_loop_ms, CONTROL_LOOP_MS)
        slack = ticks_diff(next_loop_ms, ticks_ms())
        if slack > 0:
            import utime
            utime.sleep_ms(slack)
        else:
            # Overran the budget; resynchronise instead of spiralling.
            next_loop_ms = ticks_ms()


if __name__ == "__main__":
    main()
