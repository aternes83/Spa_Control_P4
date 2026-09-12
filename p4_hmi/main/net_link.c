#include "net_link.h"

#include "sdkconfig.h"

#if CONFIG_SPA_HMI_NET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "net";

/* Backoff between association attempts. A tub panel has all day, and hammering
 * a router that is simply off is how you end up in its blocklist. */
#define RETRY_MIN_MS   2000
#define RETRY_MAX_MS  60000

static bool     s_up;                    /* associated and holding a lease */
static bool     s_provisioned;
static char     s_ip[16] = "-";
static uint32_t s_backoff_ms = RETRY_MIN_MS;
static esp_timer_handle_t s_retry;

bool net_link_up(void)          { return s_up; }
bool net_link_provisioned(void) { return s_provisioned; }
const char *net_link_ip(void)   { return s_ip; }

static void retry_connect(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        /* Only when there is something to connect to. The self-test starts the
         * radio with no credentials on purpose, and connecting anyway just
         * earns an RPC error from the C6 and a confusing line in the log. */
        if (s_provisioned) {
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_CONNECTED:
        /* Associated, but not up: there is no address yet, and MQTT cannot run
         * on an association alone. s_up waits for the lease. */
        ESP_LOGI(TAG, "associated, waiting for an address");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        s_up = false;
        strcpy(s_ip, "-");
        /* The reason code is the whole diagnosis here — 15 is a bad password,
         * 201 is an SSID that isn't there, and telling those apart from the
         * console saves taking the panel off the wall. */
        ESP_LOGW(TAG, "disconnected, reason %u — retrying in %u ms",
                 (unsigned)d->reason, (unsigned)s_backoff_ms);
        /* Armed on a timer rather than slept on. This handler runs on the event
         * loop task, so blocking it here would also stall the DHCP lease and
         * every other event in the system. */
        esp_timer_start_once(s_retry, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms *= 2;
        if (s_backoff_ms > RETRY_MAX_MS) {
            s_backoff_ms = RETRY_MAX_MS;
        }
        break;
    }
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *e = data;

    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_backoff_ms = RETRY_MIN_MS;
    s_up = true;
    ESP_LOGI(TAG, "up, address %s", s_ip);

    /* The clock the header has been drawing as "--:--" since this panel was
     * first flashed. It stays "--:--" unless a timezone has been set, because
     * UTC on a wall panel is a wrong time, and docs/HMI.md is explicit that a
     * wrong time is worse than no time. */
    if (CONFIG_SPA_HMI_TZ[0] != '\0') {
        setenv("TZ", CONFIG_SPA_HMI_TZ, 1);
        tzset();
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.wait_for_sync = false;     /* never block an event handler */
        cfg.start = true;
        if (esp_netif_sntp_init(&cfg) == ESP_OK) {
            ESP_LOGI(TAG, "sntp started, TZ=%s", CONFIG_SPA_HMI_TZ);
        }
    } else {
        ESP_LOGW(TAG, "no CONFIG_SPA_HMI_TZ — the clock stays --:--");
    }
}

/* A blocking scan. Only ever called from net_task before it exits, never from
 * an event handler, so blocking here costs nothing. */
static void radio_selftest(void)
{
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed — the C6 link is the problem, not the config");
        return;
    }
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        ESP_LOGE(TAG, "scan failed");
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    ESP_LOGI(TAG, "radio OK — the C6 answered and found %u networks", (unsigned)n);

    /* A few of the strongest, so the log shows the radio actually hearing the
     * world rather than just returning a number. */
    uint16_t want = n < 8 ? n : 8;
    if (want) {
        wifi_ap_record_t *aps = calloc(want, sizeof(*aps));
        if (aps && esp_wifi_scan_get_ap_records(&want, aps) == ESP_OK) {
            for (uint16_t i = 0; i < want; i++) {
                ESP_LOGI(TAG, "  %4d dBm  ch%-3d  %s",
                         aps[i].rssi, aps[i].primary, (char *)aps[i].ssid);
            }
        }
        free(aps);
    }
}

static void net_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    /* Plain esp_wifi_init. esp_wifi_remote forwards every one of these calls
     * over SDIO to the C6, which is why nothing here mentions the C6 at all.
     * If this is where it fails, the radio link is the problem, not the code. */
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL));

    const esp_timer_create_args_t targs = {
        .callback = retry_connect,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Credentials, in the order that lets BLE provisioning replace menuconfig
     * without touching this file: whatever was provisioned into NVS wins, and
     * the build-time strings are only a bench shortcut for proving the radio.
     * esp_wifi persists to NVS itself, so a provisioned board keeps its network
     * across a reflash of the application. */
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != '\0') {
        s_provisioned = true;
        ESP_LOGI(TAG, "using the provisioned network \"%s\"", (char *)wc.sta.ssid);
    } else if (CONFIG_SPA_HMI_WIFI_SSID[0] != '\0') {
        memset(&wc, 0, sizeof(wc));
        strlcpy((char *)wc.sta.ssid, CONFIG_SPA_HMI_WIFI_SSID, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, CONFIG_SPA_HMI_WIFI_PASS, sizeof(wc.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        s_provisioned = true;
        ESP_LOGI(TAG, "using the build-time network \"%s\" (bench only)",
                 CONFIG_SPA_HMI_WIFI_SSID);
    } else {
        /* Nobody has told this board a network yet. That is not an error and
         * not something to retry — BLE provisioning is what fixes it. But the
         * radio can still be proved without credentials, and it is worth
         * proving: a scan exercises the whole path this board is new at — the
         * C6 out of reset, the SDIO link, the stock slave image, the remote
         * esp_wifi API — and leaves only the password untested. Far better a
         * board that says "the radio works, it has no network" than one that
         * says nothing and leaves both possibilities open. */
        ESP_LOGW(TAG, "no network configured — scanning to prove the radio, then idling.");
        ESP_LOGW(TAG, "Set one under menuconfig -> Spa HMI, or wait for BLE provisioning.");
        radio_selftest();
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelete(NULL);          /* the event handlers own it from here */
}

void net_link_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Off the calling thread: bringing the C6 out of reset and negotiating the
     * SDIO link takes over a second (ESP_HOSTED_SDIO_RESET_DELAY_MS is 1500),
     * and the screen must not wait on it. */
    xTaskCreate(net_task, "net_up", 5120, NULL, 4, NULL);
}

#else  /* !CONFIG_SPA_HMI_NET */

void net_link_start(void)       { }
bool net_link_up(void)          { return false; }
bool net_link_provisioned(void) { return false; }
const char *net_link_ip(void)   { return "-"; }

#endif /* CONFIG_SPA_HMI_NET */
