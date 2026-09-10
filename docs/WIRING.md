# Wiring

Complete point-to-point wiring for both boards. Pin numbers come from
`s3_control/board_pins.py` and `p4_hmi/main/board_pins.h`; if a diagram here and
a pin map disagree, the pin map wins and this file is the bug.

**Mains work is for a qualified electrician.** Nothing on either board touches
line voltage — the ESP32s drive contactor coils through an opto-isolated relay
interface, and the diagrams stop at that boundary.

## 1. System overview

```text
        ┌──────────────────────────────┐            ┌──────────────────────────────┐
        │ ESP32-S3-DevKitC-1-N8R8      │            │ Guition JC-ESP32P4-M3        │
        │ control node                 │            │ on JC4880P443C_I_W carrier   │
        │                              │            │                              │
  NTC ──┤ GPIO6                        │            │ 4.3" 480x800 touch           │
 flow ──┤ GPIO9         GPIO47 ──► DI ┐│            │┌ DI ◄── GPIO26                │
hi-lim──┤ GPIO10        GPIO48 ◄── RO ┤│  A/B pair  │┤ RO ──► GPIO27                │
e-stop──┤ GPIO11        GPIO1  ──► DE ┘│════════════│└ DE  (automatic)              │
        │                    MAX3485   │  RS-485    │   SP485E (fitted)   J4        │
        │                    (you add) │            │                              │
        │ 8 relay outputs              │            │ WiFi 6 / BLE 5 via C6        │
        └──────────┬───────────────────┘            └──────────────────────────────┘
                   │
        opto-isolated relay board
                   │
          pumps · blower · heater · light
```

## 2. Bill of materials — the parts you still need

| Qty | Part | Notes |
|---:|---|---|
| 1 | **MAX3485CSA+** RS-485 transceiver, SOIC-8 | **3.3 V**. The S3 end. See §4 for why not a MAX485. |
| 2 | 120 Ω resistor, ¼ W | Bus termination, one at each end |
| 1 | Twisted pair, shielded (Cat5e is fine) | S3 → P4 run. One pair for A/B, plus a ground conductor |
| 1 | 100 nF ceramic capacitor | Decoupling across the transceiver's VCC/GND |

Acceptable substitutes for the transceiver, all 3.3 V and pin-compatible:

| Part | Why you might pick it |
|---|---|
| `MAX3485CSA+` | Default. Widely stocked, 3.3 V, SOIC-8. |
| `THVD1450DR` | More rugged: ±18 kV IEC ESD, ±70 V bus-fault protection, integrated failsafe. Worth it next to contactors. |
| `SP3485` family | The direct 3.3 V sibling of the SP485E already on the P4 board. |

Ordering suffixes vary by package and temperature grade — confirm the exact
suffix with your distributor. If you buy a **breakout module** rather than a
bare chip, check the silkscreen on the chip itself: the common cheap blue
"RS485 module" is a 5 V **MAX485**, which is the one part to avoid.

## 3. Power

```text
   USB-C or 5 V supply ──┬── ESP32-S3 DevKit  5V pin
                         │      └── 3V3 pin ──┬── MAX3485 VCC (pin 8)
                         │                    └── NTC divider top rail
                         │
                         └── P4 carrier USB-C (its own supply)

   GND ═══════════════════════════ common to both boards ═══════════════
```

Both boards may run from separate supplies, but **their grounds must be tied
together** — carry a ground conductor alongside the A/B pair. RS-485 tolerates a
few volts of ground offset, not an unbonded floating pair.

## 4. The RS-485 link

### S3 end — the transceiver you add

```text
   ESP32-S3                        MAX3485 (SOIC-8, 3.3 V)
   ────────                        ───────────────────────
                                   ┌───────────────┐
   GPIO48 (RX) ◄───────────────────┤ 1  RO   VCC 8 ├──── 3V3 ──┬── 100nF
                                   │               │           │
   GPIO1  (DE) ──┬─────────────────┤ 2  /RE    B 7 ├── B ──────┴── GND
                 │                 │               │
                 └─────────────────┤ 3  DE     A 6 ├── A
                                   │               │
   GPIO47 (TX) ─────────────────────┤ 4  DI   GND 5 ├──── GND
                                   └───────────────┘

   /RE and DE tied together: one GPIO drives both.
   HIGH = transmitting, LOW = listening. The driver handles this.
```

`GPIO1` is `LINK_PINS["RS485_DE"]` in `s3_control/board_pins.py`. If you fit an
auto-direction module instead — one with no DE pin — set that entry to `None`
and leave pins 2 and 3 to the module.

### P4 end — nothing to build

Already fitted on the carrier. `GPIO26` → SP485E `DI`, `GPIO27` ← `RO` through
a 1 kΩ series resistor, and DE//RE driven from the TX line by a 74LVC1G132, so
there is no direction pin. The bus is on the 4-pin **`J4`** connector.

### The bus

```text
   S3 end                                              P4 end (J4)
   ──────                                              ───────────
   A ──┬──────────────  twisted pair  ──────────────┬── Ao
       │                                            │
      120Ω                                         120Ω
       │                                            │
   B ──┴────────────────────────────────────────────┴── Bo

   GND ─────────────  third conductor  ─────────────── GND
```

`J4` carries **two** A/B pairs so nodes can be daisy-chained. Terminate only the
two physical ends of the bus — a 120 Ω at every node kills the signal.

### Why 3.3 V, specifically

