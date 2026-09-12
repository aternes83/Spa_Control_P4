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
#include "esp_hosted.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
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
static EventGroupHandle_t s_events;
#define EV_HOSTED_UP  BIT0
static bool          s_started;        /* esp_wifi_start() has returned */
static bool          s_prov_active;    /* an attempt is outstanding */
static bool          s_sntp_running;
static char          s_tz[64];
static net_prov_cb_t s_prov_cb;
static void         *s_prov_ctx;

bool net_link_up(void)          { return s_up; }

bool net_link_wait_hosted(uint32_t timeout_ms)
{
    if (!s_events) {
        return false;
    }
    return (xEventGroupWaitBits(s_events, EV_HOSTED_UP, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(timeout_ms)) & EV_HOSTED_UP) != 0;
}
bool net_link_provisioned(void) { return s_provisioned; }
const char *net_link_ip(void)   { return s_ip; }

/* The app shows this to somebody standing at the tub, so it has to say what to
 * do about it rather than quote a number at them. */
static const char *reason_text(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "network not found";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network too weak here";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "the router refused the connection";
    default:
        return NULL;        /* caller falls back to the number */
    }
}

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
        if (s_prov_active) {
            /* Do not fall into the backoff loop: somebody is standing there
             * waiting to be told whether the password was right. Reporting the
             * first failure is the honest answer, and retrying a wrong password
             * forever would leave the app spinning with nothing to show. */
            s_prov_active = false;
            char why[48];
            const char *t = reason_text(d->reason);
            if (t) {
                snprintf(why, sizeof(why), "%s", t);
            } else {
                snprintf(why, sizeof(why), "wifi error %u", (unsigned)d->reason);
            }
            ESP_LOGW(TAG, "provisioning failed: %s (reason %u)", why, (unsigned)d->reason);
            if (s_prov_cb) {
                s_prov_cb(s_prov_ctx, NET_PROV_FAILED, why);
            }
            break;
        }
        if (!s_provisioned) {
            break;              /* nothing to retry towards */
        }
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

/* Start SNTP once, and only when there is a zone to interpret the result in.
 * Without one the header keeps "--:--": UTC on a wall panel is a wrong time, and
 * docs/HMI.md is explicit that a wrong time is worse than no time. */
static void start_sntp(void)
{
    if (s_sntp_running || !s_up || s_tz[0] == '\0') {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.wait_for_sync = false;      /* never block an event handler */
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_running = true;
        ESP_LOGI(TAG, "sntp started, TZ=%s", s_tz);
    }
}

const char *net_link_tz(void) { return s_tz; }

/* Load the stored zone, falling back to the build-time one. The stored value
 * wins: a board provisioned in Denver should not revert to whatever zone its
 * firmware was built with the next time it is reflashed. */
static void load_tz(void)
{
    nvs_handle_t h;
    s_tz[0] = '\0';

    if (nvs_open("spa", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_tz);
        if (nvs_get_str(h, "tz", s_tz, &len) != ESP_OK) {
            s_tz[0] = '\0';
        }
        nvs_close(h);
    }
    if (s_tz[0] == '\0' && CONFIG_SPA_HMI_TZ[0] != '\0') {
        strlcpy(s_tz, CONFIG_SPA_HMI_TZ, sizeof(s_tz));
        ESP_LOGI(TAG, "no stored timezone — using the built-in %s", s_tz);
    }
    if (s_tz[0] != '\0') {
        setenv("TZ", s_tz, 1);
        tzset();
    } else {
        ESP_LOGW(TAG, "no timezone set — the clock stays --:-- until one arrives");
    }
}

void net_link_set_tz(const char *posix_tz)
{
    char next[sizeof(s_tz)];
    next[0] = '\0';
    if (posix_tz) {
        strlcpy(next, posix_tz, sizeof(next));
    }
    if (strcmp(next, s_tz) == 0) {
        return;                     /* the app re-sends it on every wizard run */
    }
    strlcpy(s_tz, next, sizeof(s_tz));

    nvs_handle_t h;
    if (nvs_open("spa", NVS_READWRITE, &h) == ESP_OK) {
        if (s_tz[0]) {
            nvs_set_str(h, "tz", s_tz);
        } else {
            nvs_erase_key(h, "tz");
        }
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "could not store the timezone — it will not survive a reboot");
    }

    if (s_tz[0]) {
        setenv("TZ", s_tz, 1);
        tzset();
        time_t raw = time(NULL);
        struct tm tm;
        localtime_r(&raw, &tm);
        /* Say what the panel now believes, not merely that it was told: a clock
         * out by a whole timezone looks exactly like a correct one. */
        ESP_LOGI(TAG, "timezone set to %s — local time now %04d-%02d-%02d %02d:%02d",
                 s_tz, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
        start_sntp();
    } else {
        unsetenv("TZ");
        tzset();
        ESP_LOGW(TAG, "timezone cleared — the clock goes back to --:--");
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

    if (s_prov_active) {
        s_prov_active = false;
        s_provisioned = true;
        if (s_prov_cb) {
            s_prov_cb(s_prov_ctx, NET_PROV_OK, s_ip);
        }
    }

    start_sntp();
}

/* A blocking scan. Only ever called from net_task before it exits, never from
 * an event handler, so blocking here costs nothing. */
/* One scan, blocking. Safe from any task that is allowed to block — which is
 * why the BLE provisioner calls it from a worker and never from a GATT
 * callback, where blocking would stall the whole BLE stack. */
int net_link_scan(net_scan_cb_t cb, void *ctx)
{
    if (!s_started) {
        return -1;
    }
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return -1;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return 0;
    }
    if (n > 32) {
        n = 32;                 /* more than anyone will scroll through */
    }

    wifi_ap_record_t *aps = calloc(n, sizeof(*aps));
    if (!aps) {
        return -1;
    }
    int reported = 0;
    if (esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            if (aps[i].ssid[0] == '\0') {
                continue;       /* hidden: nothing the app could offer to tap */
            }
            if (cb) {
                cb(ctx, (const char *)aps[i].ssid, aps[i].rssi,
                   aps[i].authmode != WIFI_AUTH_OPEN);
            }
            reported++;
        }
    }
    free(aps);
    return reported;
}

