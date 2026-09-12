/*
 * The radio, and nothing above it yet.
 *
 * Step 1 of docs/MQTT.md: associate through the ESP32-C6 on the module and get
 * an address. Nothing beyond that is worth writing until this works, because
 * everything above it is standard ESP-IDF and this part is the only bit that
 * depends on Guition's wiring and their stock slave image.
 *
 * The C6 is a radio and only a radio. It runs the vendor's esp_hosted slave
 * image unmodified; the TCP/IP stack, the MQTT client and the BLE provisioner
 * all live here on the P4 and reach it over the module's internal SDIO bus. So
 * this file uses the plain esp_wifi_* API — esp_wifi_remote forwards it across
 * the link, which is why there is nothing C6-specific in the code below.
 *
 * Everything compiles out when CONFIG_SPA_HMI_NET is off, and that is the
 * default: the panel's job is the tub, and a screen that runs the plant must not
 * start depending on a network to do it.
 */
#ifndef NET_LINK_H
#define NET_LINK_H

#include <stdbool.h>

/* Bring the radio up in the background. Returns immediately and never blocks
 * boot: the panel has to come up and show the tub whether or not there is a
 * network, so association failures are logged and retried, never fatal. */
void net_link_start(void);

/* Associated AND holding an address. Both, because an association without a
 * lease cannot carry MQTT and showing it as "up" would be a lie of the same
 * kind the rest of this firmware avoids. */
bool net_link_up(void);

/* Credentials exist — from menuconfig on the bench, or from NVS once BLE
 * provisioning writes them. False means nobody has told this board a network
 * yet, which is a different thing from a network that will not associate. */
bool net_link_provisioned(void);

/* Dotted-quad, or "-" when there is no lease. For the diagnostics screen. */
const char *net_link_ip(void);

#endif /* NET_LINK_H */
