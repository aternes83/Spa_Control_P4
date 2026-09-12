#include "mqtt_spa.h"

#include "sdkconfig.h"

#if CONFIG_SPA_HMI_MQTT

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_link.h"
#include "spalink_codec.h"

static const char *TAG = "mqtt";

/* Slow enough not to chatter, fast enough that an app opened cold shows the tub
 * within a couple of seconds of subscribing. Changes publish immediately. */
#define HEARTBEAT_MS  5000

static esp_mqtt_client_handle_t s_client;
static bool s_up;
static char s_topic_status[96];
static char s_topic_cmd[96];
static char s_last[512];          /* last payload, to publish only on change */
static uint32_t s_next_hb_ms;

bool mqtt_spa_up(void) { return s_up; }

/* ── Commands in ─────────────────────────────────────────────────────────── */

/* Every field is optional: Swift's encodeIfPresent omits nils, so a command
 * carries only what changed. An absent field must mean "unchanged", never
 * "off" — treating absent as false would turn a "set the light on" command into
 * an order to stop all three pumps. */
static void on_command(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable command, ignored");
        return;
    }

    ui_remote_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));

    const cJSON *j;
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "set_temp"))) {
        cmd.has_setpoint = true;
        /* Rounded, not truncated: the app sends whole degrees but a JSON float
         * of 102 can arrive as 101.99999. */
        cmd.setpoint_f = (int)(j->valuedouble + (j->valuedouble < 0 ? -0.5 : 0.5));
    }
    if (cJSON_IsNumber(j = cJSON_GetObjectItemCaseSensitive(root, "pump1"))) {
        int v = j->valueint;
        cmd.has_pump1 = true;
        cmd.pump1 = (uint8_t)(v < 0 ? 0 : (v > 2 ? 2 : v));
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItemCaseSensitive(root, "pump2"))) {
        cmd.has_pump2 = true; cmd.pump2 = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItemCaseSensitive(root, "pump3"))) {
        cmd.has_pump3 = true; cmd.pump3 = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItemCaseSensitive(root, "light"))) {
        cmd.has_light = true; cmd.light = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItemCaseSensitive(root, "eco"))) {
        cmd.has_eco = true; cmd.eco = cJSON_IsTrue(j);
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItemCaseSensitive(root, "max_jet"))) {
        cmd.has_max_jet = true; cmd.max_jet = cJSON_IsTrue(j);
    }

    /* Deliberately not handled, and silent about it rather than wrong:
     *   set_temp_cal  the probe is on the S3 and SpaLink has no message for it
     *                 (7-byte payload cap vs five floats) — docs/MQTT.md
     *   schedule      not implemented
     *   ota_apply     not implemented
     * Honouring any of these partially would be worse than not at all. */
    if (cJSON_GetObjectItemCaseSensitive(root, "set_temp_cal")) {
        ESP_LOGW(TAG, "set_temp_cal ignored: needs a SpaLink message (docs/MQTT.md)");
    }

    cJSON_Delete(root);

    /* Queued, not applied. The next ui_tick() applies it on the task that owns
     * the intent — see ui_post_remote(). */
    ui_post_remote(&cmd);
}

/* ── Status out ──────────────────────────────────────────────────────────── */

static int build_status(char *buf, size_t n, const spa_state_t *s, const ui_out_t *o)
{
    const uint8_t out = s->outputs;

    /* pump1 as 0/1/2 from the OUTPUT bits, so a pump the S3 is running for the
     * thermostat shows as running — the app's controls read status.pump1, and
     * v2.0's rule was "stay honest like the LCD". */
    int pump1 = (out & SPALINK_OUT_PUMP1_HIGH) ? 2
              : (out & SPALINK_OUT_PUMP1_LOW)  ? 1
                                               : 0;

    /* schedule_on, schedule_active, ota_avail and ota_state are omitted, not
     * sent false or null: they are optional in SpaStatus.swift, and omitting is
     * the honest encoding of "this firmware does not know". r_ohms likewise —
     * the S3 does not report resistance over SpaLink yet. */
    return snprintf(buf, n,
        "{\"id\":\"%s\""
        ",\"temp_f\":%.1f"
        ",\"setpoint\":%.1f"
        ",\"heater\":%s"
        ",\"pump1\":%d"
        ",\"pump2\":%s"
        ",\"pump3\":%s"
        ",\"light\":%s"
        ",\"eco\":%s"
        ",\"max_jet\":%s"
        ",\"fault\":%s"
        ",\"fault_code\":%d"
        ",\"fw\":\"%s\""
        ",\"link\":%s"
        "}",
        CONFIG_SPA_HMI_MQTT_ID,
        s->have_temp ? s->water_dF / 10.0 : 0.0,
        s->setpoint_dF / 10.0,
        (out & SPALINK_OUT_HEATER) ? "true" : "false",
        pump1,
        (out & SPALINK_OUT_PUMP2) ? "true" : "false",
        (out & SPALINK_OUT_PUMP3) ? "true" : "false",
        (out & SPALINK_OUT_LIGHT) ? "true" : "false",
        o->eco ? "true" : "false",
        o->max_jet ? "true" : "false",
        s->fault_active ? "true" : "false",
        s->fault_code,
        CONFIG_SPA_HMI_MQTT_FW,
        /* Not in the app's schema, and harmless there because an unknown key is
         * ignored. It is here because "the panel is talking to the broker but
         * not to the tub" is otherwise indistinguishable from a healthy spa
         * sitting idle, and that is exactly the case somebody debugging needs
         * to see. An absent link means every plant field above is stale. */
        s->link_up ? "true" : "false");
}

