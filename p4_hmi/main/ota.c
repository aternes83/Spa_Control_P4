#include "ota.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#if CONFIG_SPA_HMI_OTA

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "net_link.h"
#include "ota_version.h"

static const char *TAG = "ota";

#define MANIFEST_MAX   1024
#define URL_MAX        256
#define VER_MAX        32
#define REASON_MAX     64
#define CHUNK          2048
/* An image smaller than this is not a firmware image, whatever the manifest
 * claims; the app is about 1.9 MB. Catches a server returning an error page
 * with a 200, which is the most common way a mock campaign lies. */
#define MIN_IMAGE      (256 * 1024)

typedef enum {
    ST_IDLE = 0, ST_CHECKING, ST_DOWNLOADING, ST_VERIFYING, ST_REBOOTING, ST_FAILED,
} ota_st_t;

static SemaphoreHandle_t s_lock;
static ota_st_t  s_state;
static int       s_pct;
static char      s_reason[REASON_MAX];
static char      s_avail[VER_MAX];        /* version the manifest offers */
static char      s_avail_url[URL_MAX];
static char      s_avail_sha[65];
static int       s_avail_size;
static char      s_want[VER_MAX];         /* version ota_apply asked for */
static bool      s_busy;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void set_state(ota_st_t st, const char *reason)
{
    lock();
    s_state = st;
    if (reason) {
        snprintf(s_reason, sizeof(s_reason), "%s", reason);
    } else if (st != ST_FAILED) {
        s_reason[0] = '\0';
    }
    unlock();
}

static void fail(const char *why)
{
    ESP_LOGE(TAG, "update failed: %s", why);
    set_state(ST_FAILED, why);
}

const char *ota_running_version(void) { return CONFIG_SPA_HMI_MQTT_FW; }

bool ota_enabled(void) { return CONFIG_SPA_HMI_OTA_MANIFEST[0] != '\0'; }

const char *ota_available_version(void)
{
    return s_avail[0] ? s_avail : NULL;
}

void ota_state_str(char *buf, size_t n)
{
    if (!n) {
        return;
    }
    lock();
    switch (s_state) {
    case ST_CHECKING:    snprintf(buf, n, "checking");              break;
    case ST_DOWNLOADING: snprintf(buf, n, "downloading %d%%", s_pct); break;
    case ST_VERIFYING:   snprintf(buf, n, "verifying");             break;
    case ST_REBOOTING:   snprintf(buf, n, "rebooting");             break;
    case ST_FAILED:      snprintf(buf, n, "failed: %s", s_reason);  break;
    case ST_IDLE:
    default:             snprintf(buf, n, "idle");                  break;
    }
    unlock();
}

/* ── Rollback ────────────────────────────────────────────────────────────────
 * A freshly-flashed image boots PENDING_VERIFY and the bootloader will take it
 * back on the next reset unless it says it is well. "Well" here means the panel
 * got through display init and link init and then ran its loop for a while —
 * which is what catches the failure that actually matters, an image that
 * crashes or panics early. It deliberately does NOT require the S3 to answer:
 * a panel whose controller is switched off is not a broken panel, and rolling
 * back over that would make the tub's power switch a firmware downgrade. */
void ota_report_healthy(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return;
    }
    if (st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;                          /* nothing armed; nothing to cancel */
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "this image is marked good; rollback cancelled");
    } else {
        ESP_LOGE(TAG, "could not mark the image valid — it will roll back");
    }
}

/* ── HTTP ────────────────────────────────────────────────────────────────── */

static esp_http_client_handle_t open_url(const char *url, int *status, int64_t *len)
{
    const bool tls = strncmp(url, "https://", 8) == 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = false,
        /* A campaign server behind a redirect is normal; following it silently
         * is not, so cap it and let a loop fail rather than hang. */
        .max_redirection_count = 3,
        .crt_bundle_attach = tls ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    *len = esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    return c;
}

/* ── The manifest ────────────────────────────────────────────────────────── */

