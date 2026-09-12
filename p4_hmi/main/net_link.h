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
#include <stdint.h>

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

/* ── What BLE provisioning needs ─────────────────────────────────────────────
 *
 * The radio is brought up and left running even with no credentials, precisely
 * so these two work before the board has ever been on a network.
 */

/* Block until the C6's SDIO transport has been negotiated, or the timeout
 * expires. Returns whether it came up.
 *
 * Exactly one place in this firmware may bring that transport up, and it is
 * net_link. Two tasks calling into it concurrently do not both win and do not
 * fail cleanly either — the SD card initialisation itself fails, and every
 * subsequent symptom points at the wiring rather than at the race. Anything
 * else that needs the C6 (the BLE controller, say) waits here instead. */
bool net_link_wait_hosted(uint32_t timeout_ms);

/* One scan, blocking, cb per network found. Called from the provisioner's own
 * worker task — never from a BLE callback, which must not block. Returns the
 * number reported, or -1 if the radio is not up yet. */
typedef void (*net_scan_cb_t)(void *ctx, const char *ssid, int rssi, bool secured);
int net_link_scan(net_scan_cb_t cb, void *ctx);

/* Set this tub's timezone, as a POSIX TZ string, and remember it.
 *
 * Per board, not per build. The phone that runs the wizard is standing next to
 * the tub, so it is the only thing that reliably knows where the tub is — and a
 * timezone compiled in means every board has to be rebuilt to be installed
 * somewhere else. Stored in NVS, so it survives a reflash of the application.
 *
 * POSIX form ("EST5EDT,M3.2.0,M11.1.0"), not IANA ("America/New_York"): newlib
 * on this chip carries no timezone database and would silently ignore the
 * latter, leaving a clock that is confidently wrong. The app converts.
 *
 * Applies immediately — the clock does not wait for a reboot — and starts SNTP
 * if an address is already held. NULL or empty clears it, and the header goes
 * back to "--:--". */
void net_link_set_tz(const char *posix_tz);

/* What is in effect, or "" if none. For the diagnostics screen. */
const char *net_link_tz(void);

/* How a provisioning attempt ended. */
typedef enum {
    NET_PROV_CONNECTING,
    NET_PROV_OK,        /* detail is the address */
    NET_PROV_FAILED,    /* detail is why, in words the app can show a user */
} net_prov_state_t;

typedef void (*net_prov_cb_t)(void *ctx, net_prov_state_t state, const char *detail);

/* Try these credentials. Returns immediately; the outcome arrives on cb.
 *
 * esp_wifi persists to NVS, so a successful attempt is what unties the board
 * from the network it was flashed next to — which is the entire point of
 * provisioning, and why CONFIG_SPA_HMI_WIFI_SSID is only ever a bench shortcut. */
void net_link_provision(const char *ssid, const char *pass,
                        net_prov_cb_t cb, void *ctx);

#endif /* NET_LINK_H */
