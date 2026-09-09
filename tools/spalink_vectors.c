/*
 * Test harness: exercises the C codec from stdin so tools/test_spalink.py can
 * compare it against the Python reference on identical input.
 *
 * Commands, one per line:
 *   C <hex>                       -> crc16 of the bytes
 *   E <msg_id> <hdr> <payload_hex> -> UART-framed hex ("ERR" if rejected)
 *   D <hex stream>                -> "<msg_id> <hdr> <payload_hex>" per frame
 *                                    recovered, then "bad_crc=<n> bad_len=<n>"
 */
#include "../link/spalink_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1] && n < cap) {
        if (s[0] == ' ') { s++; continue; }
        char b[3] = { s[0], s[1], 0 };
        out[n++] = (uint8_t)strtol(b, NULL, 16);
        s += 2;
    }
    return n;
}

static void puthex(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

int main(void)
{
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char cmd = line[0];
        char *rest = line + 2;

        if (cmd == 'C') {
            uint8_t buf[2048];
            size_t n = unhex(rest, buf, sizeof(buf));
            printf("%04x\n", spalink_crc16(buf, n));
        } else if (cmd == 'E') {
            unsigned id, hdr;
            char pay[4096] = {0};
            int got = sscanf(rest, "%x %x %4000s", &id, &hdr, pay);
            uint8_t p[64];
            size_t pn = (got >= 3) ? unhex(pay, p, sizeof(p)) : 0;
            uint8_t out[SPALINK_MAX_FRAME];
            size_t n = spalink_encode_uart((uint8_t)id, (uint8_t)hdr, p, pn, out);
            if (n == 0) printf("ERR\n");
            else { puthex(out, n); printf("\n"); }
        } else if (cmd == 'D') {
            uint8_t buf[4096];
            size_t n = unhex(rest, buf, sizeof(buf));
            spalink_decoder_t d;
            spalink_decoder_init(&d);
            spalink_msg_t m;
            for (size_t i = 0; i < n; i++) {
                if (spalink_decoder_feed(&d, buf[i], &m)) {
                    printf("%02x %02x ", m.msg_id, m.hdr);
                    puthex(m.payload, m.len);
                    printf("\n");
                }
            }
            printf("bad_crc=%u bad_len=%u\n", (unsigned)d.bad_crc, (unsigned)d.bad_len);
        }
        fflush(stdout);
    }
    return 0;
}
