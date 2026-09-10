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

    print()
    if FAILED:
        print("FAILED: %d" % len(FAILED))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
