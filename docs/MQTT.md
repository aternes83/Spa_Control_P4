# MQTT — the app's contract

**Status: not implemented.** This is the design and the wire format, written down
before the code so the two ends cannot drift. The SpaControl iOS app already
speaks this protocol to the v2.0 firmware, so it is not a new design — it is a
contract we have to honour exactly.

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

## Order of work

1. `esp_wifi_remote` on the P4 against the vendor's C6 slave image; prove it
   associates and gets an address before any MQTT.
2. `esp-mqtt` publishing `spa/status` from `spa_state_t` + `ui_intent_t`.
3. `spa/commands` into `ui_intent_t`, through the mode helpers.
4. BLE provisioning to match `SpaBLEProvisioner` in the app, so a board is never
   tied to one network at flash time.
5. OTA relay, and then `set_temp_cal` once SpaLink has a message for it.

Each of 1–3 is independently testable; nothing beyond step 1 is worth writing
until the radio associates, because everything above it is standard ESP-IDF.
