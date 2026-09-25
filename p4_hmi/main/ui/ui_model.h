/*
 * What the screen should show, and what the user is asking for.
 *
 * Free of LVGL and ESP-IDF on purpose, exactly like spa_state.c: this is where
 * the interesting decisions live — whether a button reads on, off, pending or
 * refused; what Eco does to the pump requests; when Max Jet times out — and all
 * of it is covered by tools/test_ui_model.c on a host.
 *
 * The split with spa_state.h is the same one the protocol makes:
 *
 *   spa_state_t   what the S3 says is true.        Read-only to the UI.
 *   ui_intent_t   what the user has asked for.     Owned here, never echoed
 *                                                  back from the controller.
 *
 * Keeping them apart is the whole reason the screen cannot lie. A control's
 * *fill* comes from spa_state_t — the load is on only when the S3 reports it on
 * — while its *selection* comes from ui_intent_t. When the two disagree the
 * difference is itself the interesting thing to draw, which is what
 * ui_control_state() computes.
 */
#ifndef UI_MODEL_H
#define UI_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spa_state.h"

/* Dial geometry, in °F. The ring spans a wider range than the setpoint may take
 * because the sensor can and does read outside it — a 55 °F tub in February
 * must still land somewhere on the arc instead of piling up against the end. */
#define UI_DIAL_MIN_F      60
#define UI_DIAL_MAX_F      110
#define UI_SETPOINT_MIN_F  60      /* matches SETPOINT_MIN_F on the S3 */
#define UI_SETPOINT_MAX_F  104     /* matches SETPOINT_MAX_F on the S3 */

/* Inherited from the v2.0 firmware's HMI, which has been running this tub. */
#define UI_ECO_SETPOINT_F  80
#define UI_MAX_JET_MS      (20u * 60u * 1000u)

/* How long a request may go unanswered before the screen stops calling it
 * "pending" and starts calling it refused. The S3 reports state at 5 Hz and we
 * restate requests at 4 Hz, so a round trip is well under 500 ms; a second and a
 * half of silence means an interlock said no, not that a frame was lost. */
#define UI_PENDING_GRACE_MS 1500

/* Every control the dashboard can draw. There is no spa-enable here: see
 * ui_intent_init(). */
typedef enum {
    UI_CTL_PUMP1 = 0,
    UI_CTL_PUMP1_HIGH,
    UI_CTL_PUMP2,
    UI_CTL_PUMP3,
    UI_CTL_LIGHT,
    UI_CTL_COUNT,
} ui_ctl_id_t;

typedef enum {
    UI_STATE_OFF = 0,   /* not asked for, not running */
    UI_STATE_ON,        /* the S3 reports the load energised */
    UI_STATE_PENDING,   /* asked for, not yet confirmed — still inside the grace */
    UI_STATE_REFUSED,   /* asked for and still not running: an interlock said no */
} ui_ctl_state_t;

typedef struct {
    /* The user's requests. Nothing here is ever written from a received frame. */
    uint8_t  pump1;              /* 0 off, 1 low, 2 high */
    bool     pump2;
    bool     pump3;
    bool     light;
    bool     spa_enable;         /* held true for the life of the panel */
    bool     eco;
    bool     max_jet;

    uint32_t max_jet_start_ms;
    int16_t  setpoint_before_eco_dF;   /* restored when Eco is switched back off */

    /* The setpoint the user has just dialled in, held until the S3 echoes it
     * back, so the knob does not snap to the old value between release and
     * confirmation. Mirrors `localTarget` in the iOS app's dial. */
    bool     target_local;
    int      target_f;

    /* When each derived request bit last went true, for the pending/refused
     * distinction. Indexed by the bit's position in the REQ byte. */
    uint32_t asserted_ms[8];
    uint8_t  last_requests;
} ui_intent_t;

/* Starts everything off except spa-enable, which is asserted from boot and never
 * withdrawn. This panel is the tub's only control surface and it is always on;
 * there is no off, so there is no button for one and no standby state to draw.
 *
 * Asserting it costs nothing on its own — spa-enable energises no load, it only
 * opens the controller's full permissive so that a *later* request can be
 * honoured. What still protects the tub is the link: the moment this board stops
 * talking, the S3 substitutes failsafe_requests() and drops enable with
 * everything else. That is the guard that matters, and it does not depend on a
 * user remembering to press anything. */
void ui_intent_init(ui_intent_t *ui);

/* The REQ bytes to put on the wire. Eco and Max Jet are applied *here* rather
 * than on the S3: they are nothing but an override of which pump bits are set,
 * so the control node never needs to learn a UI concept. The modes byte is sent
 * anyway — it is already in the protocol, and it lets the S3's log say why the
 * pumps are doing what they are doing. */
uint8_t ui_requests(const ui_intent_t *ui);
uint8_t ui_modes(const ui_intent_t *ui);

