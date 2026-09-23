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
| 1 | **MAX3485 breakout module** (`EN VCC RXD TXD GND GND A B`) | **3.3 V**. The S3 end — pin map in §4. A bare `MAX3485CSA+` SOIC-8 works too. |
| 1 | 10 kΩ resistor | Pull-down on `EN`, so the module boots listening instead of jamming the bus. See §4. |
| 0 | ~~120 Ω terminator~~ | **Not used on this link, and harmful here — see §4, *The bus*.** If your module has one fitted, take it off. |
| 1 | Twisted pair, shielded (Cat5e is fine) | S3 → P4 run. One pair for A/B, plus a ground conductor |
| 1 | 100 nF ceramic capacitor | Decoupling across VCC/GND. Modules usually have this already. |

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
                         │      └── 3V3 pin ──┬── MAX3485 module VCC
                         │                    └── NTC divider top rail
                         │
                         └── P4 carrier USB-C (its own supply)

   GND ═══════════════════════════ common to both boards ═══════════════
```

Both boards may run from separate supplies, but **their grounds must be tied
together** — carry a ground conductor alongside the A/B pair. RS-485 tolerates a
few volts of ground offset, not an unbonded floating pair.

## 4. The RS-485 link

### S3 end — the module you have

Breakout modules relabel the MAX3485's pins, so `EN / VCC / RXD / TXD / GND /
GND / A / B` is the same chip underneath with friendlier names:

| Module pin | IC pin | What it really is | Connect to |
|---|---|---|---|
| `EN` | 2 + 3 | `/RE` and `DE`, tied together | **GPIO1** |
| `VCC` | 8 | Supply — sets the logic level | **3V3**, never 5 V |
| `RXD` | 1 | `RO`, data **out** of the module | **GPIO48** (the S3's RX) |
| `TXD` | 4 | `DI`, data **in** to the module | **GPIO47** (the S3's TX) |
| `GND` | 5 | Ground | GND |
| `GND` | — | Same net, just a second pin | GND (or leave) |
| `A` | 6 | Non-inverting line | twisted pair → P4 `J4` `A` |
| `B` | 7 | Inverting line | twisted pair → P4 `J4` `B` |

```text
   ESP32-S3                         MAX3485 module
   ────────                         ──────────────
   GPIO48 (RX) ◄──────────────────── RXD
   GPIO47 (TX) ────────────────────► TXD
   GPIO1  (DE) ──┬─────────────────► EN
                 │
                10k                  A ───── twisted pair ──► P4 J4 A
                 │                   B ───── twisted pair ──► P4 J4 B
                GND                  GND ─── third conductor ─ P4 GND
                                     VCC ◄── 3V3
```

**`RXD` and `TXD` are named for the pin you connect them to, not for what the
module does with them.** They go straight across — `RXD` to the S3's RX, `TXD`
to the S3's TX — which reads wrong if you are used to crossing TX and RX. It is
not crossed here because the crossing already happened inside the naming: `RXD`
is the receiver's *output*, carrying data toward the S3.

Vendors are not perfectly consistent about this. If the link stays silent with
everything else correct, swap those two wires — both are 3.3 V logic and neither
pin is harmed by the swap, so it costs nothing to try.

**The 10 kΩ pull-down on `EN` is worth fitting.** `GPIO1` floats from power-up
until `main.py` configures it, and a floating `EN` can leave the module driving
the bus — one node holding the pair while it boots is enough to jam the link for
both. Pulled down, the module powers up listening, which is the safe state.

Check the chip marking before powering it: the cheap blue modules sold as
"RS485" often carry a 5 V **MAX485**. See *Why 3.3 V, specifically* below — this
is the one substitution that quietly damages the S3.

Many modules already carry a 120 Ω termination resistor across A/B, sometimes on
a jumper. **Find it and take it off.** This link runs unterminated: the
carrier's idle bias cannot survive a 120 Ω across the pair, and what it fails
into looks like a flaky link rather than a wiring error. See *The bus* below.

### If you have a bare SOIC-8 chip instead

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
   GPIO47 (TX) ────────────────────┤ 4  DI   GND 5 ├──── GND
                                   └───────────────┘

   /RE and DE tied together: one GPIO drives both.
   HIGH = transmitting, LOW = listening. The driver handles this.
```

`GPIO1` is `LINK_PINS["RS485_DE"]` in `s3_control/board_pins.py`. If you fit an
auto-direction module instead — one with no `EN` or `DE` pin at all — set that
entry to `None` and the driver stops toggling anything.

### P4 end — nothing to build

Already fitted on the carrier. `GPIO26` → SP485E `DI`, `GPIO27` ← `RO` through
a 1 kΩ series resistor, and DE//RE driven from the TX line by a 74LVC1G132, so
there is no direction pin. The bus is on the 4-pin **`J4`** connector.

### The bus

**Leave the pair unterminated at both ends.** Three wires, no resistors:

```text
   S3 end                                              P4 end (J4)
   ──────                                              ───────────
   A ─────────────────  twisted pair  ───────────────── Ao

   B ────────────────────────────────────────────────── Bo

   GND ───────────────  third conductor  ────────────── GND
```

