#include "ui_dial.h"

#include <stdio.h>
#include <string.h>

#include "ui_model.h"
#include "ui_theme.h"

/* LVGL measures angles from 3 o'clock, clockwise. The app measures from noon, so
 * its 225 degree start is 135 here, and its 270 degree sweep ends at 405 = 45. */
#define ARC_START_DEG 135
#define ARC_SWEEP_DEG 270
#define ARC_END_DEG   ((ARC_START_DEG + ARC_SWEEP_DEG) % 360)

#define KNOB_CORE_D 12

static ui_dial_cb_t s_cb;

static void knob_centre(int value_f, int *x, int *y);

/* Place the knob's white core, and say what the knob is pointing at. Called from
 * the drag as well as from ui_dial_update, because at ten redraws a second the
 * core would visibly trail the finger if it only moved on the tick. */
static void place_knob(ui_dial_t *d)
{
    int v = (int)lv_arc_get_value(d->arc_target);
    if (d->applied && v == d->last_knob) {
        return;
    }
    d->last_knob = v;
    int kx, ky;
    knob_centre(v, &kx, &ky);
    lv_obj_set_pos(d->knob_core, kx - KNOB_CORE_D / 2, ky - KNOB_CORE_D / 2);
}

/* Push a target value out as though it had been dragged there. */
static void emit_target(ui_dial_t *d, int v, bool settling)
{
    lv_arc_set_value(d->arc_target, v);
    place_knob(d);
    lv_label_set_text_fmt(d->lbl_target, "TARGET  %d°", v);
    if (s_cb) {
        s_cb(v, settling);
    }
}

typedef struct {
    ui_dial_t *d;
    int        delta;
} ui_step_t;
static ui_step_t s_step_down, s_step_up;

static void step_event(lv_event_t *e)
{
    ui_step_t *st = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        /* On the press, not the click: a step key should answer under the finger
         * rather than on the way up. Holding repeats. */
        int v = ui_clamp_setpoint_f((int)lv_arc_get_value(st->d->arc_target) + st->delta);
        emit_target(st->d, v, true);
    } else if (code == LV_EVENT_RELEASED) {
        /* One frame on the wire when the finger leaves, however many degrees it
         * travelled — the same bargain the drag makes. */
        emit_target(st->d, (int)lv_arc_get_value(st->d->arc_target), false);
    }
}

static void arc_event(lv_event_t *e)
{
    lv_obj_t *arc = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    ui_dial_t *d = lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        /* LVGL's arc takes presses anywhere in its bounding box, which is what
         * makes the ring easy to grab and also means a splash, a dropped sponge
         * or a palm laid on the readout would spin the setpoint. Only a press
         * that lands outside the middle starts a drag.
         *
         * If the input device will not tell us where the press landed we accept
         * it, so the failure mode is the old behaviour rather than a dial that
         * cannot be turned at all. */
        d->ignore_press = false;
        lv_indev_t *indev = lv_indev_active();
        if (indev != NULL) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            lv_area_t a;
            lv_obj_get_coords(arc, &a);
            int32_t dx = p.x - (a.x1 + a.x2) / 2;
            int32_t dy = p.y - (a.y1 + a.y2) / 2;
            if (dx * dx + dy * dy <
                (int32_t)UI_DIAL_DEADZONE * (int32_t)UI_DIAL_DEADZONE) {
                d->ignore_press = true;
            }
        }
        return;
    }
    if (d->ignore_press) {
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        /* The ring spans more than the controller will accept, because the water
         * can read outside the setpoint range. Pin the knob at the limit rather
         * than letting it travel somewhere the S3 would only NACK. */
        int v = ui_clamp_setpoint_f((int)lv_arc_get_value(arc));
        if (v != (int)lv_arc_get_value(arc)) {
            lv_arc_set_value(arc, v);
        }
        d->dragging = true;
        place_knob(d);
        lv_label_set_text_fmt(d->lbl_target, "Target %d°", v);
        if (s_cb) s_cb(v, true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!d->dragging) {
            return;         /* a tap that never moved the knob asks for nothing */
        }
        d->dragging = false;
        if (s_cb) s_cb(ui_clamp_setpoint_f((int)lv_arc_get_value(arc)), false);
    }
}