static bool fetch_manifest(void)
{
    int status = 0;
    int64_t len = 0;
    esp_http_client_handle_t c = open_url(CONFIG_SPA_HMI_OTA_MANIFEST, &status, &len);
    if (!c) {
        ESP_LOGW(TAG, "manifest unreachable");
        return false;
    }
    bool ok = false;
    char body[MANIFEST_MAX];
    int n = 0;
    if (status != 200) {
        ESP_LOGW(TAG, "manifest HTTP %d", status);
        goto done;
    }
    n = esp_http_client_read_response(c, body, sizeof(body) - 1);
    if (n <= 0) {
        ESP_LOGW(TAG, "empty manifest");
        goto done;
    }
    body[n] = '\0';

    cJSON *root = cJSON_ParseWithLength(body, (size_t)n);
    if (!root) {
        ESP_LOGW(TAG, "manifest is not JSON");
        goto done;
    }
    const cJSON *jv  = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *ju  = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *js  = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *jz  = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsString(jv) || !cJSON_IsString(ju) || !cJSON_IsString(js) ||
        !cJSON_IsNumber(jz)) {
        ESP_LOGW(TAG, "manifest is missing version/url/sha256/size");
        cJSON_Delete(root);
        goto done;
    }

    /* Validate before storing, so a bad manifest cannot displace a good one. */
    uint8_t probe[32];
    if (!ota_hex_to_bytes(js->valuestring, probe, sizeof(probe))) {
        ESP_LOGW(TAG, "manifest sha256 is not 64 hex digits");
        cJSON_Delete(root);
        goto done;
    }
    if (strlen(jv->valuestring) >= VER_MAX || strlen(ju->valuestring) >= URL_MAX) {
        ESP_LOGW(TAG, "manifest version or url too long");
        cJSON_Delete(root);
        goto done;
    }
    if (!ota_version_is_upgrade(ota_running_version(), jv->valuestring)) {
        /* Not an error — the usual case is that the panel is up to date. */
        ESP_LOGD(TAG, "offered %s, running %s: nothing to do",
                 jv->valuestring, ota_running_version());
        lock(); s_avail[0] = '\0'; unlock();
        cJSON_Delete(root);
        ok = true;
        goto done;
    }

    lock();
    snprintf(s_avail,     sizeof(s_avail),     "%s", jv->valuestring);
    snprintf(s_avail_url, sizeof(s_avail_url), "%s", ju->valuestring);
    snprintf(s_avail_sha, sizeof(s_avail_sha), "%s", js->valuestring);
    s_avail_size = jz->valueint;
    unlock();
    ESP_LOGI(TAG, "update available: %s (%d bytes)", jv->valuestring, jz->valueint);
    cJSON_Delete(root);
    ok = true;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

/* ── The download ────────────────────────────────────────────────────────── */

static void download(const char *url, const char *sha_hex, int want_size)
{
    uint8_t want[32];
    if (!ota_hex_to_bytes(sha_hex, want, sizeof(want))) {
        fail("bad sha256 in manifest");
        return;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        fail("no free OTA slot");
        return;
    }
    ESP_LOGI(TAG, "writing %s to %s at 0x%lx", url, target->label,
             (unsigned long)target->address);

    int status = 0;
    int64_t content_len = 0;
    esp_http_client_handle_t c = open_url(url, &status, &content_len);
    if (!c) {
        fail("cannot reach the image");
        return;
    }
    if (status != 200) {
        char why[REASON_MAX];
        snprintf(why, sizeof(why), "image HTTP %d", status);
        esp_http_client_close(c); esp_http_client_cleanup(c);
        fail(why);
        return;
    }
    /* A manifest whose size disagrees with the server is a campaign that has
     * been edited in one place only. Refuse before writing a byte. */
    if (content_len > 0 && want_size > 0 && content_len != (int64_t)want_size) {
        esp_http_client_close(c); esp_http_client_cleanup(c);
        fail("size differs from the manifest");
        return;
    }
    if (want_size > 0 && want_size < MIN_IMAGE) {
        esp_http_client_close(c); esp_http_client_cleanup(c);
        fail("image is too small to be firmware");
        return;
    }

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(target, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        esp_http_client_close(c); esp_http_client_cleanup(c);
        fail("could not erase the target slot");
        return;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    static uint8_t buf[CHUNK];
    int total = 0;
    const char *why = NULL;
    set_state(ST_DOWNLOADING, NULL);

    for (;;) {
        int r = esp_http_client_read(c, (char *)buf, sizeof(buf));
        if (r < 0) {
            why = "connection dropped mid-transfer";
            break;
        }
        if (r == 0) {
            /* A clean end only counts as a clean end if everything arrived; a
             * server that closes early looks exactly like this otherwise. */
            if (esp_http_client_is_complete_data_received(c)) {
                break;
            }
            why = "transfer ended early";
            break;
        }
        if (esp_ota_write(h, buf, (size_t)r) != ESP_OK) {
            why = "flash write failed";
            break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)r);
        total += r;
        if (want_size > 0) {
            if (total > want_size) {
                why = "image is longer than the manifest says";
                break;
            }
            lock();
            s_pct = (int)((int64_t)total * 100 / want_size);
            unlock();
        }
    }

    uint8_t got[32];
    mbedtls_sha256_finish(&sha, got);
    mbedtls_sha256_free(&sha);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (!why && want_size > 0 && total != want_size) {
        why = "image is shorter than the manifest says";
    }
    if (!why && total < MIN_IMAGE) {
        why = "image is too small to be firmware";
    }
    if (!why) {
        set_state(ST_VERIFYING, NULL);
        if (memcmp(got, want, sizeof(got)) != 0) {
            why = "sha256 mismatch";
        }
    }
    if (why) {
        /* Abort rather than end: the slot keeps whatever esp_ota_abort leaves
         * it as, which is explicitly not bootable, and the running image is
         * untouched either way. */
        esp_ota_abort(h);
        fail(why);
        return;
    }

    /* esp_ota_end runs IDF's own checks on the image header — magic, chip id,
     * secure-boot signature where configured — on top of our hash. Both matter:
     * the hash proves it is the file the manifest meant, this proves the file
     * is firmware this chip can run. */
    esp_err_t err = esp_ota_end(h);
    if (err != ESP_OK) {
        fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation"
                                                : "could not finalise the image");
        return;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        fail("could not set the boot partition");
        return;
    }

    ESP_LOGW(TAG, "update written (%d bytes) — rebooting into %s", total, target->label);
    set_state(ST_REBOOTING, NULL);
    /* Let the last status publish get out, then go. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ── The task ────────────────────────────────────────────────────────────── */

