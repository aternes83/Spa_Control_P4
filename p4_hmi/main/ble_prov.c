#include "ble_prov.h"

#include "sdkconfig.h"

#if CONFIG_SPA_HMI_BLE_PROV

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "net_link.h"

static const char *TAG = "ble";

/* The name the app matches on. It scans with no service filter and compares the
 * advertised local name, so this string is load-bearing: change it and the
 * wizard simply never finds the tub. */
#define DEV_NAME "SpaControl"

/* Nordic UART Service. Not a standard, but the de-facto one for "a serial port
 * over BLE", and what the app was written against.
 *   6E400001-...  service
 *   6E400002-...  RX, the app writes here
 *   6E400003-...  TX, we notify on this
 * NimBLE wants 128-bit UUIDs little-endian, hence the reversed byte order. */
#define NUS_UUID_BASE(b1, b0) \
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                     0x93, 0xf3, 0xa3, 0xb5, b0, b1, 0x40, 0x6e)

static const ble_uuid128_t nus_svc_uuid = NUS_UUID_BASE(0x00, 0x01);
static const ble_uuid128_t nus_rx_uuid  = NUS_UUID_BASE(0x00, 0x02);
static const ble_uuid128_t nus_tx_uuid  = NUS_UUID_BASE(0x00, 0x03);

static uint8_t  s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static QueueHandle_t s_work;

/* What the worker has been asked to do. Kept small and copied by value: the
 * queue is the boundary between the BLE stack and anything that blocks. */
typedef enum { JOB_SCAN, JOB_PROVISION, JOB_BROKER, JOB_TZ } job_kind_t;

typedef struct {
    job_kind_t kind;
    char ssid[33];
    char pass[65];
    char tz[64];        /* POSIX form, from the phone; "" if it sent none */
} job_t;

bool ble_prov_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

/* ── Sending ─────────────────────────────────────────────────────────────── */

/* One JSON object to the app, fragmented to whatever the negotiated MTU allows.
 * Fragmenting is safe and expected: the app reassembles by counting braces
 * rather than trusting notification boundaries, precisely because BLE gives no
 * guarantee that one notify equals one message. */
static void notify_json(const char *json)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_tx_handle) {
        return;
    }

    size_t len = strlen(json);
    uint16_t mtu = ble_att_mtu(s_conn);
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off;
        if (n > chunk) {
            n = chunk;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(json + off, n);
        if (!om) {
            ESP_LOGW(TAG, "out of mbufs — reply dropped: %s", json);
            return;
        }
        int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
        if (rc != 0) {
            /* Silence here is the worst outcome: the phone sits waiting for a
             * reply that was never sent, and nothing anywhere says so. */
            ESP_LOGW(TAG, "notify failed (rc %d) — the phone will not see: %s", rc, json);
            return;         /* the mbuf is consumed either way */
        }
    }
}

/* ── The jobs ────────────────────────────────────────────────────────────── */

static void on_network(void *ctx, const char *ssid, int rssi, bool secured)
{
    (void)ctx;

    /* cJSON does the escaping, because an SSID is user-supplied text that may
     * contain a quote or a backslash, and hand-rolling that into a format
     * string is how a scan result becomes malformed JSON the app silently
     * drops. */
    cJSON *net = cJSON_CreateObject();
    if (!net) {
        return;
    }
    cJSON_AddStringToObject(net, "s", ssid);
    cJSON_AddNumberToObject(net, "r", rssi);
    cJSON_AddNumberToObject(net, "sec", secured ? 1 : 0);

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddItemToObject(root, "net", net);
        char *txt = cJSON_PrintUnformatted(root);
        if (txt) {
            notify_json(txt);
            cJSON_free(txt);
        }
        cJSON_Delete(root);
    } else {
        cJSON_Delete(net);
    }
}