/* Where the knob's centre sits, in the arc's own coordinates. */
static void knob_centre(int value_f, int *x, int *y)
{
    const int c = UI_DIAL_SIZE / 2;
    const int r = c - UI_DIAL_ARC_W / 2;
    int span = UI_DIAL_MAX_F - UI_DIAL_MIN_F;
    int t = value_f - UI_DIAL_MIN_F;
    if (t < 0) t = 0;
    if (t > span) t = span;

    /* LVGL's own fixed-point tables rather than sinf/cosf: this runs on every
     * value change and the result is rounded to a pixel anyway. */
    int a = ARC_START_DEG + (ARC_SWEEP_DEG * t) / span;
    int32_t s = lv_trigo_sin((int16_t)a);                 /* 1.15 fixed point */
    int32_t k = lv_trigo_sin((int16_t)(a + 90));          /* = cos */
    *x = c + (int)((r * k) >> LV_TRIGO_SHIFT);
    *y = c + (int)((r * s) >> LV_TRIGO_SHIFT);
}

/* A step key. Plain "-" and "+" at the readout's own size rather than a pair of
 * chevrons: this exists for somebody who finds the ring fiddly, and a small
 * glyph would miss the point of it. */
static lv_obj_t *make_step_key(lv_obj_t *parent, const char *glyph, ui_step_t *st)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, UI_STEPPER_H);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, 64, 0);                   /* black 25 %, as the segments */
    lv_obj_set_style_radius(b, UI_RADIUS_TILE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, UI_C_WATER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, UI_HIT_SLOP);
    lv_obj_add_event_cb(b, step_event, LV_EVENT_PRESSED, st);
    lv_obj_add_event_cb(b, step_event, LV_EVENT_LONG_PRESSED_REPEAT, st);
    lv_obj_add_event_cb(b, step_event, LV_EVENT_RELEASED, st);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, UI_FONT_TEMP, 0);
    lv_obj_set_style_text_color(l, UI_C_TEXT, 0);
    lv_label_set_text(l, glyph);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *make_arc(lv_obj_t *parent)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, UI_DIAL_SIZE, UI_DIAL_SIZE);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, ARC_START_DEG, ARC_END_DEG);
    lv_arc_set_range(arc, UI_DIAL_MIN_F, UI_DIAL_MAX_F);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    /* The dial lives on a scrollable page. Without this a drag across the ring
     * is claimed by the page and scrolls it instead of moving the knob. */
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLL_CHAIN);
    return arc;
}

