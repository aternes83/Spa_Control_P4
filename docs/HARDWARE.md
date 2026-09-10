# Hardware

Two boards, one cable. Point-to-point diagrams and the parts list are in
**[WIRING.md](WIRING.md)**; this file covers what the hardware *is* and why.

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

### Sources

Everything below is verified against the vendor documentation package
(schematic `JC4880P443_V1.0`, the `esp32_p4_function_ev_board` BSP, and the
`uart_echo_rs485` example under `1-Demo/idf_examples`). The package is ~545 MB
and is **not** in git — see `.gitignore`. Each group names the sheet it came
from so a claim can be re-checked without re-reading the whole drop.

* Module: **Guition JC-ESP32P4-M3** — ESP32-P4 + ESP32-C6-MINI, 32 MB PSRAM,
  16 MB flash. The P4↔C6 SDIO link is internal to the module and never reaches a
  carrier pin; only the C6's UART0 and boot/reset are broken out, for reflashing
  the radio.
* Carrier: **Guition JC4880P443C_I_W** — 4.3" 480×800 IPS, ST7701S over
  MIPI-DSI, GT911 touch, SP485E RS-485, ES8311 codec, microSD, camera header.

### Display and touch (sheets 2 and 3, plus the BSP)

| | |
|---|---|
| Panel | ST7701S, MIPI-DSI, **2 lanes @ 1000 Mbps** |
| Resolution | **480×800 portrait**, RGB565 |
| Timings | HSYNC 1344, HBP 160, HFP 160 / VSYNC 635, VBP 23, VFP 12 |
| DSI PHY power | LDO_VO3 at 2500 mV |
| LCD reset | GPIO5 |
| Backlight | GPIO23 → MP3202 enable; PWM it for dimming |
| Touch | GT911 at I²C `0x5D` on **GPIO7 (SDA) / GPIO8 (SCL)**, 5k1 pull-ups |
| Touch INT / RST | GPIO21 / GPIO22 — wired, but the BSP leaves both `NC` and drives the controller over I²C alone |

The I²C bus is shared with the ES8311 codec and the RTC. The BSP selects these
timings under `CONFIG_BSP_LCD_TYPE_1024_600`, whose name is misleading — the
values inside are this board's 480×800.

Note the orientation: **480×800 portrait**, where the old ST7796 HMI was 480×320
landscape. The screen is a fresh layout, not a port.

### Free user I/O — Expand-IO header JP1 (sheet 4)

26-pin header. Pin 1 is 3V3, pin 2 is 5V, GND on both sides. It breaks out:

```
GPIO28 29 30 31 32 33 34 49 50 51 52    11 usable pins
GPIO35                                  DO NOT USE — BOOTMODE strap
ES_I2C_SDA / ES_I2C_SCL                 GPIO7/8, the shared I2C bus
C6_U0RXD / C6_U0TXD / C6_IO9 / C6_CHIP_PU   for reflashing the radio
```

> **Correction to an earlier note in this file.** Sheet 3 labels GPIO28–35 and
> 49–52 with RMII Ethernet names — RMII_TXD0/1, MDC, MDIO, PHY_RSTN, RMII_CLK,
> RMII_TXEN. That is the *module's* alternate-function legend, inherited from
> the reference design and actually used on Guition's JC-ESP32P4-M3-DEV carrier.
> **There is no Ethernet PHY on this carrier** — sheets 1–7 contain only power,
> LCD/CSI, the module, USB/IO, RS-485, codec/TF-card and the microphone — so
> here they are plain GPIO. Eleven of them, which matches Guition's own claim of
> "11 accessible unused programmable GPIOs".

## The interlink cable

**Install — RS-485, and the HMI end is already built.** Sheet 3 gives
**GPIO26 = TX1, GPIO27 = RX1**; sheet 5 runs those to an **SP485E** at `U8`,
the MAX485-compatible transceiver you found on the board. Its DE and /RE are
tied together and driven from the TX line through a 74LVC1G132 (`U7`) and a
transistor (`U9`) — automatic direction control. No GPIO, no turnaround code,
and because /RE follows DE the receiver is off while transmitting, so there is
no self-echo to filter out.

The vendor's own `uart_echo_rs485` example agrees exactly: `TXD 26`, `RXD 27`,
`RTS = UART_PIN_NO_CHANGE`, `uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`. That
example is also the fastest possible first-light test for the physical layer,
before any of our code is involved.

The bus comes out on **`J4`**, a 4-pin connector carrying two A/B pairs so nodes
can daisy-chain, protected by SMAJ6.5CA TVS clamps and 0.1 A resettable fuses.

```
S3  GPIO47 TX ──┐                                       ┌── GPIO26  P4 UART1
S3  GPIO48 RX ──┤ MAX3485 ══ A/B twisted pair ══ SP485E ─┤── GPIO27
S3  GPIO1  DE ──┘  (add)      120 Ω each end    (fitted, self-directing)
```

**Fit a 3.3 V transceiver at the S3 end** — **`MAX3485CSA+`** (SOIC-8) is the
default; `THVD1450DR` if you want ±18 kV ESD and bus-fault protection next to
the contactors; the `SP3485` family is the direct 3.3 V sibling of the P4's
SP485E. Full part list and pin-by-pin diagrams: **[WIRING.md](WIRING.md)**.
Not for the bus (A/B levels interoperate between 3.3 V and 5 V parts; that is the point
of differential signalling) but for the receive pin: the SP485E is a 5 V part,
so a 5 V transceiver at the S3 end would drive its RO output 0–5 V into a 3.3 V
GPIO. The carrier gets away with that through `R70`, a 1 k series resistor into
the P4's clamp diode. There is no reason to repeat it when the 3.3 V part costs
the same.

**Termination.** The carrier has bias resistors but no 120 Ω termination. Fit
120 Ω across A/B at each end of the run.

**Bench alternative — TTL.** Build the P4 with `SPALINK_BENCH_TTL` to move the
link to JP1 pins (GPIO32/33) and skip both transceivers. Optional: with the S3
transceiver in hand you can go straight to RS-485.

### Why not CAN

The original plan was CAN, and the protocol is still shaped for it — payloads
are capped at 7 bytes so every message fits one classic frame. RS-485 turned out
to be strictly better here:

* the transceiver is already fitted on the HMI board, and switches itself
  (confirmed: SP485E at `U8`, sheet 5);
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
