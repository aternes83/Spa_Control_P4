/*
 * WiFi provisioning over BLE, as the SpaControl app already speaks it.
 *
 * Step 4 of docs/MQTT.md, and the thing that unties a board from the network it
 * happened to be flashed beside. Until this exists the only way to give the
 * panel a network is CONFIG_SPA_HMI_WIFI_SSID, which bakes somebody's password
 * into a binary and means a board cannot be moved, re-homed, or sold on.
 *
 * IMPORTANT — this is NOT ESP-IDF's wifi_provisioning component. The app is the
 * side that already exists, and it does not speak protocomm: SpaBLEProvisioner
 * in the app drives a Nordic UART Service carrying line-free JSON, matching the
 * peripheral on its advertised name alone. Using IDF's provisioning manager here
 * would produce a board the app cannot see, let alone talk to. The protocol is
 * written out in docs/BLE.md and enforced by tools/test_docs.py.
 *
 * The radio is on the ESP32-C6, as ever. The P4 runs the NimBLE host and the C6
 * runs only the controller, over the same internal SDIO bus that carries WiFi —
 * which the boot log calls "HCI over SDIO". The C6 still needs no new firmware.
 */
#ifndef BLE_PROV_H
#define BLE_PROV_H

#include <stdbool.h>

/* Start advertising as "SpaControl" and serve the provisioning protocol.
 * Returns immediately. Compiles to nothing without CONFIG_SPA_HMI_BLE_PROV. */
void ble_prov_start(void);

/* A phone is connected. Drives the header's Bluetooth indicator, which until
 * now has been hard-wired false. */
bool ble_prov_connected(void);

#endif /* BLE_PROV_H */