void ui_dial_create(ui_dial_t *d, lv_obj_t *parent, ui_dial_cb_t cb)
{
    s_cb = cb;
    memset(d, 0, sizeof(*d));
    d->dragging = false;
    d->applied = false;

    d->card = lv_obj_create(parent);
    lv_obj_remove_style_all(d->card);
    lv_obj_add_style(d->card, &ui_st_card, 0);
    lv_obj_set_size(d->card, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(d->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(d->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(d->card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(d->card, 8, 0);

    /* ── Header: "WATER TEMP" and the drag hint, as in the app ─────────────── */
    lv_obj_t *head = lv_obj_create(d->card);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(head);
    lv_obj_add_style(title, &ui_st_section, 0);
    lv_label_set_text(title, "WATER TEMP");

    /* ── The ring ────────────────────────────────────────────────────────────
     * The box is larger than the arc by the knob's overhang. Without that the
     * knob is clipped at 3 and 9 o'clock, where the overhang points straight at
     * the edge of a box exactly as wide as the arc. See ui_theme.h. */
    lv_obj_t *ring = lv_obj_create(d->card);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, UI_DIAL_BOX, UI_DIAL_BOX);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    /* Heating halo. A wider arc at low opacity under the fill, which is how the
     * app's .shadow filter reads once it is on a 16-bit panel. */
    d->arc_glow = make_arc(ring);
    lv_obj_set_style_arc_opa(d->arc_glow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(d->arc_glow, UI_DIAL_ARC_W + 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d->arc_glow, UI_C_HEAT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(d->arc_glow, LV_OPA_30, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(d->arc_glow, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(d->arc_glow, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_add_flag(d->arc_glow, LV_OBJ_FLAG_HIDDEN);

    /* Track, current-temperature fill, and the white dot at its end. */
    d->arc_water = make_arc(ring);
    lv_obj_set_style_arc_width(d->arc_water, UI_DIAL_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(d->arc_water, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(d->arc_water, 20, LV_PART_MAIN);      /* white 8% */
    lv_obj_set_style_arc_width(d->arc_water, UI_DIAL_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d->arc_water, UI_C_WATER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(d->arc_water, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(d->arc_water, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(d->arc_water, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    /* The knob is the arc's width plus its padding, so a negative pad shrinks
     * the dot inside the stroke instead of straddling it. */
    lv_obj_set_style_pad_all(d->arc_water, -(UI_DIAL_ARC_W - 14) / 2, LV_PART_KNOB);

    /* The target knob rides an otherwise invisible arc, so LVGL does the drag
     * maths and we do not have to reimplement the app's atan2 dead-zone. */
    d->arc_target = make_arc(ring);
    lv_obj_set_style_arc_opa(d->arc_target, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(d->arc_target, LV_OPA_TRANSP, LV_PART_INDICATOR);
    /* Invisible, but NOT zero width. LVGL sizes and places the knob from the
     * INDICATOR's arc width — get_knob_area() takes indic_width/2 off the radius
     * and adds it to the knob's half-extent — so an indicator that is merely
     * transparent still has to carry the real width. Left at 0 the knob comes
     * out a 7 px dot sitting 12 px off the stroke, which is how it went missing
     * on the panel while looking fine in a browser that does none of this. */
    lv_obj_set_style_arc_width(d->arc_target, UI_DIAL_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(d->arc_target, UI_C_HEAT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(d->arc_target, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(d->arc_target, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(d->arc_target, 3, LV_PART_KNOB);
    lv_obj_set_style_border_color(d->arc_target, UI_C_BG, LV_PART_KNOB);
    lv_obj_set_style_pad_all(d->arc_target, UI_KNOB_R - UI_DIAL_ARC_W / 2, LV_PART_KNOB);
    lv_obj_add_flag(d->arc_target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->arc_target, arc_event, LV_EVENT_PRESSED, d);
    lv_obj_add_event_cb(d->arc_target, arc_event, LV_EVENT_VALUE_CHANGED, d);
    lv_obj_add_event_cb(d->arc_target, arc_event, LV_EVENT_RELEASED, d);
    lv_obj_add_event_cb(d->arc_target, arc_event, LV_EVENT_PRESS_LOST, d);

    d->knob_core = lv_obj_create(d->arc_target);
    lv_obj_remove_style_all(d->knob_core);
    lv_obj_set_size(d->knob_core, KNOB_CORE_D, KNOB_CORE_D);
    lv_obj_set_style_radius(d->knob_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d->knob_core, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(d->knob_core, LV_OPA_COVER, 0);
    lv_obj_remove_flag(d->knob_core, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* ── Centre readout ────────────────────────────────────────────────────── */
    /* A fixed width rather than LV_SIZE_CONTENT. The target line is the widest
     * thing in here and it carries letter spacing, which a content-sized box
     * measures tightly enough to leave the last glyph against its own edge —
     * which is exactly what "TARGET 102°" looked like, clipped on the right.
     * Given a known box and centred text, there is nothing left to clip. */
    lv_obj_t *centre = lv_obj_create(ring);
    lv_obj_remove_style_all(centre);
    lv_obj_set_size(centre, UI_DIAL_SIZE - 2 * UI_DIAL_ARC_W - 8, LV_SIZE_CONTENT);
    lv_obj_center(centre);
    lv_obj_set_flex_flow(centre, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(centre, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(centre, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_obj_create(centre);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    d->lbl_temp = lv_label_create(row);
    lv_obj_set_style_text_font(d->lbl_temp, UI_FONT_TEMP_B, 0);
    lv_obj_set_style_text_color(d->lbl_temp, UI_C_TEXT, 0);
    lv_label_set_text(d->lbl_temp, "--");

    d->lbl_unit = lv_label_create(row);
    lv_obj_set_style_text_font(d->lbl_unit, UI_FONT_BODY_B, 0);
    lv_obj_set_style_text_color(d->lbl_unit, UI_C_MUTED, 0);
    lv_obj_set_style_pad_bottom(d->lbl_unit, 8, 0);
    lv_label_set_text(d->lbl_unit, "°F");

    d->lbl_target = lv_label_create(centre);
    lv_obj_set_width(d->lbl_target, LV_PCT(100));
    lv_obj_set_style_text_align(d->lbl_target, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(d->lbl_target, UI_FONT_LABEL_B, 0);
    lv_obj_set_style_text_color(d->lbl_target, UI_C_HEAT, 0);
    lv_obj_set_style_text_letter_space(d->lbl_target, 1, 0);
    lv_label_set_text(d->lbl_target, "TARGET  --°");

    /* ── Step keys ─────────────────────────────────────────────────────────── */
    lv_obj_t *steps = lv_obj_create(d->card);
    lv_obj_remove_style_all(steps);
    lv_obj_set_size(steps, LV_PCT(100), UI_STEPPER_H);
    lv_obj_remove_flag(steps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(steps, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(steps, UI_GROUP_HGAP, 0);

    s_step_down.d = d;
    s_step_down.delta = -UI_STEPPER_STEP;
    s_step_up.d = d;
    s_step_up.delta = UI_STEPPER_STEP;

    d->btn_minus = make_step_key(steps, "-", &s_step_down);
    d->btn_plus  = make_step_key(steps, "+", &s_step_up);
}

void ui_dial_update(ui_dial_t *d, int water_f, bool water_valid, bool heating,
                    lv_color_t sweep, int target_f, bool unconfirmed, bool enabled)
{
    /* Do not fight the finger: while a drag is in progress the arc already holds
     * the user's value and writing the controller's over it would snap the knob
     * back on every 5 Hz status frame. */
    if (!d->dragging) {
        if (!d->applied || d->last_target != target_f) {
            lv_arc_set_value(d->arc_target, target_f);
        }
    }
    place_knob(d);

    bool first = !d->applied;
    if (first || d->last_water != water_f || d->last_valid != water_valid) {
        if (water_valid) {
            int shown = water_f;
            if (shown < UI_DIAL_MIN_F) shown = UI_DIAL_MIN_F;
            if (shown > UI_DIAL_MAX_F) shown = UI_DIAL_MAX_F;
            lv_arc_set_value(d->arc_water, shown);
            lv_arc_set_value(d->arc_glow, shown);
            lv_label_set_text_fmt(d->lbl_temp, "%d", water_f);
            lv_obj_remove_flag(d->arc_water, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* Nothing has been reported. Show no sweep and no dot at all — an
             * arc parked at its minimum reads as "60 degrees", a lie with a
             * number on it. */
            lv_label_set_text(d->lbl_temp, "--");
            lv_obj_add_flag(d->arc_water, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (first || d->last_heating != heating || d->last_valid != water_valid ||
        !lv_color_eq(d->last_sweep, sweep)) {
        lv_obj_set_style_arc_color(d->arc_water, sweep, LV_PART_INDICATOR);
        /* The halo takes the same colour as the sweep rather than staying heat
         * orange: an orange glow under a green ring reads as two states at once.
         * Whether it is there at all is still the heater. */
        lv_obj_set_style_arc_color(d->arc_glow, sweep, LV_PART_INDICATOR);
        if (heating && water_valid) {
            lv_obj_remove_flag(d->arc_glow, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d->arc_glow, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Always shown. The panel this replaces had WATER TEMP and TARGET TEMP as
     * two columns of digits, and both get read from across the room; hiding the
     * target until a finger is on the ring, as the phone app does, would lose
     * that. While the S3 has not confirmed a dialled value the readout goes to
     * full strength, so "what I asked for" and "what the controller has" are
     * distinguishable without another label. */
    bool hot = unconfirmed || d->dragging;
    if (first || hot != d->last_unconfirmed || d->last_target != target_f) {
        lv_label_set_text_fmt(d->lbl_target, "TARGET  %d°",
                              (int)lv_arc_get_value(d->arc_target));
        lv_obj_set_style_text_opa(d->lbl_target, hot ? LV_OPA_COVER : LV_OPA_70, 0);
    }

    if (first || d->last_enabled != enabled) {
        if (enabled) {
            lv_obj_add_flag(d->btn_minus, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(d->btn_plus, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(d->arc_target, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(d->card, LV_OPA_COVER, 0);
        } else {
            /* The strip along the bottom explains this at length. */
            lv_obj_remove_flag(d->btn_minus, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(d->btn_plus, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(d->arc_target, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(d->card, LV_OPA_50, 0);
        }
    }

    d->last_water = water_f;
    d->last_valid = water_valid;
    d->last_heating = heating;
    d->last_sweep = sweep;
    d->last_target = target_f;
    d->last_unconfirmed = hot;
    d->last_enabled = enabled;
    d->applied = true;
}
