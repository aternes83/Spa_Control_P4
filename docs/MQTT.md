# MQTT — the app's contract

**Status: steps 1-4 working on hardware, end to end.** The panel provisions over
BLE from the app, joins WiFi, learns its timezone, and publishes to a real broker
over TLS. Verified by subscribing to it:

```
3 status messages received on spa/spa1/status
  id spa1   temp_f 0.0   setpoint 0.0   heater false   pump1 0
  light false   eco false   max_jet false   fault false   fw p4-1.0   link false
```

The zeros are correct rather than a bug: no S3 is wired to this panel yet, so
there is no plant to report. That is exactly what `link: false` is for.

This file was written before the code so the two ends could not drift, and it
stays the specification rather than a description: the SpaControl iOS app already
speaks this protocol to the v2.0 firmware, so it is not a new design and not ours
to edit to match the code. `tools/test_docs.py` now enforces that direction —
every key the firmware publishes or accepts must appear here, and no key may be
camelCase.

| step | state |
|---|---|
| 1. `esp_wifi_remote` associates through the C6 | **verified on hardware** |
| 2. `spa/status` published | **verified against a live broker** |
| 3. `spa/commands` honoured | subscribed; untested until an S3 is wired |
| 4. BLE provisioning | **advertising on hardware**, see `docs/BLE.md` |
| 5. OTA relay, then `set_temp_cal` | not written |

## Where it runs, and why not on the C6

The C6 does **not** get MQTT. It runs Guition's stock slave image
(`JC-C6-slave_v2.3.2.bin`, in the vendor package) and acts as nothing but a
radio; the P4 drives it over the internal SDIO link with `esp_wifi_remote`, and
the whole TCP/IP stack, the MQTT client and the BLE provisioner live on the P4.

The alternative — an application on the C6 with its own MQTT client — would mean
writing C6 firmware, inventing a second P4↔C6 command protocol beside SpaLink,
duplicating the network stack, and discarding a working vendor image. It would
also need the JP1 harness to flash. With esp_hosted the C6 needs no new firmware
at all.

The S3 keeps no radio and no part of this. It never had one in this
architecture: it holds the plant, and the only thing it talks to is SpaLink.

```
   iOS app ──MQTT/TLS──► broker ──► P4  ──SpaLink──►  S3
                                     │                 └─ plant, interlocks
                                     └─ C6 (radio only, over SDIO)
```

## The design decision that matters

A command from the app and a press on the touchscreen **must land in the same
place**, or the panel and the phone will show different things and fight each
other. That place is `ui_intent_t` in `p4_hmi/main/ui/ui_model.h` — already the
single owner of what the user has asked for.

So MQTT is a *second input to the same intent*, not a parallel control path:

```
  touch  ─┐
          ├─► ui_intent_t ─► SPALINK_REQ_* ─► S3 decides ─► STATUS ─► screen
  MQTT   ─┘                                                       └─► MQTT status
```

Two things follow, and both are properties we already have rather than new work:

* **The app inherits the S3's veto for free.** An MQTT command is a request like
  any other; the S3 can refuse it on an interlock, and the app learns what
  actually happened from the next status publish — exactly as the screen does.
* **Eco and Max Jets stay on the P4.** They are modes that rewrite request bits
  (see `docs/HMI.md`), so the app toggling `eco` sets the same field the Eco tile
  does, including the 80 °F setpoint hand-off.

The status payload is therefore assembled from **both** structs: the plant fields
come from `spa_state_t` (what the S3 reported) and `eco`/`max_jet` from
`ui_intent_t` (what the P4 is applying). Nothing else in the firmware needs to
know MQTT exists.

## Topics

`BrokerSettings.swift` and `mqtt_spa.py` agree, including the unprefixed
fallback:

| topic | direction | notes |
|---|---|---|
| `spa/status` | P4 → app | published on change and at a slow heartbeat |
| `spa/commands` | app → P4 | partial JSON; omitted fields mean "unchanged" |

