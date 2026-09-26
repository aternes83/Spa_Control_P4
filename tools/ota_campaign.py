#!/usr/bin/env python3
"""
A mock OTA campaign, for testing the panel's updater against things that go
wrong as well as things that go right.

    python3 tools/ota_campaign.py --image p4_hmi/build/spa_control_p4_hmi.bin \\
                                  --version p4-1.1

It serves two things on the LAN:

    /manifest.json   what the panel polls        (docs/OTA.md)
    /firmware.bin    the image the manifest points at

and prints the manifest URL to put in `idf.py menuconfig -> Spa HMI -> Manifest
URL`. Then it logs every request, so you can watch the panel find the update,
be told to take it, and pull it down.

The interesting part is `--fault`. A campaign that only ever serves a good image
proves the happy path and nothing else, and the happy path is not what bricks a
panel on a wall:

    none         serve it properly
    truncate     close the connection at --fault-at% of the image
    corrupt      flip a byte in the middle, so the SHA-256 will not match
    badsha       serve a good image under a manifest with the wrong hash
    downgrade    offer a version older than the panel is running
    tiny         serve a few hundred bytes with a 200, like an error page
    slow         serve it correctly but very slowly, to sit in the read timeout
    500          fail the image request after the manifest promised it

In every one of those the panel must end up still running the old firmware, and
say why in `ota_state`. That is the test.

Nothing here writes to the device. The panel only acts on an `ota_apply` you
send over MQTT, so serving a campaign is safe on its own.
"""
import argparse
import hashlib
import http.server
import json
import os
import socket
import sys
import threading
import time


