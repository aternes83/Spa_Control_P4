/*
 * The temperature dial, ported from the iOS app's TemperatureDialView.
 *
 * Same geometry: a 270 degree arc starting at 225 degrees clockwise from noon,
 * spanning 60-110 F, with the current temperature as a filled sweep ending in a
 * white dot and the target as a draggable amber knob. Drag anywhere in the ring
 * to set the target; the value goes to the controller on release, exactly as the
 * app sends it to the broker on release.
 *
 * Two things the app gets for free from SwiftUI and this does not, and how they
 * are done here instead: the heating glow is a second, wider arc underneath at
 * low opacity rather than a shadow filter, and the knob's white core is a small
 * circle positioned by trigonometry rather than a third layer of the same shape.
 *
 * Three things this has that the app does not. A pair of step keys under the
 * ring, because dragging is a fine gesture and not everyone wants it — they move
 * the target a degree a press and repeat on hold, and they go through exactly the
 * same path as a drag, so the caller cannot tell which the user used.
 *
 * And two more. The target reads out under the
 * water temperature at all times rather than only during a drag, because the
 * panel it replaces showed WATER TEMP and TARGET TEMP side by side and people
 * glance at this from across the room. And the middle of the dial does not
 * respond to touch at all: LVGL's arc takes its whole bounding box, so without a
 * dead centre a splash of water landing on the readout would move the setpoint.
 */
#ifndef UI_DIAL_H
#define UI_DIAL_H

#include "lvgl.h"

/* dragging is true for every intermediate value and false exactly once, on
 * release — that is the edge on which the caller sends SETPOINT. */
typedef void (*ui_dial_cb_t)(int target_f, bool dragging);

typedef struct {
    lv_obj_t *card;
    lv_obj_t *arc_glow;     /* the heating halo, hidden unless the heater is on */
    lv_obj_t *arc_water;    /* track, current-temperature fill, white dot */
    lv_obj_t *arc_target;   /* transparent; carries the amber knob */
    lv_obj_t *knob_core;
    lv_obj_t *lbl_temp;
    lv_obj_t *lbl_unit;
    lv_obj_t *lbl_target;
    lv_obj_t *btn_minus;
    lv_obj_t *btn_plus;
    bool      dragging;
    bool      ignore_press;   /* this gesture started in the dead centre */

    /* What was last pushed into the widgets. LVGL invalidates on every write,
     * and ui_tick() runs ten times a second — see the note in ui.c. */
    int  last_water;
    int  last_target;
    int  last_knob;
    bool last_valid;
    bool last_heating;
    lv_color_t last_sweep;
    bool last_enabled;
    bool last_unconfirmed;
    bool applied;
} ui_dial_t;

void ui_dial_create(ui_dial_t *d, lv_obj_t *parent, ui_dial_cb_t cb);

/* Redraw from state. water_valid is false until the controller has reported a
 * temperature at all, in which case the readout is "--" rather than a plausible
 * number nobody has confirmed. unconfirmed is true while the dialled value has
 * not been echoed back by the S3, which brightens the target readout.
 *
 * sweep is the colour of the filled arc and its halo, decided by the caller
 * rather than here: it is water normally, heat while the heater is on, and a
 * mode's own colour while Eco or Max Jets is running — the ring is the largest
 * coloured thing on the panel, so it is what announces the mode across a room.
 * heating still controls whether the halo appears at all, so "the heater is on"
 * survives the ring being some other colour. */
void ui_dial_update(ui_dial_t *d, int water_f, bool water_valid, bool heating,
                    lv_color_t sweep, int target_f, bool unconfirmed, bool enabled);

#endif /* UI_DIAL_H */
