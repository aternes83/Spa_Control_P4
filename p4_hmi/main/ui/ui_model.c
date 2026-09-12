#include "ui_model.h"

#include <stdio.h>
#include <string.h>

void ui_intent_init(ui_intent_t *ui)
{
    memset(ui, 0, sizeof(*ui));
    /* Every load starts off. The S3 seeds itself with failsafe_requests() for
     * the same reason: a boot must not resume whatever the last session wanted,
     * because nobody is necessarily standing at the tub to notice.
     *
     * Spa-enable is the exception, and is asserted here for good — see the note
     * on this function in the header. */
    ui->spa_enable = true;
}

uint8_t ui_requests(const ui_intent_t *ui)
{
    uint8_t r = 0;

    if (ui->spa_enable) r |= SPALINK_REQ_SPA_ENABLE;
    if (ui->pump1 >= 1) r |= SPALINK_REQ_PUMP;
    if (ui->pump1 >= 2) r |= SPALINK_REQ_PUMP1_HIGH;
    if (ui->pump2)      r |= SPALINK_REQ_PUMP2;
    if (ui->pump3)      r |= SPALINK_REQ_PUMP3;
    if (ui->light)      r |= SPALINK_REQ_LIGHT;

    /* Eco: circulate on Pump 1 low, everything else off. */
    if (ui->eco) {
        r &= (uint8_t)~(SPALINK_REQ_PUMP1_HIGH | SPALINK_REQ_PUMP2 | SPALINK_REQ_PUMP3);
        r |= SPALINK_REQ_PUMP;
    }
    /* Max Jet: everything on, high. Applied second so it wins if both are
     * somehow set — the same precedence the v2.0 firmware used. */
    if (ui->max_jet) {
        r |= SPALINK_REQ_PUMP | SPALINK_REQ_PUMP1_HIGH |
             SPALINK_REQ_PUMP2 | SPALINK_REQ_PUMP3;
    }
    return r;
}

uint8_t ui_modes(const ui_intent_t *ui)
{
    uint8_t m = 0;
    if (ui->eco)     m |= SPALINK_MODE_ECO;
    if (ui->max_jet) m |= SPALINK_MODE_MAX_JET;
    return m;
}

uint32_t ui_max_jet_remain_s(const ui_intent_t *ui, uint32_t now_ms)
{
    if (!ui->max_jet) {
        return 0;
    }
    uint32_t elapsed = now_ms - ui->max_jet_start_ms;   /* wraps correctly */
    if (elapsed >= UI_MAX_JET_MS) {
        return 0;
    }
    return (UI_MAX_JET_MS - elapsed) / 1000u;
}

void ui_intent_tick(ui_intent_t *ui, uint32_t now_ms)
{
    if (ui->max_jet && (uint32_t)(now_ms - ui->max_jet_start_ms) >= UI_MAX_JET_MS) {
        ui->max_jet = false;
    }

    /* Timestamp the rising edge of each *derived* request bit — derived, so that
     * a bit Eco turned on counts from when Eco was pressed, not from when the
     * underlying toggle was last touched. */
    uint8_t r = ui_requests(ui);
    uint8_t rising = (uint8_t)(r & ~ui->last_requests);
    for (int i = 0; i < 8; i++) {
        if (rising & (1u << i)) {
            ui->asserted_ms[i] = now_ms;
        }
    }
    ui->last_requests = r;
}

bool ui_set_eco(ui_intent_t *ui, bool on, const spa_state_t *s, int *setpoint_f_out)
{
    if (on == ui->eco) {
        return false;
    }
    if (on) {
        ui->max_jet = false;
        /* Remember what the controller currently has, not what the dial shows:
         * an in-flight drag has not been accepted yet, and restoring a setpoint
         * the S3 refused would leave the two boards disagreeing. */
        ui->setpoint_before_eco_dF = s->setpoint_dF;
        ui->eco = true;
        *setpoint_f_out = UI_ECO_SETPOINT_F;
    } else {
        ui->eco = false;
        *setpoint_f_out = ui_clamp_setpoint_f(
            (ui->setpoint_before_eco_dF + 5) / 10);
    }
    ui->target_local = false;
    return true;
}

bool ui_set_max_jet(ui_intent_t *ui, bool on, const spa_state_t *s, uint32_t now_ms,
                    int *setpoint_f_out)
{
    if (on == ui->max_jet) {
        return false;
    }
    bool send_setpoint = false;
    if (on) {
        /* Leaving Eco on the way in restores the setpoint it had hijacked. */
        if (ui->eco) {
            send_setpoint = ui_set_eco(ui, false, s, setpoint_f_out);
        }
        ui->max_jet = true;
        ui->max_jet_start_ms = now_ms;
    } else {
        ui->max_jet = false;
    }
    return send_setpoint;
}

/* Which bits in the STATUS frame answer each control's request. */
typedef struct {
    uint8_t req_bit;
    uint8_t out_bits;
} ui_ctl_map_t;

