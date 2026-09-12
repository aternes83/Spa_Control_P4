# Spa_Control_P4

Two-board spa controller: an **ESP32-S3** running the plant, and an **ESP32-P4**
running the touchscreen and the radios, wired together over **SpaLink**.

This is a fresh project, not a branch of
[`openplc-hot-tub`](../openplc-hot-tub). That firmware runs the tub today and is
left alone; its control logic is ported here and held to a differential test
against it.

```
ESP32-S3-DevKitC-1-N8R8            Guition JC-ESP32P4-M3, 4.3"
┌───────────────────────┐          ┌───────────────────────────┐
│ pumps, blower, heater │  SpaLink │ 480x800 capacitive touch  │
│ light, fault monitor  │◄────────►│ WiFi 6 / BLE 5 (ESP32-C6) │
│ NTC, flow, high limit │  UART    │ MQTT, app, OTA relay      │
│ e-stop                │ →RS-485  │ ST7701S DSI, GT911 touch  │
│                       │          │ 800x480 landscape UI      │
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

The same idea shapes the screen. A tile lights up when the S3 reports the load
energised, never because it was pressed, and a request the controller declines
is drawn as declined rather than as success. See `docs/HMI.md`.

One consequence worth knowing before reading `s3_control/main.py`: the HMI has no
off switch, so it holds `xSpaEnable` asserted for the life of the board, and the
four-hour run-timer ceiling is therefore turned off for this installation. The
link failsafe is what drops enable now, and it does so in 1.5 s without anyone
having to remember anything. Put the ceiling back if a wired enable switch is
ever fitted in parallel.

## Layout

| path | what | language |
|---|---|---|
| `link/` | SpaLink codec — the shared wire format, in both languages | Python + C |
| `s3_control/` | control node firmware | MicroPython |
| `p4_hmi/` | HMI node firmware | ESP-IDF + LVGL (C) |
| `tools/` | host tests — no hardware needed | Python + C |
| `docs/PROTOCOL.md` | the wire protocol, and why it is shaped that way | |
| `docs/HARDWARE.md` | pin maps, the interlink cable, verified against vendor schematics | |
| `docs/WIRING.md` | complete point-to-point wiring diagrams and the parts list | |
| `docs/HMI.md` | the screen: layout, touch sizing for wet hands, and the state model behind it | |

## Building the HMI

```sh
# once: the vendor package carries the BSP and is too large for git
#   download JC4880P443C_I_W from pan.jczn1688.com, unpack at the repo root

. ~/esp/esp-idf/export.sh          # ESP-IDF v5.5
cd p4_hmi
idf.py set-target esp32p4
idf.py -p /dev/cu.usbmodemXXXXX flash monitor
```

Three things that are not obvious and cost a bring-up session each:

* **The P4 on this module is revision v1.3**, and IDF 5.5 defaults to requiring
  v3.1+. `sdkconfig.defaults` pins the pre-v3 family; without it the bootloader
  will not run on the chip and esptool refuses to write it.
* **The BSP comes from the vendor package, not the component registry.** Guition
  modified theirs for this panel; the registry one builds and drives the display
  at the wrong resolution.
* **On macOS, `install.sh` does not fetch cmake or ninja** — it expects them from
  a package manager. `python3 $IDF_PATH/tools/idf_tools.py install cmake ninja`
  gets them without needing Homebrew.

`docs/HARDWARE.md` has the rest, including what the board reports at boot.

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
* **`test_ui_model.c`** — the HMI's presentation logic, the part of the screen
  that is not LVGL: how a press becomes a request, how Eco and Max Jets rewrite
  those requests, and the four states a control can be drawn in. The one worth
  the file is `REFUSED` — a control the user asked for that the controller is
  still not running, which a screen must show rather than quietly report as on.
* **`test_docs.py`** — every GPIO in the wiring diagrams against the pin map, so
  a diagram someone wires from at 2 a.m. cannot drift out of date.

## Status

| piece | state |
|---|---|
| SpaLink codec (Python + C) | done, cross-tested |
| Control core, ported and diffed against v2.0 | done |
| S3 link session + failsafe | done, tested |
| S3 main loop | written, **never run on hardware** |
| NTC sensor | ported; math checked on host |
| P4 link transport (ESP-IDF UART) | **running on hardware**: UART1 up on GPIO26/27, RS-485 half-duplex |
| P4 state model | done, tested |
| P4 LVGL user interface | **running on hardware**, 800x480 landscape; presentation logic tested on host |
| RS-485 for the install | **verified against vendor schematics**: SP485E on GPIO26/27, self-directing; S3 needs a 3.3 V transceiver added |
| CAN transport | superseded by RS-485; pins and codec support retained |
| MQTT / BLE / OTA on the C6 | **not started** |

The P4 half has now been on the board: it boots, drives the panel, answers touch
and brings the link up. The S3 half has not — it is still logic tested on a host
against the firmware running the tub, and has never been executed on an S3.

The two have therefore never spoken to each other. Every P4 state below the link
layer was exercised against a simulated controller, not a real one.

## Next steps

1. **Fit a 3.3 V RS-485 transceiver at the S3 end** (MAX3485 / SP3485 /
   THVD1450), tie its DE//RE to `LINK_PINS["RS485_DE"]`, 120 Ω at each end. The
   HMI end is already built and self-directing. Prove the physical layer with
   the vendor's own `uart_echo_rs485` example before running any of our code.
2. Bring up the link on the bench: three wires, both boards logging. The S3's
   `link: up` and the P4's `[up] water=... ` lines are the whole handshake. The
   P4 side of this is proven — it prints exactly that against a simulated peer —
   so what is untested is the S3 and the wire between them.
3. **Build the screen against the vendor BSP.** The layout is written
   (`p4_hmi/main/ui/`, described in `docs/HMI.md`) and has been reviewed in a
   browser mock-up driven by a simulated control board, but has never been
   through a compiler that has LVGL in it. The project pulls the BSP from the vendor's own
   copy in the documentation package rather than from the component registry,
   because Guition modified it for this panel — see the note at the top of
   `p4_hmi/CMakeLists.txt`. Expect the first build to be about LVGL 9.5 API
   names, and the first flash to be about geometry.
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
