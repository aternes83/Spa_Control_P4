# OTA — updating the panel over the air

The HMI node can replace its own firmware. The S3 cannot and deliberately does
not: it holds the plant, its filesystem is a handful of `.py` files, and a
control board that can be rewritten from a network is a control board that can be
rewritten by whoever reaches the network. Updating it stays a cable and a
`mpremote cp`.

## The shape of it

```
   campaign server                          panel
   ──────────────                           ─────
   /manifest.json  ──── every N minutes ───► is this newer than me?
                                             │  no  -> nothing, silently
                                             │  yes -> publish ota_avail
                                             ▼
   app / broker  ──── {"ota_apply":"p4-1.1"} ► is that the version I am offering?
                                             │  no  -> refused
                                             │  yes ▼
   /firmware.bin ──────── streamed ─────────► the slot that is NOT running
                                             ▼
                                          sha256 vs the manifest
                                             ▼
                                          esp_ota_end validates the header
                                             ▼
                                          set boot partition, reboot
                                             ▼
                                   30 s of running -> mark valid
                                   crash first     -> bootloader takes it back
```

## Four things stand between a bad image and a dead panel

**The version has to be a genuine upgrade, of this board's firmware.**
`ota_version.c` compares a literal prefix and up to four numbers, so `p4-1.10`
beats `p4-1.9`, `p4-1.0` does not beat itself, and `s3-9.9` is not an upgrade of
`p4-1.0` at any version number. It refuses rather than guesses on anything it
cannot parse. It is pure C and host-tested in `tools/test_ota_version.c`, because
this is the decision that is hardest to test on hardware and worst to get wrong.

**The panel only installs what it already found for itself.** `ota_apply` names a
version; it does not carry a URL. If that version is not the one the panel's own
manifest poll is currently offering, it is refused. Reaching the broker therefore
does not let anyone choose what firmware the board runs — the manifest URL is
build-time configuration, and the worst a compromised broker can do is ask for an
update the panel was already willing to take.

**The image is verified before the boot partition moves.** It streams into
whichever slot is not running, its SHA-256 is compared with the manifest, and
then `esp_ota_end()` runs IDF's own header checks — magic number, chip id,
secure-boot signature where configured. Both matter, and they answer different
questions: the hash says *this is the file the manifest meant*, the header says
*this is firmware this chip can run*. Any failure aborts and the running image is
untouched. There is no half-updated state.

**A new image has to prove it works.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is
on, so an OTA image boots `PENDING_VERIFY` and the bootloader takes it back on the
next reset unless it calls `esp_ota_mark_app_valid_cancel_rollback()`. `main.c`
calls it after the link task has been running for 30 s — long enough to be past
display init, link init and several hundred loops, which is where an image that
is going to crash does it.

It deliberately does **not** wait for the S3 to answer. A panel whose controller
is switched off is not a broken panel, and rolling back over that would quietly
turn the tub's power switch into a firmware downgrade.

## The mock campaign

`tools/ota_campaign.py` serves a manifest and an image on the LAN, and can lie in
eight different ways. The happy path is the least interesting thing it does.

```sh
# Build the image you want to ship, with its version bumped.
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.net" build

python3 tools/ota_campaign.py \
    --image p4_hmi/build/spa_control_p4_hmi.bin \
    --version p4-1.1
```

It prints the manifest URL. Put that in `idf.py menuconfig` → *Spa HMI* →
*Manifest URL*, flash the panel, and it will find the update within
`CONFIG_SPA_HMI_OTA_POLL_MIN` minutes and start reporting `ota_avail`. Publish
`{"ota_apply": "p4-1.1"}` to `spa/<id>/commands` to install it.

| `--fault` | what the server does | what the panel must do |
|---|---|---|
| `none` | serve it properly | install it, reboot, report the new `fw` |
| `truncate` | cut the connection at `--fault-at`% | `failed: connection dropped mid-transfer`, or `transfer ended early` if the close is seen as a clean EOF |
| `corrupt` | flip one byte mid-image | `failed: sha256 mismatch` |
| `badsha` | good image, wrong hash in the manifest | `failed: sha256 mismatch` |
| `downgrade` | offer an older version | never offer it at all — no `ota_avail` |
| `tiny` | a few hundred bytes with a `200` | `failed: image is too small to be firmware`, before a byte is written |
| `slow` | correct, at `--slow-bps` | install it, or time out cleanly. The one row not yet run on hardware |
| `500` | fail the image after promising it | `failed: image HTTP 500` |

In every failing row the panel must still be running the old firmware afterwards,
and `ota_state` must say which of those it was. A panel that ends up bricked, or
that ends up updated, is a bug in either case.

Two things to know before reading a run of this. `ota_state` holds its last
`failed: ...` until the next poll clears it, so a test that checks it too early
reads the *previous* fault's verdict; wait for the state to leave `failed:`
before sending the next `ota_apply`. And `ota_avail` is **omitted** rather than
nulled when there is nothing on offer, so a client that merges each status
message into a running dictionary will keep a stale version for ever — replace,
do not merge. Both of those cost a false result each the first time this matrix
was run against the panel.

## Turning it off

`CONFIG_SPA_HMI_OTA` defaults to off: a panel that is not being updated should
not be talking to an update server at all. Clearing `CONFIG_SPA_HMI_OTA_MANIFEST`
also stops the poll without a rebuild of anything else, which is the switch to
reach for if a campaign goes wrong.

## What this does to the tub

Nothing, beyond a reboot. The S3 owns the plant and keeps its thermostat,
interlocks and freeze protection whatever the panel is doing; an update reboot
drops SpaLink for a few seconds, which the S3 already handles as "stop honouring
requests" and nothing more. Jets and the light stop, the heater carries on under
the freeze permissive, and the panel comes back asking for nothing — which is the
same thing that happens if you unplug it.

The download itself is harmless: it runs on a task below the link and the UI, and
writes only to the inactive slot.