def lan_ip():
    """The address the panel can actually reach, not 127.0.0.1. No traffic is
    sent — connect() on a UDP socket only picks a route."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))     # TEST-NET-1, guaranteed unroutable
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


class Campaign:
    def __init__(self, image, version, fault, fault_at, slow_bps):
        self.raw = open(image, "rb").read()
        self.version = version
        self.fault = fault
        self.fault_at = fault_at
        self.slow_bps = slow_bps
        self.hits = []

    # ── what goes on the wire ────────────────────────────────────────────────
    def body(self):
        """The bytes the image endpoint will actually serve."""
        if self.fault == "corrupt":
            b = bytearray(self.raw)
            mid = len(b) // 2
            b[mid] ^= 0xFF
            return bytes(b)
        if self.fault == "tiny":
            return b"<html><body>404 not found</body></html>\n"
        return self.raw

    def manifest(self):
        """The manifest always promises the *pristine* image. That is the whole
        point of it: it is the statement the panel checks the delivery against,
        so a fault must never be allowed to describe itself. `corrupt` in
        particular has to keep the honest hash, or the damage would be signed
        for and the panel would be right to install it."""
        sha = hashlib.sha256(self.raw).hexdigest()
        version = self.version
        size = len(self.raw)

        if self.fault == "badsha":
            # A good image under a hash that matches nothing. Tests that the
            # comparison happens at all, from the other side to `corrupt`.
            sha = "0" * 64
        if self.fault == "downgrade":
            version = self.downgrade_of(self.version)
        if self.fault == "tiny":
            # An error page served with a 200. The size gives it away before a
            # byte is written, which is the check that should catch it.
            size = len(self.body())

        return {
            "version": version,
            "url": self.image_url,
            "sha256": sha,
            "size": size,
            "notes": "mock campaign, fault=%s" % self.fault,
        }

    @staticmethod
    def downgrade_of(version):
        """p4-1.1 -> p4-0.1, so the panel sees something genuinely older."""
        head = version.rstrip("0123456789.")
        nums = version[len(head):].split(".")
        nums[0] = str(max(0, int(nums[0]) - 1)) if nums[0].isdigit() else "0"
        return head + ".".join(nums)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    campaign = None

    def log_message(self, fmt, *args):
        print("  %s  %s" % (time.strftime("%H:%M:%S"), fmt % args), flush=True)

    def do_GET(self):
        c = self.campaign
        c.hits.append((time.time(), self.path))

        if self.path.startswith("/manifest"):
            payload = json.dumps(c.manifest()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            print("    -> offered %s" % c.manifest()["version"], flush=True)
            return

        if self.path.startswith("/firmware"):
            if c.fault == "500":
                self.send_response(500)
                self.send_header("Content-Length", "0")
                self.end_headers()
                print("    -> 500, as asked", flush=True)
                return
            self.serve_image(c)
            return

        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def serve_image(self, c):
        body = c.body()
        total = len(body)
        # Content-Length always describes the whole image, even when we are
        # about to stop early: a truncation the client can detect from the
        # headers alone is not the failure worth testing.
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(total))
        self.end_headers()

        stop = total
        if c.fault == "truncate":
            stop = max(1, total * c.fault_at // 100)
            print("    -> will cut the connection at %d/%d bytes (%d%%)"
                  % (stop, total, c.fault_at), flush=True)

        sent = 0
        chunk = 8192
        last_note = 0
        while sent < stop:
            n = min(chunk, stop - sent)
            try:
                self.wfile.write(body[sent:sent + n])
            except (BrokenPipeError, ConnectionResetError):
                print("    -> client went away at %d bytes" % sent, flush=True)
                return
            sent += n
            if c.slow_bps:
                time.sleep(n / float(c.slow_bps))
            if sent - last_note >= total // 10:
                last_note = sent
                print("    -> %d%%" % (sent * 100 // total), flush=True)

        if stop < total:
            # Close hard, mid-body. This is what a server dying looks like and
            # the panel must treat it as a failure, not as a short image.
            self.close_connection = True
            try:
                self.wfile.flush()
                self.connection.close()
            except Exception:
                pass
            print("    -> connection cut", flush=True)
        else:
            print("    -> served %d bytes" % sent, flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", required=True, help="firmware .bin to serve")
    ap.add_argument("--version", required=True,
                    help="version to offer, e.g. p4-1.1 (must beat the panel's)")
    ap.add_argument("--port", type=int, default=8070)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--fault", default="none",
                    choices=["none", "truncate", "corrupt", "badsha",
                             "downgrade", "tiny", "slow", "500"])
    ap.add_argument("--fault-at", type=int, default=40,
                    help="percent of the image to send before cutting (truncate)")
    ap.add_argument("--slow-bps", type=int, default=0,
                    help="bytes per second (slow); 0 is unthrottled")
    args = ap.parse_args()

    if not os.path.exists(args.image):
        sys.exit("no such image: %s (build it with idf.py build)" % args.image)
    if args.fault == "slow" and not args.slow_bps:
        args.slow_bps = 2000

    c = Campaign(args.image, args.version, args.fault, args.fault_at, args.slow_bps)
    ip = lan_ip()
    c.image_url = "http://%s:%d/firmware.bin" % (ip, args.port)
    manifest_url = "http://%s:%d/manifest.json" % (ip, args.port)

    Handler.campaign = c
    srv = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)

    m = c.manifest()
    print("mock OTA campaign")
    print("  image      %s (%d bytes)" % (args.image, len(c.raw)))
    print("  offering   %s" % m["version"])
    print("  sha256     %s" % m["sha256"])
    print("  fault      %s%s" % (args.fault,
                                 "" if args.fault == "none" else "  <-- expect the panel to REFUSE"))
    print()
    print("  manifest   %s" % manifest_url)
    print("             put that in: idf.py menuconfig -> Spa HMI -> Manifest URL")
    print()
    print("  then, to install it, publish to the panel's command topic:")
    print('             {"ota_apply": "%s"}' % m["version"])
    print()
    print("waiting for the panel (ctrl-c to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped after %d request(s)" % len(c.hits))


if __name__ == "__main__":
    main()
