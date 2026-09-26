/*
 * Over-the-air update for the HMI node.
 *
 * The shape is deliberately conservative, because the failure this guards
 * against is a panel on a wall that will not boot:
 *
 *   * Nothing is ever applied that the panel was not told to apply by name. The
 *     poll only *finds* a version and reports it as `ota_avail`; installing it
 *     takes an `ota_apply` carrying that same version (docs/MQTT.md). Broker
 *     access is therefore not by itself enough to put arbitrary firmware on the
 *     board — it can only ask for what the manifest the panel already trusts is
 *     offering.
 *   * A downgrade or an image from a different board is refused, in
 *     ota_version.c, which is host-tested.
 *   * The image is written to the slot that is NOT running, its SHA-256 is
 *     checked against the manifest before the boot partition is moved, and the
 *     bootloader rolls back if the new image never reports itself healthy
 *     (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 *   * Any failure leaves the running firmware untouched and the tub running.
 *     There is no state in which the panel is half-updated.
 *
 * The plant is not gated on any of this. The S3 owns the tub and keeps its
 * thermostat, interlocks and freeze protection whatever this file is doing; an
 * update reboot simply drops the link for a few seconds, which the S3 already
 * treats as "stop honouring requests" and nothing more.
 */
#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>

/* Starts the manifest poll, and marks a freshly-booted OTA image as pending so
 * the bootloader can still take it back. Safe to call with OTA disabled. */
void ota_start(void);

/* "This build works." Cancels the rollback armed at boot. Called once the panel
 * has actually run for a while — see the note in ota.c on what counts. */
void ota_report_healthy(void);

/* Install the named version, which must be the one the poll is currently
 * offering. Returns false and leaves everything alone if it is not, if an
 * update is already running, or if OTA is not configured. Safe from any task. */
bool ota_apply_version(const char *version);

/* The version the manifest is offering, or NULL when there is nothing newer.
 * The pointer is owned here and stays valid; copy it under no assumption of
 * atomicity beyond a single read. */
const char *ota_available_version(void);

/* One short line for the `ota_state` status field: "idle", "downloading 42%",
 * "verifying", "rebooting", or "failed: <why>". Always NUL-terminates. */
void ota_state_str(char *buf, size_t n);

/* What this build calls itself — CONFIG_SPA_HMI_MQTT_FW. */
const char *ota_running_version(void);

/* True when OTA is compiled in and a manifest URL is configured. */
bool ota_enabled(void);

#endif /* OTA_H */
