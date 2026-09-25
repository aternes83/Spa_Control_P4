#include "bench_peer.h"

#include "sdkconfig.h"

#if CONFIG_SPA_HMI_BENCH_PEER

#include <string.h>

#include "spa_state.h"

/* The same rates the S3 uses, so the screen's staleness logic sees what it would
 * see in the field rather than a firehose. */
#define STATE_TX_MS  200
#define HELLO_TX_MS  2000
#define LIGHT_RUN_MS (60u * 60u * 1000u)

/* Accelerated, so the arc visibly turns amber while somebody is standing at the
 * bench. A real tub moves about a degree a minute. */
#define HEAT_RATE_dF_PER_S  8
#define COOL_RATE_dF_PER_S  1

static uint8_t  s_req, s_modes;
static int16_t  s_water_dF = 940;
static int16_t  s_setpoint_dF = 1020;
static uint32_t s_light_start_ms;
static bool     s_light_running;
static bool     s_heater;
static uint32_t s_next_state_ms, s_next_hello_ms, s_last_step_ms;
static uint8_t  s_queue;          /* which of the three state frames is next */

void bench_peer_request(uint8_t requests, uint8_t modes)
{
    s_req = requests;
    s_modes = modes;
}

void bench_peer_setpoint_dF(int16_t dF)
{
    /* The same window the S3 accepts; outside it a real one would NACK. */
    if (dF >= 600 && dF <= 1040) {
        s_setpoint_dF = dF;
    }
}

/* One scan: honour the requests, move the water, count the light's timer. */
static void step(uint32_t now_ms)
{
    uint32_t dt = now_ms - s_last_step_ms;
    s_last_step_ms = now_ms;
    if (dt > 1000) {
        dt = 1000;              /* first call, or a long stall */
    }

    bool enable = (s_req & SPALINK_REQ_SPA_ENABLE) != 0;
    bool want_heat = s_water_dF < s_setpoint_dF;

    /* Jet 1 runs for the thermostat as well as on request — the behaviour the
     * screen's "Low against an Off selection" case exists to show. */
    bool p1_high = enable && (s_req & SPALINK_REQ_PUMP1_HIGH);
    bool p1_low  = (want_heat || (enable && (s_req & SPALINK_REQ_PUMP))) && !p1_high;
    s_heater = want_heat && (p1_low || p1_high);

    int32_t rate = s_heater ? HEAT_RATE_dF_PER_S : -COOL_RATE_dF_PER_S;
    s_water_dF += (int16_t)((rate * (int32_t)dt) / 1000);
    if (s_water_dF < 500)  s_water_dF = 500;
    if (s_water_dF > 1150) s_water_dF = 1150;

    if (s_req & SPALINK_REQ_LIGHT) {
        if (!s_light_running) {
            s_light_running = true;
            s_light_start_ms = now_ms;
        }
    } else {
        s_light_running = false;
    }
}

static bool light_on(uint32_t now_ms)
{
    return s_light_running && (now_ms - s_light_start_ms) < LIGHT_RUN_MS;
}

static uint16_t light_remain_s(uint32_t now_ms)
{
    if (!light_on(now_ms)) {
        return 0;
    }
    return (uint16_t)((LIGHT_RUN_MS - (now_ms - s_light_start_ms)) / 1000u);
}

bool bench_peer_recv(spalink_msg_t *out, uint32_t now_ms)
{
    memset(out, 0, sizeof(*out));

    /* Three state frames go out back to back, as the S3 sends them. */
    if (s_queue == 0 && (int32_t)(now_ms - s_next_state_ms) >= 0) {
        s_next_state_ms = now_ms + STATE_TX_MS;
        step(now_ms);
        s_queue = 1;
    }

    bool enable = (s_req & SPALINK_REQ_SPA_ENABLE) != 0;

    switch (s_queue) {
    case 1: {
        bool p1_high = enable && (s_req & SPALINK_REQ_PUMP1_HIGH);
        bool p1_low  = (s_water_dF < s_setpoint_dF ||
                        (enable && (s_req & SPALINK_REQ_PUMP))) && !p1_high;
        out->msg_id = SPALINK_MSG_STATUS;
        out->len = 4;
        out->payload[0] = (uint8_t)((p1_low ? SPALINK_OUT_PUMP1_LOW : 0) |
                                    (p1_high ? SPALINK_OUT_PUMP1_HIGH : 0) |
                                    ((enable && (s_req & SPALINK_REQ_PUMP2)) ? SPALINK_OUT_PUMP2 : 0) |
                                    ((enable && (s_req & SPALINK_REQ_PUMP3)) ? SPALINK_OUT_PUMP3 : 0) |
                                    (s_heater ? SPALINK_OUT_HEATER : 0) |
                                    (light_on(now_ms) ? SPALINK_OUT_LIGHT : 0));
        /* Every interlock healthy: this stand-in has no faults to report. */
        out->payload[1] = (uint8_t)(SPALINK_IN_FLOW_SWITCH | SPALINK_IN_HIGH_LIMIT_OK |
                                    SPALINK_IN_ESTOP_OK | SPALINK_IN_TEMP_SENSOR_OK |
                                    (enable ? SPALINK_IN_SPA_ENABLE : 0));
        out->payload[2] = 0;
        /* The light is the only ceiling this stand-in keeps, so it is the only
         * load it can report as timed out rather than refused. */
        out->payload[3] = (uint8_t)((s_light_running && !light_on(now_ms))
                                    ? SPALINK_OUT_LIGHT : 0);
        s_queue = 2;
        return true;
    }
    case 2:
        out->msg_id = SPALINK_MSG_TEMP;
        out->len = 4;
        spalink_pack_i16(s_water_dF, &out->payload[0]);
        spalink_pack_i16(s_setpoint_dF, &out->payload[2]);
        s_queue = 3;
        return true;
    case 3:
        out->msg_id = SPALINK_MSG_TIMERS;
        out->len = 4;
        spalink_pack_u16(0, &out->payload[0]);     /* no run-timer ceiling */
        spalink_pack_u16(light_remain_s(now_ms), &out->payload[2]);
        s_queue = 0;
        return true;
    default:
        break;
    }

    if ((int32_t)(now_ms - s_next_hello_ms) >= 0) {
        s_next_hello_ms = now_ms + HELLO_TX_MS;
        out->msg_id = SPALINK_MSG_HELLO;
        out->len = 4;
        out->payload[0] = SPALINK_PROTO_VERSION;
        out->payload[1] = 0;
        out->payload[2] = 0;      /* fw 0.0 — visibly not a real control board */
        out->payload[3] = 0xBE;   /* board id: bench */
        return true;
    }
    return false;
}

#endif /* CONFIG_SPA_HMI_BENCH_PEER */
