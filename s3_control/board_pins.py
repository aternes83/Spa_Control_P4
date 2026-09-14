"""
BOARD PIN MAP — ESP32-S3-DevKitC-1-N8R8, control node.

Every GPIO this firmware touches is defined here and nowhere else. A board spin
changes this file only.

What moved relative to openplc-hot-tub v2.0: the ST7796S display and XPT2046
touch are gone — the HMI is now the P4 board across the link — which frees
GPIO1/2/3/42/43/44/45/47/48. The link takes two of them; the rest are spare.

ESP32-S3 constraints to respect when re-assigning:
- GPIO0 is a boot strap — safe only for 1-Wire, which idles high via pull-up.
- GPIO19/GPIO20 are USB D-/D+ — unusable while USB is in use.
- GPIO26..GPIO37 are tied to the module's flash/PSRAM on N8R8 — not user I/O.
- GPIO43/GPIO44 are the ROM UART0 console pins. MicroPython's REPL is on native
  USB CDC so they look free, but a serial-console boot or a ROM download would
  fight the link for them. Left unused on purpose.
- GPIO45/GPIO46 are strap/input-only — inputs and IRQs only, never outputs.
- Analog must be on ADC1 (GPIO1..GPIO10); ADC2 is unusable while WiFi is up.
  (This node has no radio at all now, but the constraint costs nothing to keep.)
"""

BOARD_REVISION = "spa-p4-s3-control-r1"

# ── Digital inputs — pulled up, active-high by default ───────────────────────
INPUT_PINS = {
    "xSpaEnable": 4,
    "xPumpRequest": 5,
    "xPump1HighRequest": 7,
    "xPump2Request": 15,
    "xPump3Request": 16,
    "xJetsRequest": 17,
    "xBlowerRequest": 18,
    "xLightRequest": 8,
    "xFlowSwitch": 9,
    "xHighLimitOK": 10,
    "xRemoteEStopOK": 11,
}

# ── Digital outputs — relay/contactor board, all idle low ────────────────────
OUTPUT_PINS = {
    "xPump1_Low": 12,
    "xPump1_High": 13,
    "xPump2": 14,
    "xPump3": 21,
    "xHeater": 38,
    "xJets": 39,
    "xBlower": 40,
    "xLight": 41,
}

# ── Temperature sensing ──────────────────────────────────────────────────────
SENSOR_PINS = {
    "NTC_ADC": 6,        # ADC1_CH5
    "DS18B20_1W": 0,     # legacy 1-Wire probe
}

# ── SpaLink to the P4 HMI board ──────────────────────────────────────────────
# The link is a plain async UART on this side in both wiring schemes, because
# the HMI board carries an on-board MAX485: bench wiring is single-ended between
# the two UARTs, install wiring puts an RS485 transceiver on this end and runs a
# differential pair to the P4's. Same pins, same protocol, same driver — only the
# physical layer changes. See docs/HARDWARE.md.
LINK_PINS = {
    "UART_TX": 47,
    "UART_RX": 48,
    # Direction control for the RS485 transceiver on THIS board — a MAX3485
    # (3.3 V; see docs/WIRING.md for why not a 5 V MAX485). On a breakout module
    # this is the pin marked EN; on a bare SOIC-8 it is pins 2 and 3, /RE and DE,
    # tied together. The P4's on-board SP485E needs no such pin — its DE//RE is
    # driven from TX through a 74LVC1G132 — but a MAX3485 does. Set to None only
    # for a true auto-direction module with no EN pin at all; UartTransport
    # handles either. Fit a 10k pull-down so the module boots listening rather
    # than jamming the bus while this pin is still floating.
    "RS485_DE": 1,       # module "EN"; bare chip /RE + DE
    # Reserved. CAN is no longer the planned upgrade path — RS485 gives the same
    # noise immunity over this run, needs no protocol change, and avoids a custom
    # MicroPython build (TWAI is not in mainline). Kept only so the pins are not
    # reused if that ever changes.
    "CAN_TX": 42,
    "CAN_RX": 2,
}
LINK_UART_ID = 1         # UART0 is the console; UART1 is the link
LINK_BAUD = 115200

# Unassigned and available: GPIO3, GPIO45 (input-only), GPIO46 (strap).
