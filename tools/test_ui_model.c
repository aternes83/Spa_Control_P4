/*
 * HMI presentation-logic tests. Host-compiled, no LVGL and no ESP-IDF:
 *
 *     cc -std=c11 -I link -I p4_hmi/main -o /tmp/t tools/test_ui_model.c \
 *        p4_hmi/main/ui/ui_model.c p4_hmi/main/spa_state.c link/spalink_codec.c && /tmp/t
 *
 * (tools/run_tests.sh does this for you.)
 *
 * Two properties are worth the file on their own. First, that a button press is
 * a request and nothing more: no path here writes plant state, and a control
 * reads ON only when the controller says the load is energised. Second, that a
 * request the controller declines is *visible* — a screen that quietly shows a
 * pressed button as on is how somebody ends up believing the jets are running
 * when an interlock is holding them off.
 */
#include "ui/ui_model.h"

#include <stdio.h>
#include <string.h>

static int failed = 0;

static void check(const char *name, bool cond, long detail)
{
    if (cond) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (%ld)\n", name, detail);
        failed++;
    }
}

static void check_str(const char *name, const char *got, const char *want)
{
    bool ok = (got == NULL && want == NULL) ||
              (got != NULL && want != NULL && strcmp(got, want) == 0);
    if (ok) {
        printf("  ok   %s\n", name);
    } else {
        printf("  FAIL %s (got \"%s\", want \"%s\")\n", name,
               got ? got : "(null)", want ? want : "(null)");
        failed++;
    }
}

/* A controller that is talking, with the given outputs and inputs. */
static spa_state_t live_state(uint8_t outputs, uint8_t inputs)
{
    spa_state_t s;
    spa_state_init(&s);
    s.link_up = true;
    s.ever_connected = true;
    s.outputs = outputs;
    s.safety_inputs = inputs;
    s.setpoint_dF = 1020;
    s.water_dF = 960;
    return s;
}

