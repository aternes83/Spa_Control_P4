# Spa_Control_P4

Two-board spa controller: an **ESP32-S3** running the plant, and an **ESP32-P4**
running the touchscreen and the radios, wired together over **SpaLink**.

This is a fresh project, not a branch of
[`openplc-hot-tub`](../openplc-hot-tub). That firmware runs the tub today and is
left alone; its control logic is ported here and held to a differential test
against it.

```
ESP32-S3-DevKitC-1-N8R8            Guition JC4880P443C_I_W
┌───────────────────────┐          ┌───────────────────────────┐
│ pumps, blower, heater │  SpaLink │ 480x800 capacitive touch  │
│ light, fault monitor  │◄────────►│ WiFi 6 / BLE 5 (ESP32-C6) │
│ NTC, flow, high limit │  UART    │ MQTT, app, OTA relay      │
│ e-stop                │ →RS-485  │ ST7701S DSI, GT911 touch  │
│ MicroPython           │          │ ESP-IDF + LVGL (C)        │
└───────────────────────┘          └───────────────────────────┘
   owns the plant                     owns the screen
   no radio                           no relays
```

## The one idea worth keeping

**The S3 decides; the P4 asks.** Every message from the HMI is a request the
control node is free to refuse, and the HMI learns what actually happened only
from the controller's status reports.

The consequence that matters: if the P4 crashes, is being reflashed, or its
cable is pulled, the S3 keeps its thermostat, its interlocks and its freeze
protection. It just stops honouring user requests. Losing a touchscreen must
never mean losing freeze protection on a winter night. That property is enforced
by `spalink_session.failsafe_requests()` and tested in
`tools/test_link_session.py`.

## Layout

| path | what | language |
|---|---|---|
| `link/` | SpaLink codec — the shared wire format, in both languages | Python + C |
| `s3_control/` | control node firmware | MicroPython |
| `p4_hmi/` | HMI node firmware | ESP-IDF + LVGL (C) |
| `tools/` | host tests — no hardware needed | Python + C |
| `docs/PROTOCOL.md` | the wire protocol, and why it is shaped that way | |
| `docs/HARDWARE.md` | pin maps, the interlink cable, CAN transceivers | |

## Tests

```sh
./tools/run_tests.sh
```

Needs only `python3` and a C compiler — no hardware, no ESP-IDF, no MicroPython.
Four suites:

* **`test_spalink.py`** — the codec. Runs every vector through *both* the Python
  and the C implementation and requires identical bytes, because the S3 speaks
  one and the P4 speaks the other. Also checks that every single-bit corruption
  of a frame is caught, and that the decoder resynchronises after line noise.
* **`test_spa_core.py`** — the control logic. Pins the safety properties, then
  drives the ported controller and the original `openplc-hot-tub` controller
  through 24,000 randomised steps on a shared fake clock and requires identical
  outputs.
* **`test_link_session.py`** — ACKs, sequence numbers, and the link-loss failsafe.
* **`test_spa_state.c`** — the HMI's state model: staleness, short frames, and
  millisecond-clock wrap.

## Status

| piece | state |
|---|---|
| SpaLink codec (Python + C) | done, cross-tested |
| Control core, ported and diffed against v2.0 | done |
| S3 link session + failsafe | done, tested |
| S3 main loop | written, **never run on hardware** |
| NTC sensor | ported; math checked on host |
| P4 link transport (ESP-IDF UART) | written, **never compiled against IDF** |
| P4 state model | done, tested |
| P4 LVGL user interface | **not started** — stub only |
| RS-485 for the install | HMI transceiver is on-board and self-directing; S3 needs one added |
| CAN transport | superseded by RS-485; pins and codec support retained |
| MQTT / BLE / OTA on the C6 | **not started** |

Nothing here has been on a board yet. The pieces marked *tested* are tested as
logic on a host; the pieces marked *written* have not been executed at all.

## Next steps

1. **Confirm the board model.** The pin map in `p4_hmi/main/board_pins.h` is now
   taken from published JC4880P443C_I_W schematics, but Guition ships several
   4.3" P4 variants that are not pin-compatible, and the supplier's own download
   could not be read directly. Check the model, then check GPIO35 — it is listed
   both as the boot button and on the JP1 header.
2. Bring up the link on the bench: three wires, both boards logging. The S3's
   `link: up` and the P4's `[up] water=... ` lines are the whole handshake.
3. Bring up the panel on the Waveshare BSP, then build the LVGL screen. It is
   480×800 portrait where the old HMI was 480×320 landscape, so this is a fresh
   layout rather than a port — which is why no guessed design is checked in.
4. Move MQTT and BLE onto the C6, then the OTA relay to the S3.
5. Move the link to RS-485 for the install: one transceiver at the S3 end, build
   the P4 with `SPALINK_USE_RS485`, 120 Ω at each end of the pair. No protocol
   change — see `docs/HARDWARE.md`.

## Inherited from openplc-hot-tub v2.0

The control core is a port, not a rewrite: `SpaController`, `ThermostatHeat`,
`OnDelay`, the two-tier permissive and the fault priority are unchanged, and the
differential test holds them to that.

One deliberate behavioural change, in `spa_core.flow_fault_latch`: in v2.0
`FAULT_NO_FLOW` is unreachable. Its guard is `flow_proven and not xFlowSwitch`,
but `flow_proven` is recomputed from `(any_pump and xFlowSwitch)` earlier in the
same scan, so it is already false on the scan the flow switch opens. Losing flow
stopped the heater — correctly — but never told anyone why. Latching "flow was
proven for this pump run" makes the fault reachable. Set the flag to `False` for
bit-exact v2.0 behaviour; the differential test does exactly that.