static void ota_task(void *arg)
{
    (void)arg;
    while (!net_link_wait_ip(30000)) {
        ESP_LOGD(TAG, "waiting for an address before checking for updates");
    }

    uint32_t poll_ms = (uint32_t)CONFIG_SPA_HMI_OTA_POLL_MIN * 60u * 1000u;
    TickType_t next = xTaskGetTickCount();

    for (;;) {
        /* An apply request beats the poll: the user is standing there. */
        char want[VER_MAX];
        lock();
        snprintf(want, sizeof(want), "%s", s_want);
        s_want[0] = '\0';
        unlock();

        if (want[0]) {
            char url[URL_MAX], sha[65];
            int size;
            lock();
            snprintf(url, sizeof(url), "%s", s_avail_url);
            snprintf(sha, sizeof(sha), "%s", s_avail_sha);
            size = s_avail_size;
            bool matches = (strcmp(want, s_avail) == 0);
            unlock();

            if (!matches) {
                /* The offer moved, or never existed. Refusing is the point:
                 * this is what stops an ota_apply naming something the panel
                 * has not checked against its own manifest. */
                fail("that version is not the one on offer");
            } else {
                download(url, sha, size);
                /* download() only returns on failure; success reboots. */
            }
            lock(); s_busy = false; unlock();
        } else if ((int32_t)(xTaskGetTickCount() - next) >= 0) {
            next = xTaskGetTickCount() + pdMS_TO_TICKS(poll_ms);
            lock();
            bool idle = (s_state == ST_IDLE || s_state == ST_FAILED);
            unlock();
            if (idle) {
                set_state(ST_CHECKING, NULL);
                bool ok = fetch_manifest();
                set_state(ok ? ST_IDLE : ST_FAILED, ok ? NULL : "manifest unreachable");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

bool ota_apply_version(const char *version)
{
    if (!ota_enabled() || !version || !*version) {
        return false;
    }
    bool accepted = false;
    lock();
    if (!s_busy && s_avail[0] && strcmp(version, s_avail) == 0) {
        snprintf(s_want, sizeof(s_want), "%s", version);
        s_busy = true;
        accepted = true;
    }
    unlock();
    if (accepted) {
        ESP_LOGW(TAG, "ota_apply accepted for %s", version);
    } else {
        ESP_LOGW(TAG, "ota_apply refused for %s", version);
    }
    return accepted;
}

void ota_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "no mutex — OTA disabled for this boot");
        return;
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running %s from %s at 0x%lx", ota_running_version(),
             run ? run->label : "?", run ? (unsigned long)run->address : 0UL);

    if (!ota_enabled()) {
        ESP_LOGI(TAG, "no manifest configured — updates are off");
        return;
    }
    ESP_LOGI(TAG, "manifest %s, every %d min",
             CONFIG_SPA_HMI_OTA_MANIFEST, CONFIG_SPA_HMI_OTA_POLL_MIN);
    /* Below the link and the UI. An update must never be the reason the tub
     * stops being drawn. */
    xTaskCreate(ota_task, "ota", 6144, NULL, 3, NULL);
}

#else  /* !CONFIG_SPA_HMI_OTA */

void ota_start(void) { }
void ota_report_healthy(void) { }
bool ota_apply_version(const char *v) { (void)v; return false; }
const char *ota_available_version(void) { return NULL; }
const char *ota_running_version(void) { return ""; }
bool ota_enabled(void) { return false; }
void ota_state_str(char *buf, size_t n) { if (n) snprintf(buf, n, "off"); }

#endif /* CONFIG_SPA_HMI_OTA */
