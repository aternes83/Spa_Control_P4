#!/usr/bin/env python3
"""
Documentation/pin-map consistency check:

    python3 tools/test_docs.py

docs/WIRING.md claims the pin map is authoritative and that any disagreement is
a bug in the document. This enforces that, because a wiring diagram nobody
re-checks is worse than no diagram — someone will wire a relay to whatever it
says at two in the morning.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "s3_control"))

import board_pins as bp  # noqa: E402

FAILED = []


def check(name, cond, detail=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s %s" % (name, detail))
        FAILED.append(name)


def main():
    wiring = open(os.path.join(ROOT, "docs", "WIRING.md")).read()
    pins = {}
    for d in ("INPUT_PINS", "OUTPUT_PINS", "SENSOR_PINS", "LINK_PINS"):
        for k, v in getattr(bp, d).items():
            if v is not None:
                pins[k] = v

    print("S3 signal tables and diagrams vs board_pins.py")

    # Markdown table rows: | `xJetsRequest` | 17 | ...
    rows = re.findall(r"^\|\s*`(x[A-Za-z0-9_]+)`\s*\|\s*(\d+)\s*\|", wiring, re.M)
    bad = [(n, g) for n, g in rows if pins.get(n) != int(g)]
    check("%d signal table rows match the pin map" % len(rows), not bad, bad[:3])
    check("the input table is actually present", len(rows) >= len(bp.INPUT_PINS),
          len(rows))

    # Diagram lines: GPIO12 xPump1_Low
    diag = re.findall(r"GPIO(\d+)\s+(x[A-Za-z0-9_]+)", wiring)
    bad = [(n, g) for g, n in diag if pins.get(n) != int(g)]
    check("%d diagram labels match the pin map" % len(diag), not bad, bad[:3])
    check("every output appears in a diagram",
          set(bp.OUTPUT_PINS) <= {n for _, n in diag},
          sorted(set(bp.OUTPUT_PINS) - {n for _, n in diag}))

    print("link pins")
    for label, key in (("RX", "UART_RX"), ("TX", "UART_TX"), ("DE", "RS485_DE")):
        want = "GPIO%d" % bp.LINK_PINS[key]
        check("%s shown as %s" % (label, want),
              re.search(re.escape(want) + r"\s*\(%s\)" % label, wiring) is not None)

    print("the transceiver part number is named")
    for part in ("MAX3485CSA+", "THVD1450DR"):
        check("%s appears" % part, part in wiring)
    check("the 5 V part is called out as the one to avoid",
          "MAX485" in wiring and "avoid" in wiring.lower())

    print("the S3 config example matches the firmware that reads it")
    # Somebody will copy this file onto a board at two in the morning. If it has
    # drifted from sensors.py, the tub heats to the wrong temperature and nothing
    # says so — the maths still works, it is just calibrated for another probe.
    import json
    import sensors
    from spa_core import SpaController
    cfg = json.load(open(os.path.join(ROOT, "s3_control", "config.example.json")))

    sp = cfg.get("setpoint_f")
    check("setpoint is inside the window main.py will accept",
          sp is not None and 60.0 <= float(sp) <= 104.0, sp)

    cal = cfg.get("ntc_cal")
    check("carries an ntc_cal", isinstance(cal, dict))
    preset = {k: float(v) for k, v in sensors.NTC_BALBOA_M7_CAL.items()}
    check("and it is still NTC_BALBOA_M7_CAL, the probe in this tub",
          cal == preset, sorted(set(preset) ^ set(cal or {})) or cal)

    # Fed through the sensor exactly as main.py feeds it.
    probe = sensors.NTCSensor(bp.SENSOR_PINS["NTC_ADC"], cal, adc=object())
    check("the calibration point round-trips: r0 reads back as t0",
          abs(probe._r_to_f(probe.r0) - (probe._t0_c * 9.0 / 5.0 + 32.0)) < 0.05,
          probe._r_to_f(probe.r0))

    check("the note survives the firmware rewriting the file",
          "_comment" in json.loads(json.dumps(dict(cfg, setpoint_f=102.0))))

    print()
    print("the app's MQTT wire contract")
    mqtt_c = open(os.path.join(ROOT, "p4_hmi", "main", "mqtt_spa.c")).read()
    mqtt_md = open(os.path.join(ROOT, "docs", "MQTT.md")).read()

    # Every JSON key the firmware actually puts on the wire, and every key it
    # reads back off it.
    published = set(re.findall(r'\\"([A-Za-z0-9_-]+)\\":', mqtt_c))
    consumed = set(re.findall(r'GetObjectItemCaseSensitive\(root, "([A-Za-z0-9_-]+)"\)', mqtt_c))

    # docs/MQTT.md is the specification — the app already speaks this protocol,
    # so the document is not ours to edit to match the code. Any key here that
    # the document does not list is a key the app will ignore.
    documented = set(re.findall(r'^\| `([a-z0-9_]+)`', mqtt_md, re.M))
    documented |= set(re.findall(r'`([a-z0-9_]+)`,? `([a-z0-9_]+)`', mqtt_md) and
                      [m for pair in re.findall(r'\| `([a-z0-9_]+)`, `([a-z0-9_]+)`', mqtt_md)
                       for m in pair])

    # "link" is ours, not the app's: deliberately extra, and mqtt_spa.c says why.
    undocumented = published - documented - {"link"}
    check("%d published fields are all in docs/MQTT.md" % len(published),
          not undocumented, sorted(undocumented))
    undocumented = consumed - documented
    check("%d accepted commands are all in docs/MQTT.md" % len(consumed),
          not undocumented, sorted(undocumented))

    # The failure docs/MQTT.md calls "the single easiest way to break this
    # silently": the app decodes with .convertFromSnakeCase, so a camelCase key
    # does not error, it decodes as absent and the app shows a stale value.
    camel = sorted(k for k in published | consumed if k != k.lower() or "-" in k)
    check("no key is camelCase or hyphenated", not camel, camel)

    # The two fields the S3 cannot report, because this board applies them.
    for k in ("eco", "max_jet"):
        check("%s is published (the P4 owns it)" % k, k in published)

    # Commands the firmware must NOT silently pretend to honour.
    check("set_temp_cal is not quietly accepted",
          "set_temp_cal" not in consumed or "needs a SpaLink message" in mqtt_c)

    print()
    print("the app's BLE provisioning contract")
    ble_c = open(os.path.join(ROOT, "p4_hmi", "main", "ble_prov.c")).read()
    ble_md = open(os.path.join(ROOT, "docs", "BLE.md")).read()

    # The UUIDs are built from a macro in little-endian byte order, because that
    # is what NimBLE wants. Getting that backwards produces a service no phone
    # can find, with nothing in any log to say why - so reconstruct the UUID the
    # firmware will actually advertise and compare it to the documented one.
    m = re.search(r"BLE_UUID128_INIT\(([^)]*)\)", ble_c, re.S)
    check("the NUS uuid macro is present", m is not None)
    if m:
        parts = [t.replace("\\", " ").strip() for t in m.group(1).split(",")]
        # b1/b0 are the macro's parameters; the service is 0x00, 0x01.
        concrete = ["0x01" if t == "b0" else "0x00" if t == "b1" else t for t in parts]
        try:
            raw = [int(t, 16) for t in concrete]
        except ValueError:
            raw = []
        check("the uuid macro has 16 bytes", len(raw) == 16, len(raw))
        if len(raw) == 16:
            be = raw[::-1]                       # little-endian -> canonical
            hx = "".join("%02X" % b for b in be)
            built = "%s-%s-%s-%s-%s" % (hx[0:8], hx[8:12], hx[12:16], hx[16:20], hx[20:32])
            documented = re.findall(r"`(6E400001-[0-9A-F-]+)`", ble_md)
            check("the service uuid the firmware builds matches docs/BLE.md",
                  documented and built == documented[0], built)

    # The app scans with no service filter and matches the advertised local
    # name. Change this string and the wizard simply never finds the tub.
    name = re.search(r'#define DEV_NAME "([^"]+)"', ble_c)
    check("the advertised name is SpaControl",
          name is not None and name.group(1) == "SpaControl",
          name.group(1) if name else "missing")
    check("and docs/BLE.md says the same", name and ("`%s`" % name.group(1)) in ble_md)

    # Commands accepted, and replies emitted.
    accepts = set(re.findall(r'GetObjectItemCaseSensitive\(root, "([a-z_]+)"\)', ble_c))
    for k in ("wifi_scan", "wifi_ssid", "broker_get", "tz"):
        check("accepts %s" % k, k in accepts)

    # The timezone must be a POSIX TZ string, never an IANA name: newlib on the
    # ESP32 has no timezone database and would take "America/New_York" without
    # complaint, then ignore it. The app is the side that converts, so the two
    # documents have to agree that it converts.
    check("docs/BLE.md says POSIX, not IANA",
          "POSIX TZ string" in ble_md and "IANA" in ble_md)
    check("the app converts rather than sending an identifier",
          "SpaTimeZone" in ble_md)

    # A command without tz must not wipe a stored one - an older app build would
    # otherwise silently un-set a correct timezone every time it provisioned.
    prov = ble_c[ble_c.find("case JOB_PROVISION:"):ble_c.find("case JOB_TZ:")]
    check("provisioning without a timezone leaves the stored one alone",
          "if (job.tz[0])" in prov)

    for frag in ('{\\"scan\\":\\"begin\\"}', '{\\"scan\\":\\"end\\"}',
                 '{\\"wifi\\":\\"connecting\\"}'):
        check("emits %s" % frag.replace('\\', ''), frag in ble_c)

    # The wizard spins until "end" arrives, so it must be unconditional.
    scan_body = ble_c[ble_c.find("case JOB_SCAN:"):ble_c.find("case JOB_PROVISION:")]
    ends = scan_body.count('scan\\":\\"end')
    before_end = scan_body[:scan_body.find('scan\\":\\"end')]
    check("the scan is always closed, even when it fails", ends == 1, ends)
    check("and nothing can leave the scan before closing it",
          "return" not in before_end and "break" not in before_end)

    print()
    if FAILED:
        print("FAILED: %d" % len(FAILED))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