int main(void)
{
    ui_intent_t ui;
    spa_state_t s;

    printf("requests are derived from intent\n");
    ui_intent_init(&ui);
    /* This panel is the tub's only control surface and has no off switch, so
     * spa-enable is asserted from boot and never withdrawn. It energises nothing
     * by itself — it only opens the controller's full permissive so a later
     * request can be honoured — and the link failsafe still drops it the moment
     * this board goes quiet. */
    check("spa-enable is asserted at boot",
          ui_requests(&ui) == SPALINK_REQ_SPA_ENABLE, ui_requests(&ui));
    check("and no load is", (ui_requests(&ui) & ~SPALINK_REQ_SPA_ENABLE) == 0,
          ui_requests(&ui));
    ui.pump1 = 1;
    check("pump low asks for PUMP but not HIGH",
          ui_requests(&ui) == (SPALINK_REQ_SPA_ENABLE | SPALINK_REQ_PUMP),
          ui_requests(&ui));
    ui.pump1 = 2;
    check("pump high asks for both",
          (ui_requests(&ui) & (SPALINK_REQ_PUMP | SPALINK_REQ_PUMP1_HIGH)) ==
          (SPALINK_REQ_PUMP | SPALINK_REQ_PUMP1_HIGH), ui_requests(&ui));
    ui.pump2 = true;
    ui.light = true;
    check("pump 2 and the light map to their own bits",
          (ui_requests(&ui) & (SPALINK_REQ_PUMP2 | SPALINK_REQ_LIGHT)) ==
          (SPALINK_REQ_PUMP2 | SPALINK_REQ_LIGHT), ui_requests(&ui));

    printf("eco and max jet override the pumps, on this board\n");
    /* The S3 never learns what Eco is: the modes are applied here, as a rewrite
     * of the request bits, so the control node deals only in requests. */
    ui_intent_init(&ui);
    ui.pump1 = 2;
    ui.pump2 = true;
    ui.pump3 = true;
    ui.eco = true;
    check("eco drops to pump 1 low and nothing else",
          ui_requests(&ui) == (SPALINK_REQ_SPA_ENABLE | SPALINK_REQ_PUMP),
          ui_requests(&ui));
    check("eco is reported in the modes byte",
          ui_modes(&ui) == SPALINK_MODE_ECO, ui_modes(&ui));
    ui.eco = false;
    ui.pump1 = 0;
    ui.pump2 = false;
    ui.pump3 = false;
    ui.max_jet = true;
    check("max jet turns everything on high",
          ui_requests(&ui) == (SPALINK_REQ_SPA_ENABLE | SPALINK_REQ_PUMP |
                               SPALINK_REQ_PUMP1_HIGH | SPALINK_REQ_PUMP2 |
                               SPALINK_REQ_PUMP3),
          ui_requests(&ui));
    ui.eco = true;
    check("max jet wins if both are somehow set",
          (ui_requests(&ui) & SPALINK_REQ_PUMP1_HIGH) != 0, ui_requests(&ui));

    printf("eco takes the setpoint and gives it back\n");
    ui_intent_init(&ui);
    s = live_state(0, SPALINK_IN_SPA_ENABLE);
    s.setpoint_dF = 1020;                     /* 102 F */
    int sp = 0;
    check("entering eco asks for 80 F",
          ui_set_eco(&ui, true, &s, &sp) && sp == UI_ECO_SETPOINT_F, sp);
    check("a second press of the same state asks for nothing",
          !ui_set_eco(&ui, true, &s, &sp), 0);
    s.setpoint_dF = 800;                      /* the S3 confirms 80 F */
    check("leaving eco restores the setpoint it took",
          ui_set_eco(&ui, false, &s, &sp) && sp == 102, sp);

    printf("the two modes cancel each other\n");
    ui_intent_init(&ui);
    s = live_state(0, SPALINK_IN_SPA_ENABLE);
    s.setpoint_dF = 1040;
    ui_set_eco(&ui, true, &s, &sp);
    s.setpoint_dF = 800;
    check("max jet cancels eco and restores its setpoint",
          ui_set_max_jet(&ui, true, &s, 1000, &sp) && sp == 104 && !ui.eco, sp);
    check("max jet is on", ui.max_jet, 0);
    ui_set_eco(&ui, true, &s, &sp);
    check("eco cancels max jet", !ui.max_jet, 0);

    printf("max jet times out on its own\n");
    ui_intent_init(&ui);
    ui_set_max_jet(&ui, true, &s, 1000, &sp);
    ui_intent_tick(&ui, 1000 + UI_MAX_JET_MS - 1);
    check("still running one millisecond short", ui.max_jet, 0);
    check("and it says how long is left",
          ui_max_jet_remain_s(&ui, 1000 + UI_MAX_JET_MS - 1000) == 1,
          (long)ui_max_jet_remain_s(&ui, 1000 + UI_MAX_JET_MS - 1000));
    ui_intent_tick(&ui, 1000 + UI_MAX_JET_MS);
    check("expired at twenty minutes", !ui.max_jet, 0);
    check("and asks for no load once it has",
          ui_requests(&ui) == SPALINK_REQ_SPA_ENABLE, ui_requests(&ui));

    printf("a control reads on only when the controller says so\n");
    ui_intent_init(&ui);
    s = live_state(0, SPALINK_IN_SPA_ENABLE);
    ui.pump2 = true;
    ui_intent_tick(&ui, 10000);
    check("a fresh press reads pending, not on",
          ui_control_state(&ui, &s, UI_CTL_PUMP2, 10000) == UI_STATE_PENDING,
          ui_control_state(&ui, &s, UI_CTL_PUMP2, 10000));
    check("still pending inside the grace window",
          ui_control_state(&ui, &s, UI_CTL_PUMP2,
                           10000 + UI_PENDING_GRACE_MS - 1) == UI_STATE_PENDING, 0);
    check("an unanswered request becomes a visible refusal",
          ui_control_state(&ui, &s, UI_CTL_PUMP2,
                           10000 + UI_PENDING_GRACE_MS) == UI_STATE_REFUSED, 0);
    s.outputs |= SPALINK_OUT_PUMP2;
    check("and reads on as soon as the load is reported energised",
          ui_control_state(&ui, &s, UI_CTL_PUMP2, 99999) == UI_STATE_ON, 0);

    printf("a load nobody asked for still reads on\n");
    /* The S3 runs Pump 1 itself to circulate water for the heater, and answers
     * wired panel switches as well as us. Hiding that would put a running pump
     * behind an "Off" button. */
    ui_intent_init(&ui);
    s = live_state(SPALINK_OUT_PUMP1_LOW | SPALINK_OUT_HEATER, SPALINK_IN_SPA_ENABLE);
    check("jet 1 reads on with no request behind it",
          ui_control_state(&ui, &s, UI_CTL_PUMP1, 5000) == UI_STATE_ON, 0);
    check("and the user's own selection is untouched by that", ui.pump1 == 0,
          ui.pump1);

    printf("silence is never mistaken for refusal\n");
    ui_intent_init(&ui);
    s = live_state(0, SPALINK_IN_SPA_ENABLE);
    s.link_up = false;
    ui.pump3 = true;
    ui_intent_tick(&ui, 1000);
    check("a request outlives the grace window while the link is down",
          ui_control_state(&ui, &s, UI_CTL_PUMP3, 1000 + 60000) == UI_STATE_PENDING,
          0);

    printf("the pending grace survives the millisecond clock wrapping\n");
    ui_intent_init(&ui);
    s = live_state(0, SPALINK_IN_SPA_ENABLE);
    ui.light = true;
    ui_intent_tick(&ui, 0xFFFFFF00u);
    check("pending across the wrap",
          ui_control_state(&ui, &s, UI_CTL_LIGHT, 0x00000100u) == UI_STATE_PENDING,
          0);
    check("refused a little later, across the same wrap",
          ui_control_state(&ui, &s, UI_CTL_LIGHT, 0x00000900u) == UI_STATE_REFUSED,
          0);

    printf("a mode takes the pumps\n");
    ui_intent_init(&ui);
    const char *why = NULL;
    check("nothing owns them to begin with", !ui_pumps_locked(&ui, &why), 0);
    ui.eco = true;
    check_str("eco says so by name", (ui_pumps_locked(&ui, &why), why), "Eco Mode");
    ui.eco = false;
    ui.max_jet = true;
    check_str("and so does max jet", (ui_pumps_locked(&ui, &why), why), "Max Jets");

    printf("the dial's target\n");
    ui_intent_init(&ui);
    s = live_state(0, SPALINK_IN_SPA_ENABLE);
    s.setpoint_dF = 1020;
    check("follows the controller when nothing is being dragged",
          ui_target_f(&ui, &s) == 102, ui_target_f(&ui, &s));
    ui.target_local = true;
    ui.target_f = 99;
    check("holds the dialled value while it is unconfirmed",
          ui_target_f(&ui, &s) == 99, ui_target_f(&ui, &s));
    ui_target_settle(&ui, &s);
    check("and keeps holding it while the S3 still reports the old one",
          ui.target_local && ui_target_f(&ui, &s) == 99, 0);
    s.setpoint_dF = 990;
    ui_target_settle(&ui, &s);
    check("letting go once the S3 confirms", !ui.target_local, 0);

    printf("the dial spans more than the controller accepts\n");
    check("clamped up from below", ui_clamp_setpoint_f(55) == UI_SETPOINT_MIN_F, 0);
    check("clamped down from above", ui_clamp_setpoint_f(110) == UI_SETPOINT_MAX_F, 0);
    check("the ring reaches past the setpoint range",
          UI_DIAL_MAX_F > UI_SETPOINT_MAX_F, 0);
    spa_state_init(&s);
    check("a target before first contact is not 0 F",
          ui_target_f(&ui, &s) >= UI_SETPOINT_MIN_F, ui_target_f(&ui, &s));

    printf("durations\n");
    char buf[16];
    ui_format_duration(45, buf, sizeof(buf));
    check_str("seconds", buf, "45s");
    ui_format_duration(12 * 60, buf, sizeof(buf));
    check_str("minutes", buf, "12m");
    ui_format_duration(3 * 3600 + 58 * 60, buf, sizeof(buf));
    check_str("hours and minutes", buf, "3h 58m");
    ui_format_duration(0, buf, sizeof(buf));
    check_str("zero", buf, "0s");

    printf("\n%s\n", failed ? "FAILED" : "all HMI model tests passed");
    return failed ? 1 : 0;
}
