/*
 * The HMI screen.
 *
 * 800x480 landscape, laid out after the v2.0 HMI that has been running this tub:
 * a status strip at the top, the temperature panel on the left, four control
 * groups on the right, and a status bar along the bottom. The colours, cards and
 * the dial come from the SpaControl iOS app. The map is in ui_theme.h.
 *
 * The one real change from v2.0 is the temperature panel. Where that had two
 * columns of seven-segment digits and a pair of +/- buttons, this has one dial:
 * the water is the filled sweep, the target is the knob you drag, and both
 * numbers still read out in the middle. Nudging a setpoint one degree at a time
 * with a wet finger was the worst interaction on the old panel.
 *
 * Threading. Everything here runs under the LVGL lock: touch callbacks are
 * called from the lvgl_port task, and ui_tick() must be called from the link
 * task with bsp_display_lock() held. That single rule is what makes the shared
 * intent safe without a second mutex.
 *
 * Direction of data. ui_tick() takes the controller's state and gives back what
 * the user is asking for. The UI never sends anything itself and never writes to
 * spa_state_t — a button press moves a bit in ui_intent_t and nothing else, and
 * the screen goes on showing what the S3 reports until the S3 reports different.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

#include "spa_state.h"
#include "ui_model.h"   /* ui_remote_cmd_t, for ui_post_remote() */

typedef struct {
    uint8_t requests;       /* SPALINK_REQ_* — restate these every REQ_TX_MS */
    uint8_t modes;          /* SPALINK_MODE_* */
    bool    send_setpoint;  /* the user moved the dial, or a mode moved it for them */
    int     setpoint_f;
    /* Reported out so a publisher does not need to reach into the intent. Eco
     * and Max Jets are applied on this board (docs/HMI.md), so they are the two
     * status fields the S3 cannot tell anyone about. */
    bool    eco;
    bool    max_jet;
} ui_out_t;

/* Brightness is the one thing the UI needs from the board. Passing it in keeps
 * this file free of the BSP, which is also what lets it be read on a host. */
typedef void (*ui_brightness_cb_t)(int percent);

/* Builds the screen on the active display. Call with the LVGL lock held. */
void ui_init(ui_brightness_cb_t brightness_cb);

/* Folds the controller's state into the screen and reports what the HMI now
 * wants. Call with the LVGL lock held. */
void ui_tick(const spa_state_t *s, uint32_t now_ms, ui_out_t *out);

/* Land a command that arrived from somewhere other than the glass — today MQTT,
 * tomorrow whatever else asks. Safe to call from any task.
 *
 * It does NOT apply the command; it queues it, and the next ui_tick() applies it
 * through ui_apply_remote() on the task that already owns the intent. That is
 * deliberate: ui.h's threading rule is what lets the intent live without a
 * second mutex, and a phone must not be the one thing allowed to break it.
 *
 * A command that arrives faster than the tick rate coalesces onto the previous
 * one field by field, which is the right answer for a control surface: the last
 * thing asked for is what the user wants. */
void ui_post_remote(const ui_remote_cmd_t *cmd);

/* The status strip carries three things this board does not own yet.
 *
 * The radios live on the ESP32-C6 across the SDIO link, and that firmware is not
 * written; the clock wants either NTP through the C6 or the carrier's RTC on the
 * shared I2C bus, and neither is driven. Both indicators therefore render as
 * "off" and the clock as "--:--" until something calls these — which is honest,
 * and is one line of glue each when the radio work lands, instead of a rewrite.
 * Call with the LVGL lock held. */
void ui_set_radio(bool wifi_up, bool bt_up);
void ui_set_clock(int hour, int minute);    /* hour < 0 renders "--:--" */

#endif /* UI_H */