The transceiver's supply sets what its **RO** pin outputs, and RO drives the
S3's RX pin directly. A 5 V transceiver would swing RO 0–5 V into GPIO48, which
is a 3.3 V input and not 5 V tolerant. It usually appears to work, then degrades
the pin.

The P4 carrier has exactly this problem — its SP485E runs at 5 V — and Guition
solved it with `R70`, a 1 kΩ series resistor into the P4's clamp diode. That is
a workaround for a part they had already chosen; no reason to inherit it.

**The bus itself does not care.** A 3.3 V and a 5 V transceiver interoperate
perfectly over A/B — identical differential thresholds. Mixing them is normal
and is the whole point of differential signalling. The 3.3 V choice protects the
S3's pin, nothing else.

## 5. Control inputs

All inputs are `Pin.IN, Pin.PULL_UP` and read active-high: **HIGH = asserted**.

```text
                            3V3 ──┐
                                  │  (internal pull-up, ~45 kΩ)
   panel switch / interlock       │
        ┌───o  o───────────────┬──┴── GPIOxx
        │                      │
       3V3                    GND  (switch closed to 3V3 = asserted)
```

| Signal | GPIO | Asserted (HIGH) means |
|---|---:|---|
| `xSpaEnable` | 4 | Spa enabled |
| `xPumpRequest` | 5 | Pump 1 low speed |
| `xPump1HighRequest` | 7 | Pump 1 high speed |
| `xPump2Request` | 15 | Pump 2 |
| `xPump3Request` | 16 | Pump 3 |
| `xJetsRequest` | 17 | Jets |
| `xBlowerRequest` | 18 | Blower |
| `xLightRequest` | 8 | Light |
| `xFlowSwitch` | 9 | Flow proven |
| `xHighLimitOK` | 10 | High limit **OK** |
| `xRemoteEStopOK` | 11 | E-stop **OK** |

Every one of these is also reachable from the touchscreen — the firmware ORs the
physical input with the HMI's request, so a hardwired panel switch and the P4
both work, and neither can be overridden by the other.

> **Read this before wiring the safety inputs.** With the internal pull-up,
> a disconnected `xHighLimitOK` or `xRemoteEStopOK` floats **HIGH**, which the
> firmware reads as *OK*. A cut wire therefore reads safe. This is inherited
> from v2.0 and is why the hardware interlock chain in §6 is not optional: the
> software interlocks are the second line of defence, never the first. If you
> want the software side fail-safe too, those two pins need `Pin.PULL_DOWN` and
> the interlock contacts wired to source 3V3 — a one-line change, and a
> deliberate one, so it is not made silently here.

## 6. Control outputs and the safety chain

Outputs are `Pin.OUT`, idle LOW, HIGH = energise. They drive an **opto-isolated**
relay/contactor interface — never a coil or a load directly.

```text
   ESP32-S3                    opto-isolated              contactor
   ────────                    relay board                  coil
   GPIO12 xPump1_Low  ───────►  IN1  ──┤├──  RLY1  ────────► K1
   GPIO13 xPump1_High ───────►  IN2  ──┤├──  RLY2  ────────► K2   (interlocked
   GPIO14 xPump2      ───────►  IN3  ──┤├──  RLY3  ────────► K3    with K1 in
   GPIO21 xPump3      ───────►  IN4  ──┤├──  RLY4  ────────► K4    software AND
   GPIO38 xHeater     ───────►  IN5  ──┤├──  RLY5  ──┐        wiring)
   GPIO39 xJets       ───────►  IN6  ──┤├──  RLY6  ──│─────► K6
   GPIO40 xBlower     ───────►  IN7  ──┤├──  RLY7  ──│─────► K7
   GPIO41 xLight      ───────►  IN8  ──┤├──  RLY8  ──│─────► K8
                                                    │
                                                    │  heater coil feed
                                                    ▼
        ┌───────────────────────────────────────────────────────────┐
        │  HARDWARE SAFETY CHAIN — in series with the heater coil   │
        │                                                           │
        │   RLY5 ──o/o── high limit ──o/o── e-stop ──o/o── flow ──► K5 coil
        │           (N.C.)             (N.C.)          (N.C.)       │
        └───────────────────────────────────────────────────────────┘
```

The heater contactor coil passes through the physical high-limit thermostat,
the e-stop and the flow switch as normally-closed contacts in series. Any one of
them opening kills the heater **regardless of what the firmware believes**. Pump
1's two speeds must also be mechanically interlocked at the contactors, not only
in software.

## 7. Water temperature — NTC

```text
        3V3
         │
        R_fixed          10 kΩ for a generic 10 k NTC
         │               30 kΩ for the Balboa M7 (this tub)
         ├──────────────► GPIO6  (ADC1_CH5)
         │
        NTC probe        in the wet well
         │
        GND
```

`R_fixed` matches the probe's nominal resistance at 25 °C — that puts the
divider near mid-rail at spa temperatures, where the ADC is most linear. The
presets are `NTC_DEFAULT_CAL` (10 k, Beta 3950) and `NTC_BALBOA_M7_CAL`
(30 k, Beta 3892) in `s3_control/sensors.py`.

Above ~500 kΩ the probe reads as open, below 200 Ω as shorted; either raises
`FAULT_TEMP_SENSOR` and the heater is refused. Keep the probe leads away from
the contactor wiring — it is a high-impedance analog run, and the firmware
oversamples 16× precisely because these leads pick up noise.

Analog must stay on **ADC1** (GPIO1–GPIO10). ADC2 is unusable while WiFi is up;
this node has no radio now, but the constraint costs nothing to keep.