/* Call every UI tick. Expires Max Jet and timestamps newly-asserted requests. */
void ui_intent_tick(ui_intent_t *ui, uint32_t now_ms);

/* Let go of anything the controller has stopped honouring because its own
 * runtime ceiling ran out — the 20 minutes on a high-flow pump, the hour on the
 * light. Call every tick, straight after ui_intent_tick().
 *
 * Without this the panel keeps asking forever and the load can never come back,
 * because on the S3 it is releasing the request that restarts the ceiling. The
 * tile also sits amber on "Refused" for the rest of the evening, which says an
 * interlock is holding the load off when nothing is: the run simply ended. Both
 * were reported from the tub — jets 2 and 3 stuck refusing, and a light whose
 * button never went out when its hour was up.
 *
 * Only a timeout releases a request. An interlock refusal leaves it standing and
 * the tile amber, which is the one case a user does need to see.
 *
 * Nothing is released inside UI_PENDING_GRACE_MS of being asked for, so a press
 * that lands before the S3's next STATUS is not cancelled by the timeout flag
 * from the *previous* run — the flag clears on the controller as soon as the
 * request drops, but the panel can be a frame or two ahead of hearing it. */
void ui_intent_release_timeouts(ui_intent_t *ui, const spa_state_t *s,
                                uint32_t now_ms);

/* Eco locks the setpoint to 80 °F and restores the previous one on the way out,
 * as the v2.0 HMI did. Returns true if the caller must send a SETPOINT frame,
 * with the value in *setpoint_f_out. Entering Eco cancels Max Jet, and the
 * reverse, because their pump overrides contradict each other. */
bool ui_set_eco(ui_intent_t *ui, bool on, const spa_state_t *s, int *setpoint_f_out);
bool ui_set_max_jet(ui_intent_t *ui, bool on, const spa_state_t *s, uint32_t now_ms,
                    int *setpoint_f_out);

/* Seconds of Max Jet left, or 0 when it is not running. */
uint32_t ui_max_jet_remain_s(const ui_intent_t *ui, uint32_t now_ms);

/* How one control should be drawn. See ui_ctl_state_t.
 *
 * The case worth naming: a control the user has *not* asked for can still read
 * ON, because the S3 runs Jet 1 by itself to circulate water for the heater.
 * That is not a disagreement to be hidden — it is the true state of the plant,
 * and it is what the Jet 1 heading reports while the segments below go on
 * showing what the user selected. */
ui_ctl_state_t ui_control_state(const ui_intent_t *ui, const spa_state_t *s,
                                ui_ctl_id_t id, uint32_t now_ms);

/* True while Eco or Max Jet owns the pumps, so the pump controls must be locked
 * — otherwise a press would appear to do nothing, the mode having overridden it
 * on the very next frame. Names the mode in *why for the caption. */
bool ui_pumps_locked(const ui_intent_t *ui, const char **why);

/* The target to draw on the dial: the value being dragged if there is one, else
 * the setpoint the S3 has confirmed. */
int  ui_target_f(const ui_intent_t *ui, const spa_state_t *s);
/* Take a dialled value, clamped to what the S3 will accept. */
int  ui_clamp_setpoint_f(int f);
/* Drop the optimistic target once the controller confirms it. */
void ui_target_settle(ui_intent_t *ui, const spa_state_t *s);

/* ── Remote commands ─────────────────────────────────────────────────────────
 * What the app sends over MQTT, decoded. Every field is optional — the app omits
 * what has not changed — hence a has_* flag beside each.
 *
 * This exists so a command from the phone and a press on the glass land in the
 * SAME place. If MQTT got its own control path the two would show different
 * things and fight each other; routed through here, a remote command is a
 * request like any other and inherits the S3's veto for free. See docs/MQTT.md.
 */
typedef struct {
    bool has_pump1;     uint8_t pump1;      /* 0 off, 1 low, 2 high */
    bool has_pump2;     bool    pump2;
    bool has_pump3;     bool    pump3;
    bool has_light;     bool    light;
    bool has_eco;       bool    eco;
    bool has_max_jet;   bool    max_jet;
    bool has_setpoint;  int     setpoint_f;
} ui_remote_cmd_t;

/* Fold one command into the intent. Returns true if the caller must send a
 * SETPOINT frame, with the value in *setpoint_f_out — which happens both when
 * the app asks for a temperature and when it toggles a mode that moves it.
 *
 * Modes go through ui_set_eco()/ui_set_max_jet() rather than the fields, so the
 * setpoint hand-off and the mutual cancellation cannot be skipped by arriving
 * over the network instead of through a finger. */
bool ui_apply_remote(ui_intent_t *ui, const spa_state_t *s, uint32_t now_ms,
                     const ui_remote_cmd_t *cmd, int *setpoint_f_out);

/* "3h 58m", "12m", "45s" — for the run timers. Writes at most `n` bytes. */
void ui_format_duration(uint32_t seconds, char *out, size_t n);

#endif /* UI_MODEL_H */
