# SpaLink v1 — wire protocol

The contract between the ESP32-S3 control node and the ESP32-P4 HMI node.

Two implementations must agree byte for byte: `link/spalink_codec.py` (the
reference, MicroPython) and `link/spalink_codec.c` (ESP-IDF).
`tools/test_spalink.py` runs both against the same vectors and fails on any
divergence. **Change the spec and both implementations together, then run the
tests** — a one-sided change here surfaces as intermittent link faults on the
bench, which is a miserable way to find a typo.

## Design rules

1. **The S3 decides, the P4 asks.** Every P4→S3 message is a *request*. The S3
   applies its own interlocks and may refuse. The P4 learns what actually
   happened only from the S3's `STATUS`, never by assuming its press worked.
2. **Silence is a safe state, not an unknown one.** Both ends time out at
   1500 ms. The S3 falls back to `failsafe_requests()`; the P4 marks its screen
   stale. Neither end waits for the other to say goodbye.
3. **Payloads are at most 7 bytes.** Seven bytes plus the header is exactly one
   classic CAN frame, so the protocol can move to a CAN bus without a
   fragmentation layer. The install link is now RS-485 rather than CAN (see
   `docs/HARDWARE.md`), which needs no such guarantee — but the cap costs
   nothing, keeps every message a single atomic unit on any transport, and
   leaves the door open if a third node ever joins.
4. **No floats on the wire.** Temperatures are deci-Fahrenheit in a signed
   16-bit word, so the C and Python sides cannot disagree about rounding.

## Message format

A message is `msg_id` (1 byte), `hdr` (1 byte), and 0–7 payload bytes.

```
hdr:  bit 7  RESP   this frame is a response (ACK/NACK)
      bit 6  ACK    sender wants an ACK for this frame
      bits 0-5      sequence number, wraps at 64
```

### UART transport (both wirings: direct TTL on the bench, RS-485 for the install)

```
7E | msg_id | hdr | len | payload[len] | crc_hi | crc_lo
```

* `7E` starts a frame. Every byte after it is stuffed: `7E → 7D 5E`,
  `7D → 7D 5D`. So `7E` appears only at a frame start, and a receiver can always
  resynchronise on it after line noise.
* CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) over `msg_id, hdr, len,
  payload` — the unstuffed bytes.
* A `7E` mid-frame abandons the partial frame and starts a new one. A truncated
  frame must never swallow the good frame behind it.

### CAN transport (retained, not currently planned)

The same messages, unchanged. CAN supplies framing, CRC and ACK, so none of the
UART machinery is used:

```
CAN ID = 0x100 + msg_id      (11-bit standard identifier)
data   = hdr | payload...    (≤ 8 bytes)
```

Putting `msg_id` in the identifier means the receiver can filter in hardware, and
message priority falls out of the numbering for free — `ACK` (0x101) wins
arbitration over `STATUS` (0x110), which wins over `REQ` (0x120).

## Messages

### S3 → P4 — state

| id | name | payload | notes |
|---|---|---|---|
| `0x10` | `STATUS` | `outputs`, `inputs`, `fault` | 5 Hz, and immediately on change |
| `0x11` | `TEMP` | `water_dF:i16`, `setpoint_dF:i16` | little-endian, 0.1 °F |
| `0x12` | `TIMERS` | `spa_s:u16`, `light_s:u16` | seconds remaining |
| `0x13` | `HELLO` | `proto`, `fw_major`, `fw_minor`, `board` | 0.5 Hz identity beacon |

`outputs` bits: `PUMP1_LOW 0x01`, `PUMP1_HIGH 0x02`, `PUMP2 0x04`, `PUMP3 0x08`,
`HEATER 0x10`, `JETS 0x20`, `BLOWER 0x40`, `LIGHT 0x80`.

`inputs` bits: `FLOW_SWITCH 0x01`, `HIGH_LIMIT_OK 0x02`, `ESTOP_OK 0x04`,
`TEMP_SENSOR_OK 0x08`, `SPA_ENABLE 0x10`. These are carried so the HMI can
*explain* a fault instead of just naming it.

`fault`: bit 7 set when faulted, low nibble is the code — `1` no flow,
`2` high limit, `3` over temp, `4` e-stop, `5` temp sensor. Same numbering as
`spa_core.FAULT_*` and `spa_state_t.fault_code`.

### P4 → S3 — requests

| id | name | payload | notes |
|---|---|---|---|
| `0x20` | `REQ` | `requests`, `modes` | restated every 400 ms; doubles as the keepalive |
| `0x21` | `SETPOINT` | `setpoint_dF:i16` | ACK requested; refused outside 60–104 °F |
| `0x22` | `PING` | — | keepalive when there is nothing to ask for |
| `0x23` | `CLEAR_FAULT` | — | accepted and ACKed; no fault latches today |

`requests` bits: `SPA_ENABLE 0x01`, `PUMP 0x02`, `PUMP1_HIGH 0x04`, `PUMP2 0x08`,
`PUMP3 0x10`, `JETS 0x20`, `BLOWER 0x40`, `LIGHT 0x80`.
`modes` bits: `ECO 0x01`, `MAX_JET 0x02`.

`REQ` carries the complete desired state every time rather than edges. A lost
frame then costs 400 ms of latency instead of leaving the two boards
disagreeing about whether the blower is on, and it means the keepalive and the
command path are the same mechanism — they cannot drift apart.

### Either direction

| id | name | payload |
|---|---|---|
| `0x01` | `ACK` | `acked_seq` |
| `0x02` | `NACK` | `acked_seq`, `reason` |

`reason`: `1` bad length, `2` unknown id, `3` refused.

A refused setpoint is NACKed rather than clamped: a silently clamped value would
leave the screen showing a number the controller is not using.

## Timing

| what | value | why |
|---|---|---|
| S3 control scan | 50 ms | unchanged from the single-board firmware |
| S3 state broadcast | 200 ms | 5 Hz is smooth on screen and idle on the wire |
| P4 request/keepalive | 400 ms | four per timeout window; one loss is harmless |
| Link timeout, both ends | 1500 ms | rides out a P4 reflash blip; a user still sees jets stop |
| UART baud | 115200 | ~1 % of capacity — headroom for noise, not speed |

## Failure behaviour

**S3 loses the link.** Substitutes `failsafe_requests()`: every user request
drops — jets, blower, pumps 2/3, high speed, light, spa-enable. The thermostat
is *not* in that set. Heating continues from the S3's own measured temperature
and persisted setpoint under the freeze permissive, because losing a touchscreen
must never mean losing freeze protection on a winter night.

**P4 loses the link.** Keeps the last known values on screen but marks them
stale (`spa_state_t.link_up`, `.stale_ms`) and greys them. It never invents a
reading and never assumes a button press took effect.

**Corrupt frame.** Dropped silently; counted in `bad_crc` / `bad_len`. There is
no retransmission because there is nothing worth retransmitting — state is
periodic, requests are restated. Only `SETPOINT` is one-shot, which is why it is
the message that asks for an ACK.