static const ui_ctl_map_t CTL_MAP[UI_CTL_COUNT] = {
    [UI_CTL_PUMP1]      = { SPALINK_REQ_PUMP,
                            SPALINK_OUT_PUMP1_LOW | SPALINK_OUT_PUMP1_HIGH },
    [UI_CTL_PUMP1_HIGH] = { SPALINK_REQ_PUMP1_HIGH, SPALINK_OUT_PUMP1_HIGH },
    [UI_CTL_PUMP2]      = { SPALINK_REQ_PUMP2,      SPALINK_OUT_PUMP2 },
    [UI_CTL_PUMP3]      = { SPALINK_REQ_PUMP3,      SPALINK_OUT_PUMP3 },
    [UI_CTL_LIGHT]      = { SPALINK_REQ_LIGHT,      SPALINK_OUT_LIGHT },
};

ui_ctl_state_t ui_control_state(const ui_intent_t *ui, const spa_state_t *s,
                                ui_ctl_id_t id, uint32_t now_ms)
{
    if ((unsigned)id >= (unsigned)UI_CTL_COUNT) {
        return UI_STATE_OFF;
    }
    const ui_ctl_map_t *m = &CTL_MAP[id];
    bool running   = (s->outputs & m->out_bits) != 0;
    bool requested = (ui_requests(ui) & m->req_bit) != 0;

    if (running) {
        /* True whether or not we asked: the S3 answers to wired panel switches
         * and to its own thermostat as well as to us. */
        return UI_STATE_ON;
    }
    if (!requested) {
        return UI_STATE_OFF;
    }
    /* Asked for but not running. Until the S3 has had time to answer this is
     * ordinary latency; past that it is a refusal, and saying so is the only way
     * a user standing in the cold learns that an interlock is holding the load
     * off rather than that the screen is broken. */
    int bit = __builtin_ctz(m->req_bit);
    uint32_t waited = now_ms - ui->asserted_ms[bit];
    if (!s->link_up || waited < UI_PENDING_GRACE_MS) {
        return UI_STATE_PENDING;
    }
    return UI_STATE_REFUSED;
}

bool ui_pumps_locked(const ui_intent_t *ui, const char **why)
{
    if (ui->max_jet) {
        if (why) *why = "Max Jets";
        return true;
    }
    if (ui->eco) {
        if (why) *why = "Eco Mode";
        return true;
    }
    if (why) *why = NULL;
    return false;
}

bool ui_apply_remote(ui_intent_t *ui, const spa_state_t *s, uint32_t now_ms,
                     const ui_remote_cmd_t *cmd, int *setpoint_f_out)
{
    bool send = false;

    /* Loads first, so a mode applied below overrides them exactly as it would
     * have done had the user pressed the tiles in the same order. */
    if (cmd->has_pump2) ui->pump2 = cmd->pump2;
    if (cmd->has_pump3) ui->pump3 = cmd->pump3;
    if (cmd->has_light) ui->light = cmd->light;
    if (cmd->has_pump1) ui->pump1 = (cmd->pump1 > 2) ? 2 : cmd->pump1;

    /* Max Jet before Eco: if a single command somehow carries both, the same
     * one wins here as wins in ui_requests(). */
    if (cmd->has_max_jet && ui_set_max_jet(ui, cmd->max_jet, s, now_ms, setpoint_f_out)) {
        send = true;
    }
    if (cmd->has_eco && ui_set_eco(ui, cmd->eco, s, setpoint_f_out)) {
        send = true;
    }

    /* An explicit temperature last, so it beats a mode's hand-off in the same
     * command — the app asked for a number and should get it. */
    if (cmd->has_setpoint) {
        *setpoint_f_out = ui_clamp_setpoint_f(cmd->setpoint_f);
        ui->target_local = true;
        ui->target_f = *setpoint_f_out;
        send = true;
    }
    return send;
}

int ui_clamp_setpoint_f(int f)
{
    if (f < UI_SETPOINT_MIN_F) return UI_SETPOINT_MIN_F;
    if (f > UI_SETPOINT_MAX_F) return UI_SETPOINT_MAX_F;
    return f;
}

int ui_target_f(const ui_intent_t *ui, const spa_state_t *s)
{
    if (ui->target_local) {
        return ui->target_f;
    }
    if (!s->ever_connected) {
        /* Nothing has been heard, so there is no setpoint to show. Park the knob
         * mid-range rather than at 0 °F, which would pin it off the arc. */
        return UI_ECO_SETPOINT_F;
    }
    return ui_clamp_setpoint_f(spa_state_setpoint_f(s));
}

void ui_target_settle(ui_intent_t *ui, const spa_state_t *s)
{
    if (ui->target_local && spa_state_setpoint_f(s) == ui->target_f) {
        ui->target_local = false;
    }
}

void ui_format_duration(uint32_t seconds, char *out, size_t n)
{
    if (n == 0) {
        return;
    }
    if (seconds >= 3600) {
        snprintf(out, n, "%luh %02lum", (unsigned long)(seconds / 3600),
                 (unsigned long)((seconds % 3600) / 60));
    } else if (seconds >= 60) {
        snprintf(out, n, "%lum", (unsigned long)(seconds / 60));
    } else {
        snprintf(out, n, "%lus", (unsigned long)seconds);
    }
}