With a device id configured both become `spa/<id>/status` and
`spa/<id>/commands`, which is how more than one tub shares a broker.

## Status payload

JSON is **snake_case**. The app decodes with `.convertFromSnakeCase`, so
`temp_f` arrives as `tempF`; `TempCalDTO` pins its own keys explicitly because
they are already snake. Getting the case wrong is the single easiest way to break
this silently — the app decodes a partial object and shows stale values.

| field | type | source |
|---|---|---|
| `id` | string | device id |
| `temp_f` | number, 0.1 °F | `spa_state_t.water_dF` |
| `setpoint` | number, 0.1 °F | `spa_state_t.setpoint_dF` |
| `r_ohms` | int or null | NTC resistance, for the app's calibration wizard |
| `heater` | bool | `SPALINK_OUT_HEATER` |
| `pump1` | int 0/1/2 | off / low / high, from the **output** bits |
| `pump2`, `pump3` | bool | output bits |
| `light` | bool | output bit |
| `eco`, `max_jet` | bool | `ui_intent_t` — the P4 owns these |
| `fault` | bool | `spa_state_t.fault_active` |
| `fault_code` | int | matches `spa_core.FAULT_*` and `spa_fault_t` |
| `schedule_on`, `schedule_active` | bool | not implemented yet; omit rather than lie |
| `fw` | string | P4 firmware version |
| `ota_avail`, `ota_state` | string or null | not implemented yet |

**Publish actual outputs, never the user's selection.** v2.0's own comment says
it best — *"so the app stays honest like the LCD — e.g. MAX JET forcing all jets
high, or the heater forcing Jet 1 low."* That is the same rule the screen follows
(`docs/HMI.md`, fill versus selection), and the app's dial already relies on it:
its pump controls read `status.pump1`, so a pump the S3 is running for the
thermostat shows as running.

`maxJet`, `rOhms`, `scheduleOn`, `scheduleActive`, `fw`, `otaAvail` and
`otaState` are **optional** in `SpaStatus.swift`, so an early firmware that omits
them still decodes. Send only what is true.

## Command payload

Every field optional; Swift's `encodeIfPresent` omits nils, so a command carries
only what changed.

| field | type | lands on |
|---|---|---|
| `set_temp` | number °F | `SPALINK_MSG_SETPOINT`, clamped 60–104 by the S3 |
| `pump1` | int 0/1/2 | `ui_intent_t.pump1` |
| `pump2`, `pump3`, `light` | bool | the matching intent field |
| `eco`, `max_jet` | bool | `ui_set_eco()` / `ui_set_max_jet()`, so the mode logic runs |
| `set_temp_cal` | object | NTC coefficients — **belongs to the S3**, needs a new SpaLink message |
| `schedule` | object | not implemented |
| `ota_apply` | string | not implemented |

`eco` and `max_jet` must go through `ui_set_eco()` and `ui_set_max_jet()` rather
than writing the fields, or the setpoint hand-off and the mutual cancellation are
skipped.

`set_temp_cal` is the one command that is not the P4's to honour: the probe is on
the S3 and the coefficients live in its `config.json`. SpaLink has no message for
it, and its 7-byte payload cap means five floats need either several frames or a
reduced form. That is a protocol change, so it is deliberately out of scope for
the first cut — the app's calibration wizard will not work until it is added.

## What the hardware actually said

Step 1 is done, and it settled the question that made it step 1. The risk was
never the code — it was whether Guition's stock `JC-C6-slave_v2.3.2.bin` would
talk to a current `esp_hosted` host at all. It does:

```
I (3192) sdio_wrapper: SDIO master: Slot 1, Data-Lines: 4-bit Freq(KHz)[40000 KHz]
I (3192) sdio_wrapper: GPIOs: CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]
I (3410) transport: Identified slave [esp32c6]
I (3427) transport:        - HCI over SDIO
I (6947) net: radio OK — the C6 answered and found 3 networks
```

