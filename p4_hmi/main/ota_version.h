/*
 * Firmware version arithmetic and hex parsing — the part of OTA worth testing
 * on a host, so it is free of ESP-IDF exactly like ui_model.c and spa_state.c.
 * Covered by tools/test_ota_version.c.
 *
 * A version is a literal prefix followed by up to four dot-separated numbers:
 * "p4-1.0", "p4-1.12.3". The prefix is what stops an image built for a
 * different board being accepted as an upgrade because its numbers happen to be
 * bigger, so it is compared literally and a mismatch is never an upgrade.
 */
#ifndef OTA_VERSION_H
#define OTA_VERSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_VER_PARTS 4

typedef struct {
    char     prefix[24];           /* everything before the trailing numbers */
    uint16_t part[OTA_VER_PARTS];
    int      parts;                /* how many were present, 1..4 */
    bool     ok;
} ota_version_t;

/* Splits "p4-1.12" into prefix "p4-" and {1, 12}. ok is false for anything with
 * no trailing number at all, an over-long prefix, a part above 65535, or an
 * empty part ("1..2", "1."). */
ota_version_t ota_version_parse(const char *s);

/* Strictly newer, and from the same line of firmware. False when either side is
 * unparseable, when the prefixes differ, or when the candidate is equal or
 * older — a downgrade is refused rather than silently applied, because the
 * usual cause is a stale manifest rather than an intent to go back. */
bool ota_version_is_upgrade(const char *running, const char *candidate);

/* Exactly 2*n lowercase-or-uppercase hex digits and nothing else. Returns false
 * without touching out on anything shorter, longer or non-hex — a SHA-256 that
 * parses loosely is worse than none, since it would be compared against
 * whatever happened to land in the buffer. */
bool ota_hex_to_bytes(const char *hex, uint8_t *out, size_t n);

#endif /* OTA_VERSION_H */
