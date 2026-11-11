#!/usr/bin/env bash
# Build the current tree, flash it to a P4 panel, and prove it booted:
#
#     tools/flash_p4.sh [--erase] [port]        default /dev/cu.usbmodem83201
#
# --erase wipes the P4's whole flash first, including the stored timezone. WiFi
# survives it: the credentials are kept on the C6, which this does not touch.
#
# docs/BRINGUP.md is the procedure this belongs to. The port is never guessed by
# scanning: probing a port resets whatever is on it, and the other Espressif
# port on this bench may be the S3 running the tub.
#
# Needs the gitignored p4_hmi/sdkconfig.local, which carries the broker login.
. "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1 || {
    echo "FAIL: ESP-IDF not found at ~/esp/esp-idf" >&2
    exit 1
}
set -euo pipefail
cd "$(dirname "$0")/../p4_hmi"

WRITE=(write_flash)
if [ "${1:-}" = "--erase" ]; then
    WRITE+=(--erase-all)
    shift
fi
PORT="${1:-/dev/cu.usbmodem83201}"
ESPTOOL=(python -m esptool --chip esp32p4 -p "$PORT" -b 460800 --before usb_reset)

die() { echo "FAIL: $*" >&2; exit 1; }

# esptool can hang forever on a wedged USB port rather than report it.
limit() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }

[ -e "$PORT" ] || die "no such port $PORT"
[ -f sdkconfig.local ] || die "p4_hmi/sdkconfig.local is missing; the panel would have no broker login"

echo "── checking the board ──"
ID="$(limit 45 "${ESPTOOL[@]}" --after no_reset flash_id 2>&1)" || { echo "$ID"; die "no ESP32-P4 answered on $PORT"; }
echo "$ID" | grep -E '^(Chip is|MAC:|Detected flash size)'
# sdkconfig.defaults pins the pre-v3 silicon family, so the image takes v1.x only.
echo "$ID" | grep -q 'revision v1\.' || die "this firmware is built for P4 revision v1.x"
echo "$ID" | grep -q 'Detected flash size: 16MB' || die "partitions.csv needs 16MB of flash"

echo "── building ──"
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.net;sdkconfig.local" build | grep -E 'binary size|rror'
grep -Eq '^CONFIG_SPA_HMI_MQTT_URI="[^"]+"' sdkconfig || die "the build has no broker URI"
# A compiled-in id is shared by every board built with it, so two of them would
# answer each other's app. Each board must take its own from its MAC.
grep -Eq '^CONFIG_SPA_HMI_MQTT_ID="[^"]+"' sdkconfig \
    && die "CONFIG_SPA_HMI_MQTT_ID is set (look in sdkconfig.local); clear it so the board uses its own id"

echo "── flashing ──"
(cd build && limit 400 "${ESPTOOL[@]}" --after hard_reset "${WRITE[@]}" "@flash_args") | grep -E 'Hash of data|Wrote'

echo "── watching it boot ──"
python - "$PORT" <<'PY'
import re, sys, time
import serial

# The checkpoints of a healthy boot, in order. The control board is not among
# them: a panel on the bench has none, and says "waiting" indefinitely.
WANT = ["App version:", "device_id: device id ", "Display initialized",
        "Identified slave [esp32c6]", 'advertising as "SpaControl"']
BAD = re.compile(r"Guru Meditation|abort\(\)|the C6 never came up|Rebooting\.\.\.")

port, deadline, log, conn = sys.argv[1], time.time() + 40, "", None
while time.time() < deadline and not all(w in log for w in WANT):
    try:
        if conn is None:
            conn = serial.Serial()
            conn.port, conn.baudrate, conn.timeout = port, 115200, 0.2
            conn.dtr = conn.rts = False     # opening must not reset the board
            conn.open()
        log += conn.read(4096).decode("utf8", "replace")
    except (OSError, serial.SerialException):   # USB re-enumerating after reset
        conn = None
        time.sleep(0.1)
    if BAD.search(log):
        break

for line in log.splitlines():
    if "App version:" in line or "net: " in line:
        print(re.sub(r"\x1b\[[0-9;]*m", "", line))
missing = [w for w in WANT if w not in log]
crash = BAD.search(log)
if crash or missing:
    print("FAIL: %s" % (crash.group(0) if crash else "never saw: %s" % missing))
    sys.exit(1)
print("OK: booted, display up, C6 answering, advertising as SpaControl")
print("ID: %s" % re.search(r"device id (spa-[0-9a-f]{12})", log).group(1))
PY
