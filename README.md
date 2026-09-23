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
| `docs/MQTT.md` | the app's wire contract, and why MQTT runs on the P4 rather than the C6 | |

## Bringing up the S3

Not yet done — this is the next milestone, and none of it has run on hardware.

```sh
# MicroPython v1.28 for the S3, then copy onto the board's filesystem:
#   link/spalink_codec.py   -> /link/spalink_codec.py   (main.py adds /link to the path)
#   s3_control/*.py         -> /
#   s3_control/config.example.json -> /config.json      and edit it
```

`config.json` holds two things and no credentials — this node has no radio, so
nothing about WiFi, MQTT or time lives here. `setpoint_f` is the thermostat
target, and **the firmware writes it back** whenever the HMI changes it, so the
file has to stay writable. `ntc_cal` is the water probe's calibration, preset to
the 30 k Balboa M7 already in this tub; `offset_f` is the trim to set against a
reference thermometer once it is up to temperature. A missing or unparseable file
is survivable — `main.py` falls back to the defaults in `spa_core` and `sensors`,
so a first boot with no config still runs the plant.

`tools/test_docs.py` checks the example against the code that reads it, because a
calibration that has drifted from `sensors.py` heats the tub to the wrong
temperature and nothing on the screen says so.

The handshake to look for is one line on each board: `link: up` from the S3 and
`[up] water=...` from the P4. The P4 side is already proven against a simulated
peer, so what is being tested here is the S3 and the three wires between them.

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

* **The app partition is custom** (`p4_hmi/partitions.csv`). IDF's stock table is
  sized for a 4 MB part and caps the app at 1500K, which the MQTT client crosses
  as soon as `mqtts://` pulls mbedTLS in. This module has 16 MB.

`docs/HARDWARE.md` has the rest, including what the board reports at boot.

### With the radio and MQTT

Both are off by default. A panel that runs a tub must not acquire a dependency on
a network to do it, so the default build is the one that has been on the glass.

```sh
cd p4_hmi
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.net" menuconfig
#   Spa HMI -> WiFi SSID / password      (bench only; BLE provisioning replaces it)
#           -> Broker URI / user / pass / device id
#           -> Timezone                  (leave empty and the clock stays --:--)
idf.py build flash monitor
```

`sdkconfig` is gitignored, so credentials typed into menuconfig stay on the
machine that typed them and never reach the repository.

**With no network configured the firmware scans instead of idling**, which proves
the C6, the SDIO link and the slave image without needing a password:

```
I (3410) transport: Identified slave [esp32c6]
I (6947) net: radio OK — the C6 answered and found 3 networks
```

With `CONFIG_SPA_HMI_BLE_PROV` the panel also advertises as **SpaControl** and
serves the app's WiFi-setup wizard, so a board never has to be told a network at
build time. `docs/BLE.md` has that protocol; note it is deliberately *not*
ESP-IDF's `wifi_provisioning`, because the app does not speak protocomm.

See `docs/MQTT.md` for the MQTT wire contract and what is still missing (OTA, and
`set_temp_cal`, which needs a new SpaLink message).

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
| MQTT / BLE / OTA | contract written (`docs/MQTT.md`), remote-command seam built and tested; **radio not started** |

The P4 half has now been on the board: it boots, drives the panel, answers touch
and brings the link up. The S3 half has not — it is still logic tested on a host
against the firmware running the tub, and has never been executed on an S3.

The two have therefore never spoken to each other. Every P4 state below the link
layer was exercised against a simulated controller, not a real one.

## Next steps

1. **Fit a 3.3 V RS-485 transceiver at the S3 end** (MAX3485 / SP3485 /
   THVD1450), tie its DE//RE to `LINK_PINS["RS485_DE"]`, and leave the pair
   unterminated — the carrier's idle bias does not survive a 120 Ω across it,
   and `docs/WIRING.md` §4 has the arithmetic. The HMI end is already built and
   self-directing. Prove the physical layer with the vendor's own
   `uart_echo_rs485` example before running any of our code.
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
4. **MQTT, on the P4 — not the C6.** The C6 keeps Guition's stock slave image
   and is only a radio, driven over SDIO by `esp_wifi_remote`; the TCP/IP stack,
   the MQTT client and BLE provisioning all live on the P4. `docs/MQTT.md` has
   the app's wire contract and the reasoning. The seam where a remote command
   meets the UI's intent (`ui_apply_remote`) is written and host-tested, so what
   is left is the radio itself: prove association first, then publish, then
   subscribe.
5. Move the link to RS-485 for the install: one transceiver at the S3 end, build
   the P4 with `SPALINK_USE_RS485`, no termination on the pair. No protocol
   change — see `docs/HARDWARE.md`.

## Inherited from openplc-hot-tub v2.0

The control core is a port, not a rewrite: `SpaController`, `ThermostatHeat`,
`OnDelay`, the two-tier permissive and the fault priority are unchanged, and the
differential test holds them to that.

Two deliberate behavioural changes, both switchable back off.

`spa_core.pump_run_ms` puts a **20-minute runtime ceiling** on pump 1 high,
pump 2 and pump 3, so a tub left with the jets on does not run them all night.
The clock starts when the request goes true and only releasing it restarts the
clock, so a panel holding the request stops at the ceiling until someone toggles
it — 20 minutes per press. Pump 1 **low** is deliberately uncapped: it is the
circulation pump the thermostat and freeze protection depend on, and it takes
over automatically when high speed times out. Set `pump_run_ms = None` for v2.0
behaviour; the differential test does exactly that.

The second, in `spa_core.flow_fault_latch`: in v2.0
`FAULT_NO_FLOW` is unreachable. Its guard is `flow_proven and not xFlowSwitch`,
but `flow_proven` is recomputed from `(any_pump and xFlowSwitch)` earlier in the
same scan, so it is already false on the scan the flow switch opens. Losing flow
stopped the heater — correctly — but never told anyone why. Latching "flow was
proven for this pump run" makes the fault reachable. Set the flag to `False` for
bit-exact v2.0 behaviour; the differential test does exactly that.