static void on_prov_state(void *ctx, net_prov_state_t st, const char *detail)
{
    (void)ctx;
    char buf[160];

    switch (st) {
    case NET_PROV_CONNECTING:
        notify_json("{\"wifi\":\"connecting\"}");
        break;
    case NET_PROV_OK:
        snprintf(buf, sizeof(buf), "{\"wifi\":\"ok\",\"ip\":\"%s\"}",
                 detail ? detail : "");
        notify_json(buf);
        break;
    case NET_PROV_FAILED:
        snprintf(buf, sizeof(buf), "{\"wifi\":\"fail\",\"err\":\"%s\"}",
                 detail ? detail : "unknown");
        notify_json(buf);
        break;
    }
}

/* The app asks for these so it can configure itself against the same broker the
 * panel uses, instead of making somebody type a host and port twice. */
static void send_broker(void)
{
    /* Say nothing rather than something false. An empty host is not broker
     * settings, and the app saves whatever arrives — so replying with a blank
     * object overwrites a broker the user may already have working. Its wizard
     * anticipates exactly this ("Fallback so Done isn't blocked if the board
     * reports no broker") and finishes on a timer instead. */
    if (CONFIG_SPA_HMI_MQTT_URI[0] == '\0') {
        ESP_LOGW(TAG, "no broker configured — telling the phone nothing rather "
                      "than an empty one, which it would save over a good one");
        return;
    }

    cJSON *b = cJSON_CreateObject();
    if (!b) {
        return;
    }

    /* The client takes a whole URI; the app wants host and port apart. Split
     * rather than add a second setting to disagree with the first. */
    const char *uri = CONFIG_SPA_HMI_MQTT_URI;
    char host[96] = "";
    int port = 0;
    const char *p = strstr(uri, "://");
    bool tls = (strncmp(uri, "mqtts://", 8) == 0) || (strncmp(uri, "wss://", 6) == 0);
    p = p ? p + 3 : uri;
    const char *colon = strrchr(p, ':');
    const char *slash = strchr(p, '/');
    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl >= sizeof(host)) {
            hl = sizeof(host) - 1;
        }
        memcpy(host, p, hl);
        host[hl] = '\0';
        port = atoi(colon + 1);
    } else {
        size_t hl = slash ? (size_t)(slash - p) : strlen(p);
        if (hl >= sizeof(host)) {
            hl = sizeof(host) - 1;
        }
        memcpy(host, p, hl);
        host[hl] = '\0';
    }
    if (port == 0) {
        port = tls ? 8883 : 1883;
    }

    cJSON_AddStringToObject(b, "host", host);
    cJSON_AddNumberToObject(b, "port", port);
    cJSON_AddStringToObject(b, "user", CONFIG_SPA_HMI_MQTT_USER);
    cJSON_AddStringToObject(b, "pw", CONFIG_SPA_HMI_MQTT_PASS);
    cJSON_AddStringToObject(b, "device_id", CONFIG_SPA_HMI_MQTT_ID);

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddItemToObject(root, "broker", b);
        char *txt = cJSON_PrintUnformatted(root);
        if (txt) {
            ESP_LOGI(TAG, "sending broker settings: %s", txt);
            notify_json(txt);
            cJSON_free(txt);
        }
        cJSON_Delete(root);
    } else {
        cJSON_Delete(b);
    }
}

/* Everything that can block lives here, off the BLE stack's own task. A scan
 * takes seconds and an association can take longer; doing either inside a GATT
 * write callback would stall the host and drop the connection that is waiting
 * to be told the answer. */