void net_link_provision(const char *ssid, const char *pass,
                        net_prov_cb_t cb, void *ctx)
{
    s_prov_cb = cb;
    s_prov_ctx = ctx;

    if (!s_started || !ssid || ssid[0] == '\0') {
        if (cb) {
            cb(ctx, NET_PROV_FAILED, "no network name");
        }
        return;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (pass) {
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    }

    /* Storage is WIFI_STORAGE_FLASH by default, so this call is also what
     * persists the credentials — the board stops being tied to whatever network
     * it happened to be flashed beside, which is the entire point. */
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        if (cb) {
            cb(ctx, NET_PROV_FAILED, "could not store the credentials");
        }
        return;
    }

    s_up = false;
    strcpy(s_ip, "-");
    s_prov_active = true;
    s_backoff_ms = RETRY_MIN_MS;
    if (cb) {
        cb(ctx, NET_PROV_CONNECTING, NULL);
    }

    esp_wifi_disconnect();      /* drop any earlier association first */
    if (esp_wifi_connect() != ESP_OK) {
        s_prov_active = false;
        if (cb) {
            cb(ctx, NET_PROV_FAILED, "the radio refused to connect");
        }
    }
}

/* Log-only proof of life, for a build with no provisioner to ask for a scan. */
static void log_scan_result(void *ctx, const char *ssid, int rssi, bool secured)
{
    (void)ctx;
    ESP_LOGI(TAG, "  %4d dBm  %s%s", rssi, ssid, secured ? "" : "  (open)");
}


static void net_task(void *arg)
{
    (void)arg;

    /* Bring the C6 up first, explicitly, and announce it. esp_wifi_init() would
     * do this implicitly, but then it is a side effect nothing else can wait on,
     * and whoever needs the BT controller has to guess when the co-processor is
     * ready. It takes about two seconds from reset. */
    if (esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "the C6 never came up — no radio of any kind");
        vTaskDelete(NULL);
        return;
    }
    xEventGroupSetBits(s_events, EV_HOSTED_UP);

    load_tz();

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
        ESP_LOGW(TAG, "no network configured — the radio comes up anyway, for provisioning.");
        ESP_LOGW(TAG, "Provision over BLE from the app, or set one under menuconfig -> Spa HMI.");
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    if (!s_provisioned) {
        /* Proof of life while nothing has asked for a scan yet: it exercises
         * the C6 out of reset, the SDIO link, the slave image and the remote
         * esp_wifi API, leaving only the password untested. A board that can
         * say "the radio works, it has no network" is worth far more than one
         * that says nothing and leaves both possibilities open. */
        int n = net_link_scan(log_scan_result, NULL);
        if (n >= 0) {
            ESP_LOGI(TAG, "radio OK — the C6 answered and found %d networks", n);
        } else {
            ESP_LOGE(TAG, "scan failed — the C6 link is the problem, not the config");
        }
    }

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

    s_events = xEventGroupCreate();
    if (!s_events) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    /* Off the calling thread: bringing the C6 out of reset and negotiating the
     * SDIO link takes over a second (ESP_HOSTED_SDIO_RESET_DELAY_MS is 1500),
     * and the screen must not wait on it. */
    xTaskCreate(net_task, "net_up", 5120, NULL, 4, NULL);
}

#else  /* !CONFIG_SPA_HMI_NET */

void net_link_start(void)       { }
bool net_link_up(void)          { return false; }
int  net_link_scan(net_scan_cb_t cb, void *ctx) { (void)cb; (void)ctx; return -1; }
void net_link_set_tz(const char *posix_tz) { (void)posix_tz; }
const char *net_link_tz(void) { return ""; }
bool net_link_wait_hosted(uint32_t timeout_ms) { (void)timeout_ms; return false; }
void net_link_provision(const char *ssid, const char *pass,
                        net_prov_cb_t cb, void *ctx)
{
    (void)ssid; (void)pass;
    if (cb) {
        cb(ctx, NET_PROV_FAILED, "networking is not built into this firmware");
    }
}
bool net_link_provisioned(void) { return false; }
const char *net_link_ip(void)   { return "-"; }

#endif /* CONFIG_SPA_HMI_NET */