void mqtt_spa_publish(const spa_state_t *s, const ui_out_t *o, uint32_t now_ms)
{
    if (!s_up) {
        return;
    }

    char buf[512];
    int len = build_status(buf, sizeof(buf), s, o);
    if (len <= 0 || (size_t)len >= sizeof(buf)) {
        ESP_LOGE(TAG, "status payload did not fit — not publishing a truncated one");
        return;
    }

    bool due = (int32_t)(now_ms - s_next_hb_ms) >= 0;
    if (!due && strcmp(buf, s_last) == 0) {
        return;
    }
    s_next_hb_ms = now_ms + HEARTBEAT_MS;
    memcpy(s_last, buf, (size_t)len + 1);

    /* Retained: an app opening cold gets the tub's state immediately instead of
     * an empty screen until the next heartbeat. QoS 0 — the heartbeat is the
     * recovery mechanism, and a queue of stale spa states is worth nothing. */
    esp_mqtt_client_publish(s_client, s_topic_status, buf, len, 0, 1);
}

/* ── Plumbing ────────────────────────────────────────────────────────────── */

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        s_up = true;
        s_last[0] = '\0';        /* force a full publish on reconnect */
        s_next_hb_ms = 0;
        ESP_LOGI(TAG, "connected; publishing %s, listening on %s",
                 s_topic_status, s_topic_cmd);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_up = false;
        ESP_LOGW(TAG, "disconnected");
        break;

    case MQTT_EVENT_DATA:
        /* Only our command topic; the client is subscribed to nothing else, but
         * a wildcard added later should not silently start feeding the plant. */
        if (e->topic_len && strncmp(e->topic, s_topic_cmd, (size_t)e->topic_len) == 0) {
            on_command(e->data, e->data_len);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "error (transport %d)", e->error_handle->error_type);
        break;

    default:
        break;
    }
}

static void start_when_ready(void *arg)
{
    (void)arg;
    /* No timeout: a tub can sit for hours with the router off, and the right
     * behaviour is to connect whenever the network finally appears. */
    while (!net_link_wait_ip(30000)) {
        ESP_LOGD(TAG, "waiting for an address before opening the broker socket");
    }
    ESP_LOGI(TAG, "network up — connecting to the broker");
    esp_mqtt_client_start(s_client);
    vTaskDelete(NULL);
}

void mqtt_spa_start(void)
{
    if (CONFIG_SPA_HMI_MQTT_URI[0] == '\0') {
        ESP_LOGW(TAG, "no broker configured — set CONFIG_SPA_HMI_MQTT_URI");
        return;
    }

    /* With a device id both topics gain it, which is how more than one tub
     * shares a broker; without one they are the bare topics the app falls back
     * to. Both forms are in BrokerSettings.swift. */
    if (CONFIG_SPA_HMI_MQTT_ID[0] != '\0') {
        snprintf(s_topic_status, sizeof(s_topic_status), "spa/%s/status", CONFIG_SPA_HMI_MQTT_ID);
        snprintf(s_topic_cmd, sizeof(s_topic_cmd), "spa/%s/commands", CONFIG_SPA_HMI_MQTT_ID);
    } else {
        snprintf(s_topic_status, sizeof(s_topic_status), "spa/status");
        snprintf(s_topic_cmd, sizeof(s_topic_cmd), "spa/commands");
    }

    /* mqtts:// needs a trust anchor or the handshake fails with nothing useful
     * to show for it. IDF's bundled roots cover the public CAs that hosted
     * brokers use, which is the whole reason TLS is affordable here. */
    const bool tls = (strncmp(CONFIG_SPA_HMI_MQTT_URI, "mqtts://", 8) == 0) ||
                     (strncmp(CONFIG_SPA_HMI_MQTT_URI, "wss://", 6) == 0);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_SPA_HMI_MQTT_URI,
        .broker.verification.crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
        .credentials.username = CONFIG_SPA_HMI_MQTT_USER,
        .credentials.authentication.password = CONFIG_SPA_HMI_MQTT_PASS,
        .session.keepalive = 30,
        /* Nothing here is worth the flash wear of a persistent session, and a
         * reconnect republishes everything anyway. */
        .session.disable_clean_session = false,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);

    /* Starting the client is what must wait, not creating it.
     *
     * esp-mqtt does tolerate having no address — it retries on its own — but it
     * does NOT tolerate an uninitialised TCP/IP stack, and lwIP comes up on
     * net_link's task well after app_main has run. Calling start() here directly
     * panics with "assert failed: tcpip_send_msg_wait_sem ... (Invalid mbox)"
     * and boot-loops the panel. That stayed hidden for as long as no broker was
     * configured, because then start() was never reached at all. */
    xTaskCreate(start_when_ready, "mqtt_up", 3072, NULL, 4, NULL);
}

#else  /* !CONFIG_SPA_HMI_MQTT */

void mqtt_spa_start(void) { }
void mqtt_spa_publish(const spa_state_t *s, const ui_out_t *o, uint32_t now_ms)
{
    (void)s; (void)o; (void)now_ms;
}
bool mqtt_spa_up(void) { return false; }

#endif /* CONFIG_SPA_HMI_MQTT */
