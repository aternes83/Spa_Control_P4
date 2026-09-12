# BLE provisioning — the app's contract

**Status: advertising and serving on hardware; the request/response path has not
yet been driven by a client.** The panel advertises as `SpaControl` and registers
the service, both confirmed on the board. What has not happened is a phone
actually running the wizard against it.

This is what unties a board from the network it was flashed beside. Until it
existed the only way to give the panel a network was `CONFIG_SPA_HMI_WIFI_SSID`,
which bakes somebody's password into a binary and means a board cannot be moved,
re-homed or sold on.

## It is not ESP-IDF's provisioning

The obvious implementation — `wifi_provisioning` with `scheme_ble` — is the wrong
one, and it is worth saying why before somebody "fixes" this.

`SpaBLEProvisioner.swift` in the app is the side that already exists, and it does
not speak protocomm. It drives a **Nordic UART Service** carrying plain JSON, and
it finds the peripheral by **advertised local name alone** — it scans with no
service filter and compares the name. A board built on IDF's provisioning manager
would advertise a different service, speak a protobuf-over-protocomm session with
security1 handshakes, and be invisible to the wizard. It would be correct, modern,
and completely unable to talk to the product.

The app is not ours to change here. This document is the specification and the
firmware follows it.

## Transport

| | |
|---|---|
| advertised name | `SpaControl` — **load-bearing**, the app matches on it |
| service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX, app writes | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX, panel notifies | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

The advertisement carries the name and nothing else — a 128-bit UUID would crowd
the 31-byte packet for no gain, since the app never filters on it. The service
UUID goes in the scan response so other BLE tools can still identify the board.

**Messages are JSON objects with no delimiter.** The app reassembles by counting
braces rather than trusting notification boundaries, so the panel is free to
fragment a reply across several notifications — and does, to whatever the
negotiated MTU allows. At the default 23-byte MTU that is 20 bytes a packet, and
a single scan result is about twice that.

## App → panel

| message | means |
|---|---|
| `{"wifi_scan":1}` | scan for networks |
| `{"wifi_ssid":"...","wifi_pw":"..."}` | try these credentials |
| `{"broker_get":1}` | send me your MQTT settings |

## Panel → app

A scan is a bracketed sequence, so the app knows when the list is complete:

```json
{"scan":"begin"}
{"net":{"s":"TER_2.4GHz","r":-71,"sec":1}}
{"net":{"s":"…","r":-86,"sec":1}}
{"scan":"end"}
```

`s` is the SSID, `r` the RSSI, `sec` 0 for an open network and 1 otherwise.
Hidden networks are skipped — there is nothing for the app to offer somebody to
tap. **`{"scan":"end"}` is sent even when the scan failed or found nothing**: the
wizard spins until it arrives, so an empty list is a result and silence is a
hang.

A provisioning attempt reports its outcome:

```json
{"wifi":"connecting"}
{"wifi":"ok","ip":"192.168.1.42"}
{"wifi":"fail","err":"wrong password"}
```

`err` is written for somebody standing at the tub, not for a log: *wrong
password*, *network not found*, *network too weak here*, *the router refused the
connection*. The raw reason code goes to the console instead. A failed attempt is
reported on the **first** disconnect rather than retried, because someone is
waiting to be told whether the password was right, and a silent retry loop leaves
the wizard spinning with nothing to show.

Broker settings let the app configure itself against the same broker the panel
uses, instead of making somebody type a host and port twice:

```json
{"broker":{"host":"…","port":8883,"user":"…","pw":"…","device_id":"…"}}
```

The firmware holds one URI and the app wants host and port apart, so the URI is
split rather than duplicated into a second setting that can disagree with the
first. A missing port defaults to 8883 for `mqtts://` and 1883 for `mqtt://`.

## Where the radio actually is

The P4 has no radio at all. The **NimBLE host runs on the P4** and the
**controller stays on the C6**, reached over the same internal SDIO bus that
carries Wi-Fi — the boot log calls it `HCI over SDIO`. So `CONFIG_BT_ENABLED=y`
sits next to `CONFIG_BT_CONTROLLER_DISABLED=y`, which looks contradictory and is
not: there is no local controller to build.

```
I (3535) vhci_drv: Host BT Support: Enabled
I (3538) vhci_drv: 	BT Transport Type: VHCI
I (4459) ble: advertising as "SpaControl"
```

The C6 still needs no new firmware, and Wi-Fi and BLE run at the same time on it.

### Two things that cost a bring-up each

**Only one place may bring the SDIO transport up.** `net_link` owns it. When
`ble_prov` also called `esp_hosted_connect_to_slave()`, the two raced and the SD
card initialisation itself failed — `sdmmc_card_init failed`, over and over. That
reads exactly like a hardware or wiring fault and is not one. Anything needing the
C6 waits on `net_link_wait_hosted()` instead.

**`esp_hosted_bt_controller_init()` and `_enable()` are best-effort.** Against
Guition's stock image they return `ESP_ERR_NOT_SUPPORTED`:

```
W (4210) ble: slave declined bt_controller_init (ESP_ERR_NOT_SUPPORTED) — continuing
```

They are RPCs asking a *newer* slave to start a controller. The stock image
reports itself as 2.3.0 against a 2.11.0 host, predates them, and starts its own
controller anyway — VHCI announces itself up before either call is made.
esp_hosted's own example only warns on them for this reason. Treating the refusal
as fatal produced the message "no BT controller on the C6" about a C6 whose
controller was working perfectly.

### A version gap worth knowing about

```
W (3382) transport: Version mismatch: Host [2.11.0] > Co-proc [2.3.0]
                    ==> Upgrade co-proc to avoid RPC timeouts
```

Everything needed so far works, but Espressif warn about RPC timeouts across this
gap and the missing BT RPCs are one visible symptom. Reflashing the C6 with a
matching slave is possible — its UART0 and boot straps are broken out on the
carrier for exactly that — but it means giving up a known-good vendor image, so
it is worth doing only if a real timeout shows up.

## Building it

```sh
cd p4_hmi
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.net" build flash monitor
```

`CONFIG_SPA_HMI_BLE_PROV` is off by default and depends on `CONFIG_SPA_HMI_NET`.
The NimBLE host costs about 185 KB of flash — which is also why the partition
table had to change before this would fit at all (see `docs/MQTT.md`).

## Security, stated plainly

There is **no pairing, no bonding and no authentication**. Anyone in Bluetooth
range while the panel is advertising can read the broker credentials with
`broker_get` and can point the tub at a different Wi-Fi network. The app does not
ask for pairing, so adding it here would break the wizard.

What that does *not* give away: BLE cannot run the plant. The provisioning
service exposes no spa controls at all, and the S3 keeps its veto over everything
regardless of where a request came from.

**The board advertises continuously**, including after it is provisioned, so the
wizard works whenever the network changes rather than only at first boot. If that
trade is wrong for an installed tub, the fix is to stop advertising once
credentials are stored and re-enable it from a long-press on the panel — the
header's diagnostics gesture already exists for that kind of thing.