static void worker(void *arg)
{
    (void)arg;
    job_t job;

    for (;;) {
        if (xQueueReceive(s_work, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (job.kind) {
        case JOB_SCAN:
            ESP_LOGI(TAG, "phone asked for a network scan");
            notify_json("{\"scan\":\"begin\"}");
            net_link_scan(on_network, NULL);
            /* "end" goes out even when the scan failed or found nothing: the
             * app leaves its wizard spinning until it arrives. An empty list is
             * a result; silence is a hang. */
            notify_json("{\"scan\":\"end\"}");
            break;

        case JOB_PROVISION:
            ESP_LOGI(TAG, "provisioning onto \"%s\"", job.ssid);
            /* Before associating, so SNTP has a zone the moment the lease
             * lands rather than a second sync later. A phone that sends no
             * timezone leaves whatever the tub already had — an older app
             * build must not wipe a correct setting. */
            if (job.tz[0]) {
                net_link_set_tz(job.tz);
            }
            net_link_provision(job.ssid, job.pass, on_prov_state, NULL);
            break;

        case JOB_TZ:
            /* On its own, so moving a tub — or a phone crossing into DST —
             * does not mean re-entering the WiFi password. */
            net_link_set_tz(job.tz);
            notify_json("{\"tz\":\"ok\"}");
            break;

        case JOB_BROKER:
            ESP_LOGI(TAG, "phone asked for the broker settings");
            send_broker();
            break;
        }
        /* Do not keep somebody's WiFi password in this task's stack any longer
         * than the call that needed it. */
        memset(&job, 0, sizeof(job));
    }
}

/* ── Receiving ───────────────────────────────────────────────────────────── */

static void handle_command(const char *data, uint16_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "unparseable command, ignored");
        return;
    }
    /* Not the payload: it carries the WiFi password. The key names are enough
     * to follow a wizard run without printing somebody's secret to a console. */
    {
        const cJSON *it = NULL;
        char keys[96];
        size_t k = 0;
        cJSON_ArrayForEach(it, root) {
            if (it->string) {
                int w = snprintf(keys + k, sizeof(keys) - k, "%s%s", k ? "," : "", it->string);
                if (w < 0 || (size_t)w >= sizeof(keys) - k) {
                    break;
                }
                k += (size_t)w;
            }
        }
        ESP_LOGI(TAG, "command from phone: {%s}", keys);
    }

    job_t job;
    memset(&job, 0, sizeof(job));
    bool have = false;

    /* Carried alongside the credentials, and accepted on its own. The phone is
     * the only thing that reliably knows where this tub is. */
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "tz");

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
    if (cJSON_IsString(ssid) && ssid->valuestring) {
        const cJSON *pw = cJSON_GetObjectItemCaseSensitive(root, "wifi_pw");
        job.kind = JOB_PROVISION;
        strlcpy(job.ssid, ssid->valuestring, sizeof(job.ssid));
        if (cJSON_IsString(pw) && pw->valuestring) {
            strlcpy(job.pass, pw->valuestring, sizeof(job.pass));
        }
        if (cJSON_IsString(tz) && tz->valuestring) {
            strlcpy(job.tz, tz->valuestring, sizeof(job.tz));
        }
        have = true;
    } else if (cJSON_IsString(tz) && tz->valuestring) {
        job.kind = JOB_TZ;
        strlcpy(job.tz, tz->valuestring, sizeof(job.tz));
        have = true;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "wifi_scan")) {
        job.kind = JOB_SCAN;
        have = true;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "broker_get")) {
        job.kind = JOB_BROKER;
        have = true;
    }

    cJSON_Delete(root);

    if (have && s_work) {
        /* Never block the BLE stack on a full queue: drop the request instead.
         * The app's wizard is driven by a person who can press the button
         * again, and a stalled host loses the connection entirely. */
        if (xQueueSend(s_work, &job, 0) != pdTRUE) {
            ESP_LOGW(TAG, "busy — command dropped");
        }
    }
    memset(&job, 0, sizeof(job));
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt,
                       void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > 512) {
            return 0;
        }
        char *buf = malloc((size_t)len + 1);
        if (!buf) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &got) == 0) {
            buf[got] = '\0';
            handle_command(buf, got);
        }
        /* A WiFi password passed through here. */
        memset(buf, 0, (size_t)len + 1);
        free(buf);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* The app writes with response, so both flags. */
                .uuid = &nus_rx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            }, {
                .uuid = &nus_tx_uuid.u,
                .access_cb = gatt_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            }, {
                0,
            },
        },
    },
    { 0 },
};

/* ── GAP ─────────────────────────────────────────────────────────────────── */

