#include "ota_version.h"

#include <string.h>

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

ota_version_t ota_version_parse(const char *s)
{
    ota_version_t v;
    memset(&v, 0, sizeof(v));
    if (!s || !*s) {
        return v;
    }

    /* The version is the trailing run of digits and dots. Walk back from the
     * end over that set, then require the character before it not to be one —
     * which it cannot be, by construction. */
    size_t len = strlen(s);
    size_t i = len;
    while (i > 0 && (is_digit(s[i - 1]) || s[i - 1] == '.')) {
        i--;
    }
    if (i == len) {
        return v;                      /* no number at the end at all */
    }
    if (i >= sizeof(v.prefix)) {
        return v;                      /* prefix we could not store is a refusal */
    }
    memcpy(v.prefix, s, i);
    v.prefix[i] = '\0';

    const char *p = s + i;
    int n = 0;
    while (*p) {
        if (!is_digit(*p)) {
            return (ota_version_t){ .ok = false };   /* leading or doubled dot */
        }
        uint32_t acc = 0;
        while (is_digit(*p)) {
            acc = acc * 10u + (uint32_t)(*p - '0');
            if (acc > 65535u) {
                return (ota_version_t){ .ok = false };
            }
            p++;
        }
        if (n >= OTA_VER_PARTS) {
            return (ota_version_t){ .ok = false };    /* more parts than we compare */
        }
        v.part[n++] = (uint16_t)acc;
        if (*p == '.') {
            p++;
            if (*p == '\0') {
                return (ota_version_t){ .ok = false }; /* trailing dot */
            }
        }
    }
    v.parts = n;
    v.ok = n > 0;
    return v;
}

bool ota_version_is_upgrade(const char *running, const char *candidate)
{
    ota_version_t a = ota_version_parse(running);
    ota_version_t b = ota_version_parse(candidate);
    if (!a.ok || !b.ok) {
        return false;
    }
    if (strcmp(a.prefix, b.prefix) != 0) {
        return false;
    }
    /* Missing parts read as zero, so "p4-1.1" beats "p4-1" and "p4-1.0" does
     * not beat "p4-1". */
    for (int i = 0; i < OTA_VER_PARTS; i++) {
        uint16_t x = a.part[i], y = b.part[i];
        if (y > x) return true;
        if (y < x) return false;
    }
    return false;                      /* identical: nothing to do */
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ota_hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    if (!hex || !out) {
        return false;
    }
    if (strlen(hex) != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(hex[i * 2]), lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
