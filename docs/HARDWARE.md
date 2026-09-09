# Hardware

Two boards, one cable.

```
        ┌─────────────────────────────┐        ┌──────────────────────────────┐
        │  ESP32-S3-DevKitC-1-N8R8    │        │  ESP32-P4-WIFI6-Touch-LCD-4.3│
        │  control node               │        │  HMI node                    │
        │                             │        │                              │
  NTC ──┤ GPIO6  ADC1_CH5             │        │   480x800 MIPI-DSI, cap touch│
 flow ──┤ GPIO9                       │ SpaLink│   ESP32-C6 → WiFi 6 / BLE 5  │
 hi-lim─┤ GPIO10          GPIO47 TX ──┼────────┼── RX  (UART1)                │
 e-stop─┤ GPIO11          GPIO48 RX ──┼────────┼── TX                         │
        │                             │   GND ─┼──                            │
        │ 8 relay outputs             │        │   no relays, no interlocks   │
        └──────────┬──────────────────┘        └──────────────────────────────┘
                   │
          pumps, blower, heater, light
```

The S3 owns the plant. The P4 owns the screen and the radio. Everything the P4
wants is a request the S3 is free to refuse.

## Pin maps

Both are single blocks, and they are the only place GPIO numbers appear:

* S3 — `s3_control/board_pins.py`
* P4 — `p4_hmi/main/board_pins.h`

The S3 map is inherited from the v2.0 firmware with the display and touch pins
removed (the HMI moved across the link), which frees GPIO1/2/3/42/43/44/45/47/48.

> **The P4 pin numbers are unverified.** They were chosen from Waveshare's
> published 40-pin header list, not from the schematic. Check them against the
> board before wiring, and fix `board_pins.h` rather than the call sites.
> Confirmed from the vendor documentation: MIPI-DSI 2-lane 480×800 display,
> touch I²C on GPIO7/GPIO8, and an ESP32-C6-MINI radio co-processor on SDIO.

## The interlink cable

**Bring-up — UART.** Three wires: S3 TX → P4 RX, S3 RX → P4 TX, and a common
ground. Both boards are 3.3 V logic, so no level shifting. Keep it under about a
metre and away from the contactor wiring; if the two boards are on separate
supplies, the ground wire is not optional.

**Install — CAN.** Once the boards are metres apart in a cabinet with pump
contactors switching inductive loads, single-ended UART is the wrong tool. Fit a
3.3 V transceiver at each end — SN65HVD230 or TJA1051T/3 — and run twisted pair
with 120 Ω termination at both ends. 500 kbit/s is far more than this protocol
needs and is comfortable over any length this tub will involve.

The protocol does not change: see the CAN mapping in `docs/PROTOCOL.md`.

### Before CAN can work on the S3

TWAI is **not in mainline MicroPython** (micropython/micropython#12331). The S3
needs a firmware built with the
[`straga/micropython-esp32-twai`](https://github.com/straga/micropython-esp32-twai)
`USER_C_MODULE`, which provides `machine.CAN`. `spalink_session.CanTransport`
targets that API and raises on a stock build, deliberately and loudly.

The P4 side has no such problem — TWAI is a first-class ESP-IDF driver.

This asymmetry is the main reason the link starts on UART: it lets the whole
system be brought up and tested before anyone rebuilds a MicroPython port.

## Sensor

NTC thermistor on ADC1, wired as a divider:

```
3.3V ── R_fixed ──┬── GPIO6 (ADC1_CH5) ── NTC ── GND
```

The tub's existing Balboa M7 probe is a 30 k NTC; its preset is
`sensors.NTC_BALBOA_M7_CAL`. `NTC_DEFAULT_CAL` is a generic 10 k Beta-3950 part.
Above ~500 kΩ the probe reads as open and below 200 Ω as shorted; either raises
`FAULT_TEMP_SENSOR` and the heater is refused.

The DS18B20 1-Wire path from the old firmware is not carried over — its bit-bang
timing disables interrupts for milliseconds per read, which a control loop and a
framed serial link both do without gladly.

## Safety wiring unchanged from v2.0

The high-limit thermostat, flow switch and remote e-stop are wired to the S3 and
read every 50 ms scan. They gate the heater in software, but they should also
remain in series with the heater contactor coil in hardware. **Software is the
second line of defence here, not the first.**
