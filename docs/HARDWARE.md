# Hardware

Two boards, one cable.

```
        ┌─────────────────────────────┐        ┌──────────────────────────────┐
        │  ESP32-S3-DevKitC-1-N8R8    │        │  Guition JC-ESP32P4-M3      │
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

### Module vs carrier

The silkscreen reads **Guition JC-ESP32P4-M3**. That is the *module* — the
ESP32-P4 + ESP32-C6-MINI system-on-module with 32 MB PSRAM and 16 MB flash. It
is soldered to a 4.3" display *carrier*, and the two have different levels of
certainty:

* **Module pins are settled.** The C6 radio sits on SDIO — CLK 18, CMD 19,
  D0–D3 on 14–17, reset on GPIO54 — for any board this module is fitted to.
* **Carrier pins are provisional.** Guition ships several carriers for this
  module and they are not pin-compatible. The JC-ESP32P4-M3-DEV board, for
  instance, puts an Ethernet PHY on GPIO31/50/51/52 — the very pins that are free
  header I/O on the 4.3" display carrier.

The carrier data below comes from the published
[JC4880P443C_I_W schematics](https://github.com/ultramcu/guition-jc4880p443c-i-w),
which is the closest documented match for a 4.3" 480×800 carrier. Confirm it
against your board.

### Confirming your carrier in two minutes

Three things to look for, no instruments needed:

1. ~~**RS-485.**~~ **CONFIRMED on this board:** an **SP485E** 8-pin SOIC beside
   the 4-pin `Ao`/`Bo` connector. That is the Sipex/Exar half-duplex RS-485
   transceiver, pin-for-pin the MAX485 on the schematic (`U8`): RO, /RE, DE, DI,
   GND, A, B, VCC. The install link is RS-485 on GPIO26/27, with nothing to add
   to the HMI board.
With that answered, the remaining two no longer block anything — the link lives
on GPIO26/27, so the header is not on the critical path:

2. **The Expand-IO header, `JP1`.** A 26-pin (2×13) header. Pin 1 is 3V3 and
   pin 2 is 5V; it breaks out GPIO28–35 and GPIO49–52, and the C6's UART0 plus
   boot/reset for reflashing the radio. Only needed if you want a TTL bench link
   before fitting the S3's transceiver, or for future I/O.
3. **GPIO35.** Listed both as the boot button and on JP1. If it appears on JP1
   *and* a button, leave it alone — there are eleven other free pins.

## The interlink cable

**Bench — direct TTL.** Three wires: S3 TX → P4 RX, S3 RX → P4 TX, common
ground. Both boards are 3.3 V logic, so no level shifting. Keep it under about a
metre. If the boards are on separate supplies the ground wire is not optional.

**Install — RS-485, using the transceiver already on the HMI board.** Confirmed
present: an **SP485E on UART1
(GPIO26 TX / GPIO27 RX)**, and its DE//RE is driven from the TX line itself
through a 74LVC1G132 and a transistor. That is automatic direction control — the
P4 needs no direction GPIO and no turnaround code.

**Fit a 3.3 V transceiver at the S3 end** — MAX3485, SP3485 or THVD1450, powered
from 3.3 V. Not for the bus (A/B levels interoperate between 3.3 V and 5 V parts;
that is the point of differential signalling) but for the receive pin: the
SP485E is a 5 V part, so a 5 V transceiver at the S3 end would drive its RO
output 0–5 V into a 3.3 V GPIO. The HMI carrier gets away with it via a 1 k
series resistor into the P4's clamp diode; there is no reason to repeat that
here when the 3.3 V part costs the same.

So the install link is:

```
S3  GPIO47 TX ──┐                                    ┌── GPIO26  P4 (UART1)
S3  GPIO48 RX ──┤ MAX485 ══ A/B twisted pair ══ MAX485 ├── GPIO27
S3  GPIO1  DE ──┘ (add one)      120 Ω each end   (on board)
```

Add one transceiver at the S3 end. If it is a bare MAX485 breakout, tie DE and
/RE together to `LINK_PINS["RS485_DE"]` — `UartTransport` raises it around each
frame and, importantly, holds it until the last stop bit has actually shifted
out. Release it early and the frame is truncated on the wire, which shows up at
the far end as a CRC error rather than anything that looks like a timing bug.
Fit an auto-direction module instead and set that pin to `None`.

**Termination and bias.** The HMI board's MAX485 has bias resistors but no 120 Ω
termination. Fit 120 Ω across A/B at each end of the run.

### Why not CAN

The original plan was CAN, and the protocol is still shaped for it — payloads
are capped at 7 bytes so every message fits one classic frame. RS-485 turned out
to be strictly better here:

* the transceiver is already fitted on the HMI board, and switches itself;
* it is the same async UART on both sides, so no new driver and **no protocol
  change at all**;
* it avoids a custom MicroPython build. TWAI is not in mainline
  (micropython/micropython#12331) and would need the S3 rebuilt with the
  [`straga/micropython-esp32-twai`](https://github.com/straga/micropython-esp32-twai)
  `USER_C_MODULE` to get `machine.CAN`.

RS-485 gives the same differential noise immunity over this run for none of that
cost. `CanTransport` and the reserved pins stay in the tree in case a third node
ever joins the bus, where CAN's arbitration would start to earn its keep.

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