This is the opposite of the usual RS-485 advice, and it is specific to this
link. What the carrier fits across A/B is not a termination but an idle bias
chain (sheet 5): `R77` 10 kΩ from 5 V to `A`, `R73` 5.1 kΩ across the pair, and
`R68` 10 kΩ from `B` to GND. Undriven, 199 µA flows through it and holds the
pair at **1.02 V** — the failsafe that stops both receivers chattering while
nobody is transmitting.

A 120 Ω across the pair is forty times stiffer than that 5.1 kΩ leg, so it does
not add to the bias. It replaces it:

| Terminations fitted | Idle V(A−B) |
|---:|---|
| none | 1.02 V |
| one | 29 mV |
| two — the usual advice | 15 mV |

Neither transceiver has an internal failsafe. Both switch on a ±200 mV receiver
threshold, and between those thresholds the output is undefined. Terminated,
this bus idles inside that dead band: the receivers chatter, the UARTs see
phantom start bits, `bad_len` and `bad_crc` climb, and a phantom byte landing
just ahead of a real frame eats its header. It presents as an unreliable link,
never as a wiring error.

Nothing is lost by leaving termination off. A bit at 115200 baud is 8.68 µs and
cable delay is around 5 ns/m, so even a 20 m run is about 1 % of a bit — the
reflections termination exists to kill have long settled before the sampling
instant. It earns its keep at high baud or over long runs, and this link is
neither.

**Measure it rather than trusting it.** With the P4 powered and the S3 idle, a
meter across `J4` should read **≈ +1.0 V from `Ao` to `Bo`**. A few millivolts
means a termination is still on the bus somewhere — most often one fitted on the
S3 module. About a volt the *wrong way* means A and B are swapped in the cable.
Powered down, an ohmmeter across the pair reads several kΩ with nothing
terminated, ~118 Ω with one 120 Ω fitted, and ~59 Ω with two.

**If you ever do need termination** — a longer run, a faster baud, a third node
— the bias has to be stiffened in the same breath or you land back here. 330 Ω
from 3V3 to `A` and 330 Ω from `B` to GND at the S3 end restores 275 mV across a
terminated pair, at the cost of loading the drivers to 55 Ω against their 54 Ω
minimum. With two nodes on a short cable, leaving the termination off is the
better trade.

### As built: the S3 end is terminated, deliberately

The transceiver currently fitted at the S3 end is a **SparkFun BOB-10124**
(SP3485 breakout), and it carries **`R4`, 220 Ω, hardwired across A/B**. Not a
jumper, not a solder blob — a fixed part. This board cannot be run unterminated
without removing it.

Measured consequence, on this bench: idle bias **85 mV**, against **0.99 V** with
the carrier's chain alone. That is inside the ±200 mV dead band, so between
frames neither receiver has a defined state. Driven signalling is unaffected —
220 Ω is a light load when RS-485 is specified down to 54 Ω — and the link runs
clean with it, which is why it is still fitted.

> **If `Bad CRC` or `Bad length` ever starts climbing, check `R4` first.** It is
> the known weak point in this bus and the cheapest thing to rule out. Desolder
> it and the carrier's own chain restores ~0.99 V of failsafe with no other
> change. Everything else — baud, framing, the codec, the cable — has been
> verified against working traffic; the idle bias has not, because it is
> knowingly out of spec.

`J4` carries **two** A/B pairs so nodes can be daisy-chained; they are the same
net brought out twice.

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

All inputs read active-high: **HIGH = asserted**. The request inputs are
`Pin.IN, Pin.PULL_DOWN`, so an open or absent switch reads as not requested.
The three interlocks are `Pin.IN, Pin.PULL_UP` — see the warning below.

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
        R_fixed          10 kΩ 1%, whichever probe is fitted
         │
         ├──────────────► GPIO6  (ADC1_CH5)
         │
        NTC probe        in the wet well
         │
        GND
```

**`R_fixed` is 10 kΩ and does not change with the probe.** It is the divider
resistor on the board, inherited from v2.0. Only `r0` and `beta` in the
calibration describe the thermistor: `NTC_DEFAULT_CAL` is `r0` 10 k / Beta 3950
for a generic part, `NTC_BALBOA_M7_CAL` is `r0` 30 k / Beta 3892 for this tub's
probe — both on the same 10 k divider.

Reading `r_fixed` as "match it to the probe" is what made the panel report a
70 °F room as 30 °F: three times the resistance, a plausible-looking cold
number, and no fault raised because it was still inside the plausible band.

Above ~500 kΩ the probe reads as open, below 200 Ω as shorted; either raises
`FAULT_TEMP_SENSOR` and the heater is refused. Keep the probe leads away from
the contactor wiring — it is a high-impedance analog run, and the firmware
oversamples 16× precisely because these leads pick up noise.

Analog must stay on **ADC1** (GPIO1–GPIO10). ADC2 is unusable while WiFi is up;
this node has no radio now, but the constraint costs nothing to keep.