static int on_gap(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEV_NAME;
    fields.name_len = strlen(DEV_NAME);
    fields.name_is_complete = 1;
    /* The name and nothing else: the app matches on it, and a 128-bit service
     * UUID would crowd the 31-byte advertisement for no gain. It goes in the
     * scan response instead, where other BLE tools can still find it. */
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGE(TAG, "could not set advertising fields");
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = (ble_uuid128_t *)&nus_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof(adv));
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, on_gap, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed to start: %d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as \"%s\"", DEV_NAME);
    }
}

static int on_gap(struct ble_gap_event *ev, void *arg)
{
    (void)arg;

    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected");
        } else {
            advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected (reason %d)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        /* Straight back to advertising. The wizard is used again whenever the
         * network changes, and a panel that has to be power-cycled to be
         * re-provisioned is a panel somebody will take off the wall. */
        advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE stack reset, reason %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();              /* returns only at nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Bring-up, on its own task because the first call blocks.
 *
 * The C6 takes roughly two seconds to come out of reset and negotiate SDIO, and
 * the BT controller does not exist until it has. Calling straight into
 * esp_hosted_bt_controller_init() from app_main lost that race and reported "no
 * BT controller on the C6" against a C6 that was simply still booting —
 * esp_hosted_connect_to_slave() is what waits for it. Doing that wait on a task
 * rather than in app_main keeps it off the path that starts the control link:
 * the tub comes first, as everywhere else here. */
static void bringup_task(void *arg)
{
    (void)arg;

    /* net_link owns the transport and is already bringing it up; waiting is the
     * whole job here. Calling esp_hosted_connect_to_slave() from this task as
     * well raced net_link doing the same and broke the SDIO card init outright,
     * which reads as a hardware fault and is not one. */
    if (!net_link_wait_hosted(20000)) {
        ESP_LOGE(TAG, "the C6 never came up — provisioning unavailable");
        vTaskDelete(NULL);
        return;
    }

    /* The P4 has no radio of its own. The controller lives on the C6 and is
     * reached over the same internal SDIO bus that carries WiFi — the boot log
     * calls it "HCI over SDIO". CONFIG_BT_CONTROLLER_DISABLED is set for that
     * reason: there is no local controller to build.
     *
     * These two are best-effort, and esp_hosted's own example only warns on
     * them for good reason: they are RPCs to the slave, and Guition's stock
     * image reports itself as 2.3.0 against a 2.11.0 host, so it predates them.
     * The VHCI transport announces itself up ("Host BT Support: Enabled") before
     * either call is made, and the controller on the C6 is already running —
     * what these do is ask a newer slave to start one. Treating a refusal as
     * fatal cost a bring-up here: the log said "no BT controller on the C6"
     * about a C6 whose controller was working. */
    esp_err_t rc_init = esp_hosted_bt_controller_init();
    if (rc_init != ESP_OK) {
        ESP_LOGW(TAG, "slave declined bt_controller_init (%s) — continuing, the "
                      "stock image starts its own", esp_err_to_name(rc_init));
    }
    esp_err_t rc_en = esp_hosted_bt_controller_enable();
    if (rc_en != ESP_OK) {
        ESP_LOGW(TAG, "slave declined bt_controller_enable (%s) — continuing",
                 esp_err_to_name(rc_en));
    }

    s_work = xQueueCreate(4, sizeof(job_t));
    if (!s_work) {
        ESP_LOGE(TAG, "out of memory");
        vTaskDelete(NULL);
        return;
    }

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        vTaskDelete(NULL);
        return;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "could not register the UART service: %d", rc);
        vTaskDelete(NULL);
        return;
    }

    ble_svc_gap_device_name_set(DEV_NAME);

    /* Small stack: it parses short JSON and calls into the WiFi driver, and the
     * scan results are heap. */
    xTaskCreate(worker, "ble_prov", 4096, NULL, 4, NULL);
    nimble_port_freertos_init(host_task);

    vTaskDelete(NULL);
}

void ble_prov_start(void)
{
    xTaskCreate(bringup_task, "ble_up", 4096, NULL, 4, NULL);
}

#else  /* !CONFIG_SPA_HMI_BLE_PROV */

void ble_prov_start(void)      { }
bool ble_prov_connected(void)  { return false; }

#endif /* CONFIG_SPA_HMI_BLE_PROV */
