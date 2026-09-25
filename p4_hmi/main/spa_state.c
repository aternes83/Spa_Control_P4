#include "spa_state.h"

#include <string.h>

void spa_state_init(spa_state_t *s)
{
    memset(s, 0, sizeof(*s));
    /* Start cold and honest: nothing has been heard, so nothing is known. The
     * UI must show "connecting", not a plausible-looking 0 °F reading. */
    s->link_up = false;
    s->ever_connected = false;
}

static int round_half_away(int16_t dF)
{
    return (dF >= 0) ? ((dF + 5) / 10) : (-((-dF + 5) / 10));
}

int spa_state_water_f(const spa_state_t *s)    { return round_half_away(s->water_dF); }
int spa_state_setpoint_f(const spa_state_t *s) { return round_half_away(s->setpoint_dF); }

bool spa_state_apply(spa_state_t *s, const spalink_msg_t *m, uint32_t now_ms)
{
    s->rx_frames++;

    /* REQ, SETPOINT, PING and CLEAR_FAULT are ids only this panel sends. One
     * arriving here did not come from the S3; it is this panel's own frame back
     * off the half-duplex pair. Counting it as a heartbeat is how a link that
     * has never heard the control board shows green, with the dial on "--" and
     * every tile refusing to confirm — the failure looks like a broken UI
     * rather than a broken wire. Count it and say nothing happened.
     *
     * Unknown ids are left alone: they are not this panel's voice, and a future
     * S3 message must not stop looking like a live peer to an older panel. */
    if (spalink_msg_dir(m->msg_id) == SPALINK_DIR_HMI) {
        s->self_echo++;
        return false;
    }

    s->last_rx_ms = now_ms;
    s->stale_ms = 0;
    bool changed = !s->link_up || !s->ever_connected;
    s->link_up = true;
    s->ever_connected = true;

    switch (m->msg_id) {
    case SPALINK_MSG_STATUS:
        if (m->len < 3) {
            return changed;     /* short frame: keep the last good state */
        }
        {
            /* Byte 3 is optional: an older controller sends three. Absent means
             * nothing has timed out, which is also what it meant before the
             * byte existed. */
            uint8_t timed_out = (m->len >= 4) ? m->payload[3] : 0;
            if (s->outputs != m->payload[0] || s->safety_inputs != m->payload[1] ||
                s->timed_out != timed_out) {
                changed = true;
            }
            s->outputs = m->payload[0];
            s->safety_inputs = m->payload[1];
            s->timed_out = timed_out;
        }
        {
            bool active = (m->payload[2] & SPALINK_FAULT_ACTIVE) != 0;
            uint8_t code = m->payload[2] & SPALINK_FAULT_CODE;
            if (active != s->fault_active || code != s->fault_code) {
                changed = true;
            }
            s->fault_active = active;
            s->fault_code = code;
        }
        break;

    case SPALINK_MSG_TEMP:
        if (m->len < 4) {
            return changed;
        }
        {
            int16_t w = spalink_unpack_i16(&m->payload[0]);
            int16_t sp = spalink_unpack_i16(&m->payload[2]);
            /* Only a change in the *displayed* whole degree is a redraw. */
            if (round_half_away(w) != round_half_away(s->water_dF) ||
                round_half_away(sp) != round_half_away(s->setpoint_dF)) {
                changed = true;
            }
            s->water_dF = w;
            s->setpoint_dF = sp;
            if (!s->have_temp) {
                s->have_temp = true;
                changed = true;
            }
        }
        break;

    case SPALINK_MSG_TIMERS:
        if (m->len < 4) {
            return changed;
        }
        {
            uint16_t spa = spalink_unpack_u16(&m->payload[0]);
            uint16_t light = spalink_unpack_u16(&m->payload[2]);
            if (spa / 60 != s->spa_remain_s / 60 ||
                light / 60 != s->light_remain_s / 60) {
                changed = true;     /* the UI shows minutes */
            }
            s->spa_remain_s = spa;
            s->light_remain_s = light;
        }
        break;

    case SPALINK_MSG_HELLO:
        if (m->len < 3) {
            return changed;
        }
        if (s->peer_proto != m->payload[0] ||
            s->peer_fw_major != m->payload[1] ||
            s->peer_fw_minor != m->payload[2]) {
            changed = true;
        }
        s->peer_proto = m->payload[0];
        s->peer_fw_major = m->payload[1];
        s->peer_fw_minor = m->payload[2];
        break;

    default:
        break;      /* ACK/NACK and anything unknown still count as liveness */
    }
    return changed;
}

bool spa_state_tick(spa_state_t *s, uint32_t now_ms, uint32_t timeout_ms)
{
    if (!s->ever_connected) {
        return false;
    }
    uint32_t silent = now_ms - s->last_rx_ms;   /* wraps correctly on uint32 */
    s->stale_ms = silent;
    bool up = silent < timeout_ms;
    if (up == s->link_up) {
        return false;
    }
    s->link_up = up;
    return true;
}

const char *spa_fault_text(uint8_t code)
{
    switch (code) {
    case SPA_FAULT_NONE:        return "OK";
    case SPA_FAULT_NO_FLOW:     return "NO FLOW";
    case SPA_FAULT_HIGH_LIMIT:  return "HIGH LIMIT";
    case SPA_FAULT_OVERTEMP:    return "OVER TEMP";
    case SPA_FAULT_ESTOP:       return "E-STOP";
    case SPA_FAULT_TEMP_SENSOR: return "TEMP SENSOR";
    default:                    return "FAULT";
    }
}
