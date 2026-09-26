/*
 * OTA version arithmetic. Host-compiled, no ESP-IDF:
 *
 *     cc -std=c11 -I p4_hmi/main -o /tmp/t tools/test_ota_version.c \
 *        p4_hmi/main/ota_version.c && /tmp/t
 *
 * (tools/run_tests.sh does this for you.)
 *
 * This is the half of OTA that decides whether a downloaded image is allowed
 * anywhere near the flash, and every interesting case is a refusal: a downgrade,
 * an image from a different board, a truncated hash. Getting a refusal wrong is
 * how a panel on a wall ends up running firmware nobody meant to send it.
 */
#include "ota_version.h"

#include <stdio.h>
#include <string.h>

static int failed = 0;

static void check(const char *name, bool cond)
{
    if (cond) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s\n", name);
        failed++;
    }
}

int main(void)
{
    printf("parsing\n");
    ota_version_t v = ota_version_parse("p4-1.0");
    check("prefix and parts split", v.ok && strcmp(v.prefix, "p4-") == 0 &&
          v.parts == 2 && v.part[0] == 1 && v.part[1] == 0);
    v = ota_version_parse("p4-1.12.3.4");
    check("four parts are kept", v.ok && v.parts == 4 && v.part[1] == 12 &&
          v.part[3] == 4);
    v = ota_version_parse("2.0");
    check("a bare number needs no prefix", v.ok && v.prefix[0] == '\0' &&
          v.part[0] == 2);

    printf("what will not parse\n");
    check("no trailing number", !ota_version_parse("p4-rc").ok);
    check("empty string", !ota_version_parse("").ok);
    check("a doubled dot", !ota_version_parse("p4-1..2").ok);
    check("a trailing dot", !ota_version_parse("p4-1.").ok);
    check("a part that will not fit", !ota_version_parse("p4-65536").ok);
    check("more parts than are compared", !ota_version_parse("p4-1.2.3.4.5").ok);
    /* A prefix longer than the struct can hold is refused rather than cut down
     * to something that would then compare equal to a different board's. */
    check("an over-long prefix", !ota_version_parse(
        "an-extremely-long-board-name-that-will-not-fit-1.0").ok);

    printf("upgrades\n");
    check("a later minor", ota_version_is_upgrade("p4-1.0", "p4-1.1"));
    check("a later major", ota_version_is_upgrade("p4-1.9", "p4-2.0"));
    check("10 really is above 9", ota_version_is_upgrade("p4-1.9", "p4-1.10"));
    check("a longer version with more detail",
          ota_version_is_upgrade("p4-1.0", "p4-1.0.1"));

    printf("refusals — the cases that matter\n");
    check("the same version is not an upgrade",
          !ota_version_is_upgrade("p4-1.0", "p4-1.0"));
    check("a trailing zero is not an upgrade",
          !ota_version_is_upgrade("p4-1", "p4-1.0"));
    check("a downgrade is refused", !ota_version_is_upgrade("p4-1.1", "p4-1.0"));
    check("a big downgrade is still refused",
          !ota_version_is_upgrade("p4-2.0", "p4-1.99"));
    /* The one that would brick a panel: an image for the control board, or for
     * a different product, whose numbers merely happen to be larger. */
    check("a different board is never an upgrade",
          !ota_version_is_upgrade("p4-1.0", "s3-9.9"));
    check("nor is a bare number against a prefixed one",
          !ota_version_is_upgrade("p4-1.0", "9.9"));
    check("unparseable candidate", !ota_version_is_upgrade("p4-1.0", "latest"));
    check("unparseable running version", !ota_version_is_upgrade("dev", "p4-1.1"));

    printf("hash parsing\n");
    uint8_t buf[4];
    memset(buf, 0xAA, sizeof(buf));
    check("exact length lowercase", ota_hex_to_bytes("0a1b2c3d", buf, 4) &&
          buf[0] == 0x0a && buf[3] == 0x3d);
    check("uppercase too", ota_hex_to_bytes("0A1B2C3D", buf, 4) && buf[1] == 0x1b);
    memset(buf, 0xAA, sizeof(buf));
    check("one digit short is refused", !ota_hex_to_bytes("0a1b2c3", buf, 4));
    check("and the buffer is untouched by a refusal", buf[0] == 0xAA);
    check("too long is refused", !ota_hex_to_bytes("0a1b2c3d4e", buf, 4));
    check("non-hex is refused", !ota_hex_to_bytes("0a1b2c3g", buf, 4));
    check("empty is refused", !ota_hex_to_bytes("", buf, 4));

    /* A real SHA-256 round trip, since 32 bytes is what the manifest carries. */
    uint8_t sha[32];
    const char *h = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    check("a full sha256 parses", ota_hex_to_bytes(h, sha, 32) &&
          sha[0] == 0xe3 && sha[31] == 0x55);

    printf("\n");
    if (failed) {
        printf("%d OTA version tests FAILED\n", failed);
        return 1;
    }
    printf("all OTA version tests passed\n");
    return 0;
}
