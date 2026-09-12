/*
 * A control board that isn't there.
 *
 * The HMI draws nothing but "waiting for the control board" until an S3 is on
 * the other end of the wire, which makes the panel impossible to judge on the
 * bench before the control node exists. This stands in for one: it takes the
 * requests the UI is producing and answers with the frames a real S3 would send,
 * through the same spa_state_apply() path, so every tile, the dial, the
 * indicator strip and all four control states come alive on the glass.
 *
 * It is a bench aid and nothing more. CONFIG_SPA_HMI_BENCH_PEER is off by
 * default, the whole file compiles out when it is, and a build with it on says
 * so on the console at boot and on the screen's own diagnostics. Never ship it:
 * a panel that answers its own requests is exactly the lie the rest of this
 * firmware is built to avoid.
 *
 * It is deliberately NOT a second implementation of the control core. It honours
 * requests, runs a crude thermostat and counts the light's run timer — enough to
 * exercise the screen. Faults, interlocks and the real permissive belong to
 * spa_core.py and are checked against it by tools/test_spa_core.py; for driving
 * the UI through fault and refusal states, use the browser bench instead, which
 * ports that logic properly.
 */
#ifndef BENCH_PEER_H
#define BENCH_PEER_H

#include <stdbool.h>
#include <stdint.h>

#include "spalink_codec.h"

/* What the UI is asking for, handed over each tick. */
void bench_peer_request(uint8_t requests, uint8_t modes);

/* A setpoint the UI would have put on the wire. */
void bench_peer_setpoint_dF(int16_t dF);

/* Pop the next frame the stand-in would have sent, if one is due. Call in a
 * loop, exactly like spalink_port_recv(). */
bool bench_peer_recv(spalink_msg_t *out, uint32_t now_ms);

#endif /* BENCH_PEER_H */
