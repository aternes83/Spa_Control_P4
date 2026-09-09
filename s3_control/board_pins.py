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
# UART is the bring-up transport; CAN pins are reserved now so the harness does
# not have to be rebuilt when the link moves to a differential pair. See
# docs/HARDWARE.md for the interlink cable and the CAN transceiver notes.
LINK_PINS = {
    "UART_TX": 47,
    "UART_RX": 48,
    "CAN_TX": 42,        # to transceiver TXD (e.g. SN65HVD230 / TJA1051T/3)
    "CAN_RX": 2,         # from transceiver RXD
}
LINK_UART_ID = 1         # UART0 is the console; UART1 is the link
LINK_BAUD = 115200

# Unassigned and available: GPIO1, GPIO3, GPIO45 (input-only), GPIO46 (strap).
