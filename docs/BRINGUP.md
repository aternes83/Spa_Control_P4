# Bringing up a fresh P4 panel

About two minutes at the bench, plus the app wizard. The C6 keeps its stock image
and needs nothing (`docs/HARDWARE.md`).

## 1. Flash

Plug the panel's USB-Serial/JTAG port into the Mac, then from the repo root:

```sh
tools/flash_p4.sh                  # or: tools/flash_p4.sh /dev/cu.usbmodemXXXXX
```

Add `--erase` to wipe the P4's whole flash first, for a board with an unknown past
or to start over. It clears the timezone, so the clock needs step 2 again. It does
not clear WiFi, which is kept on the C6 (see step 2).

The build takes its broker login from `p4_hmi/sdkconfig.local`, which is gitignored
and lives only on the machine that made it. Without it the script refuses to run.

The port is named by its USB position, so a replacement board in the same socket
usually gets the same one. If another Espressif board is attached, check which is
which before passing a port: the S3 may be on the other one, and the panel flash
must never go there. `--chip esp32p4` makes esptool refuse the S3, but probing the
port still resets it.

Use the script rather than `idf.py flash`. On this Mac esptool only connects
reliably with `--before usb_reset`, and it must be the one in the IDF environment:
the older copy on the system Python hung on connect.

## 2. Provision

WiFi and the timezone belong to the board and do not come with the firmware. The
timezone is in the P4's NVS. WiFi credentials are held by the C6, through
`esp_wifi` over SDIO: on the bench a full P4 erase left the panel still reporting
`using the provisioned network`. So a module that has been on a network before
may join it again with no setup.

On a new module the panel logs `no network configured`. Without a timezone it logs
`no timezone set` and the clock reads `--:--`.

Run the app's WiFi-setup wizard against the panel, which advertises as
`SpaControl` (`docs/BLE.md`). That sets both.

## 3. Confirm

* The panel joins WiFi and `spa/<id>/online` reads a retained `true` (`docs/MQTT.md`).
* The clock shows the time.

## Replacing a panel

The device id is compiled in (`CONFIG_SPA_HMI_MQTT_ID`, `spa1` here), so a new
board takes over the old one's identity. Do not run both powered and on a network
at once: both would publish to the same `spa/<id>/` topics and answer the same
commands, and one board's last will could mark the other offline.