Three things worth keeping:

* **No SDIO configuration was needed.** Every value `esp_hosted` picks by
  default already matches Guition's own working build for this exact carrier
  (`CONFIG_BOARD_TYPE_GUITION_JC4880P443=y` in the `xiaozhi` demo in the
  documentation package) — verified key by key before building, then confirmed
  live by the log above. `sdkconfig.net` pins none of them on purpose; a second
  copy is a second thing to drift.
* **`HCI over SDIO` is advertised**, so step 4's BLE provisioning has a transport
  on this image. The C6 still needs no new firmware.
* **The radio can be proved without credentials.** With no network configured the
  firmware scans instead of idling, which exercises the C6 out of reset, the SDIO
  link, the slave image and the remote `esp_wifi` API, and leaves only the
  password untested. A board that says "the radio works, it has no network" is
  worth a great deal more than one that says nothing.

## The partition table had to change

The first link with a real broker URI produced **1,631,360 bytes against a
1,536,000-byte app partition**. IDF's `single app, large` table is sized for a
4 MB part; this module has 16 MB, so the ceiling was an accident of the default
rather than a real constraint. `p4_hmi/partitions.csv` replaces it with two 4 MB
OTA slots and keeps `nvs` and `phy_init` at byte-identical offsets so provisioned
credentials survive the migration.

Two slots now rather than one big one later: the flash layout is the one thing an
OTA cannot change, so getting it wrong means a wired reflash of every board.

Most of that growth is mbedTLS, arriving with `mqtts://` support. It is the last
thing worth dropping to save space — the alternative puts the broker password in
clear text on the wire.

## Two sequencing bugs worth remembering

**esp-mqtt must not be started before lwIP exists.** It tolerates having no
address — it retries by itself — but not an uninitialised TCP/IP stack, and lwIP
comes up on net_link's task long after `app_main` has run. Calling
`esp_mqtt_client_start()` from `app_main` panics with

```
assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)
```

and boot-loops the panel. This hid for as long as no broker was configured,
because then `mqtt_spa_start()` returned early and never reached `start()` at
all — the bug arrived with the first working broker URI. The client is created in
`app_main` and started from a task that waits on `net_link_wait_ip()`.

**TLS needs a trust anchor.** `mqtts://` without `crt_bundle_attach` fails at the
handshake. IDF's bundled roots cover the public CAs that hosted brokers use.

**A note on reading the build.** With an empty `CONFIG_SPA_HMI_MQTT_URI` the
literal `""[0] == '\0'` folds at compile time, `-O3` proves `mqtt_spa_start()`
unreachable past its warning, and the entire client is dead-stripped — the binary
grows by about 700 bytes and looks like it contains an MQTT implementation.
It does not. Check with a real URI, or check the symbols:

```
riscv32-esp-elf-nm build/esp-idf/main/CMakeFiles/__idf_main.dir/mqtt_spa.c.obj | grep esp_mqtt
```

## Order of work

1. `esp_wifi_remote` on the P4 against the vendor's C6 slave image; prove it
   associates and gets an address before any MQTT.
2. `esp-mqtt` publishing `spa/status` from `spa_state_t` + `ui_intent_t`.
3. `spa/commands` into `ui_intent_t`, through the mode helpers.
4. ~~BLE provisioning to match `SpaBLEProvisioner` in the app.~~ Written and
   advertising — `docs/BLE.md`. It is **not** IDF's `wifi_provisioning`: the app
   speaks a Nordic UART Service carrying JSON, and a protocomm board would be
   invisible to the wizard.
5. OTA relay, and then `set_temp_cal` once SpaLink has a message for it.

Each of 1–3 is independently testable; nothing beyond step 1 is worth writing
until the radio associates, because everything above it is standard ESP-IDF.
