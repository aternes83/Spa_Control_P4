#include "device_id.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

static const char *TAG = "device_id";

static char s_id[24];

const char *device_id(void)
{
    if (s_id[0]) {
        return s_id;
    }

#ifdef CONFIG_SPA_HMI_MQTT_ID
    /* A compiled-in id is the same on every board built with it, so two panels
     * flashed from one build would answer each other's app. Bench use only. */
    if (CONFIG_SPA_HMI_MQTT_ID[0] != '\0') {
        snprintf(s_id, sizeof(s_id), "%s", CONFIG_SPA_HMI_MQTT_ID);
        ESP_LOGW(TAG, "device id \"%s\" is compiled in, not this board's own", s_id);
        return s_id;
    }
#endif

    /* The base MAC, not a derived interface address: the P4 has no radio of its
     * own, and the base is what esptool prints, so the id on the label can be
     * read off the board before it has ever booted. */
    uint8_t m[6];
    if (esp_efuse_mac_get_default(m) != ESP_OK) {
        /* Not expected on a board with programmed eFuses. Fail loudly rather
         * than fall back to a constant every such board would share. */
        ESP_LOGE(TAG, "no factory MAC; this board has no unique id");
        snprintf(s_id, sizeof(s_id), "spa-unknown");
        return s_id;
    }
    snprintf(s_id, sizeof(s_id), "spa-%02x%02x%02x%02x%02x%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    ESP_LOGI(TAG, "device id %s", s_id);
    return s_id;
}
