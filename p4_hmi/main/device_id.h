/*
 * Which tub this panel is, as far as the app and the broker are concerned.
 *
 * The id scopes every topic (spa/<id>/status and so on) and is handed to the app
 * over BLE in the broker settings; the app pairs to whatever it is told and
 * subscribes to that tub alone. So it has to differ between every board sold,
 * without a per-board build: it is derived from the factory MAC burned into the
 * chip's eFuses, which also means a reflash or a full erase cannot change it.
 */
#ifndef DEVICE_ID_H
#define DEVICE_ID_H

/* "spa-" and the base MAC in lowercase hex, e.g. "spa-80f1b2d36640", unless
 * CONFIG_SPA_HMI_MQTT_ID overrides it for a bench board. Never empty. */
const char *device_id(void);

#endif /* DEVICE_ID_H */
