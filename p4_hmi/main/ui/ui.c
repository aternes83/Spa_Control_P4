#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "spalink_port.h"
#include "ui_dial.h"
#include "ui_icons.h"
#include "ui_model.h"
#include "ui_theme.h"

/* If a bar height or a heading ever changes, fail here rather than ship a
 * control that a wet hand cannot reliably hit. See the note in ui_theme.h. */
_Static_assert(UI_ROW_H >= UI_TOUCH_MIN,
               "control buttons have fallen below the 9 mm wet-hand floor");

/* ── Shared state ────────────────────────────────────────────────────────────
 * Touched from the lvgl_port task (button callbacks) and from the link task
 * (ui_tick). Both hold the LVGL lock, which is the only reason none of this
 * needs a mutex of its own. */
static ui_intent_t  s_ui;
static spa_state_t  s_last;          /* the newest state, for the callbacks */
static uint32_t     s_now_ms;
static bool         s_send_setpoint;
static int          s_setpoint_f;
static ui_brightness_cb_t s_brightness_cb;

/* From the C6 and the RTC; see ui_set_radio() / ui_set_clock(). */
static bool s_wifi_up, s_bt_up;
static int  s_hour = -1, s_minute = 0;

/* ── Widgets ─────────────────────────────────────────────────────────────────
 * Each remembers what was last pushed into it. LVGL invalidates an object on
 * every write whether or not the value changed, and ui_tick() runs ten times a
 * second — writing unconditionally would redraw the whole panel ten times a
 * second to show the same picture, which is both wasteful and felt: a drag on
 * the dial has to compete with it. Nothing below is written unless it differs. */
typedef struct {
    lv_obj_t  *root;
    lv_obj_t  *icon;
    lv_obj_t  *lbl_name;
    lv_obj_t  *lbl_state;
    lv_color_t accent;
    ui_ctl_state_t last_state;
    lv_color_t last_accent;
    bool       last_enabled;
    bool       applied;
} ui_tile_t;

static lv_obj_t *s_dot, *s_lbl_link, *s_lbl_wifi, *s_lbl_bt, *s_lbl_clock;
static ui_dial_t s_dial;

static ui_tile_t s_t_pump1, s_t_pump2, s_t_pump3, s_t_light, s_t_eco, s_t_maxjet;
static lv_obj_t *s_alert, *s_alert_text;
static lv_obj_t *s_diag, *s_diag_text;


/* ── Fault text ──────────────────────────────────────────────────────────────
 * The protocol carries the safety inputs alongside the fault code precisely so
 * this screen can explain a fault rather than only name it. Somebody reading
 * "NO FLOW" at 11 p.m. in a dressing gown needs the next sentence more than the
 * code. */
static const char *fault_detail(const spa_state_t *s)
{
    switch (s->fault_code) {
    case SPA_FAULT_NO_FLOW:
        return "A pump is running but no flow. Check the filter and the valves.";
    case SPA_FAULT_HIGH_LIMIT:
        return "Reset the high-limit thermostat at the heater once it has cooled.";
    case SPA_FAULT_OVERTEMP:
        return "Above the safe limit. The heater stays off until it falls.";
    case SPA_FAULT_ESTOP:
        return "The emergency stop is open. Nothing will run until it is reset.";
    case SPA_FAULT_TEMP_SENSOR:
        return (s->safety_inputs & SPALINK_IN_TEMP_SENSOR_OK)
               ? "The water temperature sensor is reading out of range."
               : "The temperature sensor is open or shorted. Heating is disabled.";
    default:
        return "The controller has stopped the plant.";
    }
}

/* ── Change-guarded setters ─────────────────────────────────────────────── */
static void set_text(lv_obj_t *o, const char *t)
{
    if (t == NULL) {
        t = "";
    }
    const char *cur = lv_label_get_text(o);
    if (cur == NULL || strcmp(cur, t) != 0) {
        lv_label_set_text(o, t);
    }
}

static void set_text_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(o, c, 0);
    }
}

static void set_bg_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), c)) {
        lv_obj_set_style_bg_color(o, c, 0);
    }
}

/* ── Event handlers ──────────────────────────────────────────────────────────
 * Each one moves a bit in s_ui and returns. Nothing here writes to s_last, sends
 * a frame, or changes what is drawn: the next ui_tick() does all of that, from
 * the controller's answer. */

static void on_toggle(lv_event_t *e)
{
    bool *flag = lv_event_get_user_data(e);
    *flag = !*flag;
}

static void on_eco(lv_event_t *e)
{
    (void)e;
    int sp;
    if (ui_set_eco(&s_ui, !s_ui.eco, &s_last, &sp)) {
        s_send_setpoint = true;
        s_setpoint_f = sp;
    }
}

static void on_max_jet(lv_event_t *e)
{
    (void)e;
    int sp;
    if (ui_set_max_jet(&s_ui, !s_ui.max_jet, &s_last, s_now_ms, &sp)) {
        s_send_setpoint = true;
        s_setpoint_f = sp;
    }
}

static void on_pump1_cycle(lv_event_t *e)
{
    (void)e;
    s_ui.pump1 = (uint8_t)((s_ui.pump1 + 1) % 3);   /* Off -> Low -> High -> Off */
}

static void on_dial(int target_f, bool dragging)
{
    s_ui.target_local = true;
    s_ui.target_f = target_f;
    if (!dragging) {
        s_send_setpoint = true;
        s_setpoint_f = target_f;
    }
}

/* Diagnostics has no button. It is a service screen — link counters and firmware
 * versions, nothing a person in the tub ever needs — and a permanent icon in the
 * status strip would be one more thing on a line that should read at a glance.
 * Holding the link indicator opens it instead, which is also where somebody
 * would look: you press the link status to ask about the link. */
static uint32_t s_link_press_ms;

static void on_link_hold(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_link_press_ms = lv_tick_get();
    } else if (code == LV_EVENT_RELEASED &&
               lv_tick_elaps(s_link_press_ms) >= UI_SERVICE_HOLD_MS) {
        lv_obj_remove_flag(s_diag, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_diag_close(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_diag, LV_OBJ_FLAG_HIDDEN);
}

static void on_brightness(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    if (s_brightness_cb) {
        s_brightness_cb((int)lv_slider_get_value(sl));
    }
}

/* ── Small builders ─────────────────────────────────────────────────────── */

static lv_obj_t *plain(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

/* A control row. There is no heading over it: every button carries its own name,
 * so a label above was a second one for the same thing, and the height it cost
 * four times over is what the rows are made of now. */
static lv_obj_t *control_row(lv_obj_t *parent)
{
    lv_obj_t *row = plain(parent, LV_PCT(100), UI_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, UI_GROUP_HGAP, 0);
    return row;
}

static ui_tile_t tile_create(lv_obj_t *parent, ui_icon_id_t ic, const char *name,
                             lv_color_t accent, lv_event_cb_t cb, void *user_data)
{
    ui_tile_t t;
    t.accent = accent;

    t.root = lv_obj_create(parent);
    lv_obj_remove_style_all(t.root);
    lv_obj_add_style(t.root, &ui_st_tile, 0);
    lv_obj_set_height(t.root, UI_ROW_H);
    lv_obj_set_flex_grow(t.root, 1);
    lv_obj_remove_flag(t.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t.root, LV_OBJ_FLAG_CLICKABLE);
    /* Stacked, not across. With the temperature panel taking 65 % of the width
     * a paired tile is 110 px wide, which leaves about 36 px for text once the
     * icon and the padding have had their share — not enough for a name and a
     * state on one line. Icon over name over state fits 78 px with room to
     * spare: 24 + 2 + 15 + 2 + 15, inside 6 px of padding. */
    lv_obj_set_flex_flow(t.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t.root, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t.root, 2, 0);
    lv_obj_add_event_cb(t.root, cb, LV_EVENT_CLICKED, user_data);
    /* An invisible margin, so the gap between two tiles belongs to whichever is
     * nearer rather than to neither. */
    lv_obj_set_ext_click_area(t.root, UI_HIT_SLOP);

    t.icon = ui_icon_create(t.root, ic, 28, 2, UI_C_MUTED);
    t.lbl_name = label(t.root, name, UI_FONT_BODY_B, UI_C_TEXT);
    t.lbl_state = label(t.root, "Off", UI_FONT_LABEL_B, UI_C_MUTED);

    t.last_state = UI_STATE_OFF;
    t.last_accent = accent;
    t.last_enabled = true;
    t.applied = false;
    return t;
}

/* One place decides how every control looks, so a jet tile and the light cannot
 * drift apart. text_override lets a tile say something better than the generic
 * word — the light says how long it has before its run timer drops it. */
static void tile_apply(ui_tile_t *t, ui_ctl_state_t stt, bool enabled,
                       const char *text_override)
{
    /* Filled, not outlined. A 3 px border round a dark card reads as "on" only
     * once you are close enough to look for it; on a wet panel across a garden
     * the whole button has to change.
     *
     * The fill is the control's colour mixed down over the screen's ground, and
     * the text on it is white. At full strength white measures under 2:1 on
     * every one of these accents — the yellow is 1.4:1 — so the colour that
     * stays at full strength is the border, where nothing has to be read
     * against it. The result is a vivid ring, a deep fill of the same hue, and
     * white that is actually legible on it. */
    lv_color_t fill   = UI_C_CARD;
    lv_color_t edge   = t->accent;
    lv_opa_t   edge_o = LV_OPA_TRANSP;
    lv_color_t ink    = UI_C_MUTED;   /* icon and state text */
    lv_color_t name   = UI_C_TEXT;
    const char *text  = "Off";

    switch (stt) {
    case UI_STATE_ON:
        fill = lv_color_mix(t->accent, UI_C_BG, UI_FILL_MIX);
        edge_o = LV_OPA_COVER;
        ink = lv_color_white();
        name = lv_color_white();
        text = "On";
        break;
    case UI_STATE_PENDING:
        /* Asked for, not yet confirmed. Outlined only: the fill belongs to loads
         * the controller has actually reported running. */
        edge_o = LV_OPA_60;
        ink = t->accent;
        text = "Starting";
        break;
    case UI_STATE_REFUSED:
        /* Asked for, and still not running — the one a user needs to notice
         * without hunting for it, so it fills too, in amber. */
        fill = lv_color_mix(UI_C_STALE, UI_C_BG, UI_FILL_MIX);
        edge = UI_C_STALE;
        edge_o = LV_OPA_COVER;
        ink = lv_color_white();
        name = lv_color_white();
        text = "Refused";
        break;
    case UI_STATE_OFF:
    default:
        break;
    }

    set_text(t->lbl_state, text_override ? text_override : text);

    if (t->applied && t->last_state == stt && t->last_enabled == enabled &&
        lv_color_eq(t->last_accent, t->accent)) {
        return;
    }
    t->last_state = stt;
    t->last_accent = t->accent;
    t->last_enabled = enabled;
    t->applied = true;

    set_bg_color(t->root, fill);
    ui_icon_set_color(t->icon, ink);
    set_text_color(t->lbl_state, ink);
    set_text_color(t->lbl_name, name);
    lv_obj_set_style_border_color(t->root, edge, 0);
    lv_obj_set_style_border_opa(t->root, edge_o, 0);
    lv_obj_set_style_opa(t->root, enabled ? LV_OPA_COVER : LV_OPA_50, 0);
    if (enabled) {
        lv_obj_add_flag(t->root, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(t->root, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ── The status strip ────────────────────────────────────────────────────── */

static void build_topbar(lv_obj_t *scr)
{
    lv_obj_t *bar = plain(scr, UI_SCREEN_W, UI_TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_C_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
    lv_obj_set_style_pad_hor(bar, UI_GUTTER, 0);

    /* Brightness, on the left, exactly where v2.0 put it. The track is thin
     * because a fat one looks clumsy, but the touch area is not: the slider
     * carries enough slop to be grabbed at 46 px, which is what the old panel
     * was doing when it extended this control's hit box down past its bezel. */
    lv_obj_t *sun = ui_icon_create(bar, UI_ICON_SUN, 20, 2, UI_C_MUTED);
    lv_obj_align(sun, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *slider = lv_slider_create(bar);
    lv_obj_set_size(slider, 190, 10);
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 32, 0);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, UI_C_CARD, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, UI_C_WATER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, UI_C_WATER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 9, LV_PART_KNOB);      /* a 28 px knob */
    lv_obj_set_ext_click_area(slider, 18);
    lv_obj_add_event_cb(slider, on_brightness, LV_EVENT_VALUE_CHANGED, NULL);

    /* Right: the link, the two radios, the clock, and the way into diagnostics. */
    lv_obj_t *right = plain(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 14, 0);

    s_lbl_bt    = label(right, LV_SYMBOL_BLUETOOTH, UI_FONT_GLYPH_BT, UI_C_MUTED);
    s_lbl_wifi  = label(right, LV_SYMBOL_WIFI, UI_FONT_GLYPH_WIFI, UI_C_MUTED);

    /* The link sits between the radios and the clock: it belongs with the other
     * things that are either connected or not. Holding it opens diagnostics. */
    lv_obj_t *linkgrp = lv_obj_create(right);
    lv_obj_remove_style_all(linkgrp);
    lv_obj_set_size(linkgrp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(linkgrp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(linkgrp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(linkgrp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(linkgrp, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(linkgrp, 6, 0);
    lv_obj_set_ext_click_area(linkgrp, 14);
    lv_obj_add_event_cb(linkgrp, on_link_hold, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(linkgrp, on_link_hold, LV_EVENT_RELEASED, NULL);

    /* 22 px, not smaller: at 18 the 2 px stroke closes the shafts into solid
     * bars and the arrows stop reading as arrows. */
    s_dot = ui_icon_create(linkgrp, UI_ICON_TRANSFER, 22, 2, UI_C_FAULT);
    s_lbl_link = label(linkgrp, "LINK", UI_FONT_CAPTION, UI_C_FAULT);
    lv_obj_set_style_text_letter_space(s_lbl_link, 1, 0);

    s_lbl_clock = label(right, "--:--", UI_FONT_BODY_B, UI_C_TEXT);
}

/* ── The alert ───────────────────────────────────────────────────────────────
 * What the old bottom strip carried that nothing else did was the sentence: the
 * fault's explanation, and how long the link has been dead. Its four indicators
 * each repeated something a control already said, so they are gone and their
 * 40 px went to the dial — but the sentence still needs somewhere, and it needs
 * to be impossible to miss when it appears.
 *
 * So it is an overlay across the foot of the screen, present only when there is
 * something to say. It costs no layout space the rest of the time, and when it
 * does appear it covers the bottom of both columns, which during a fault is
 * exactly right: nothing under it will be honoured anyway. */
static void build_alert(lv_obj_t *scr)
{
    s_alert = lv_obj_create(scr);
    lv_obj_remove_style_all(s_alert);
    lv_obj_set_size(s_alert, UI_SCREEN_W, LV_SIZE_CONTENT);
    lv_obj_align(s_alert, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_alert, UI_C_FAULT, 0);
    lv_obj_set_style_pad_all(s_alert, 10, 0);
    lv_obj_set_style_pad_hor(s_alert, UI_GUTTER, 0);
    lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);

    s_alert_text = label(s_alert, "", UI_FONT_LABEL_B, lv_color_white());
    lv_obj_set_width(s_alert_text, LV_PCT(100));
    lv_label_set_long_mode(s_alert_text, LV_LABEL_LONG_WRAP);
}

static void build_diag(lv_obj_t *scr)
{
    s_diag = lv_obj_create(scr);
    lv_obj_remove_style_all(s_diag);
    lv_obj_set_size(s_diag, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_diag, 0, 0);
    lv_obj_set_style_bg_color(s_diag, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_diag, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_diag, UI_GUTTER, 0);
    lv_obj_remove_flag(s_diag, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_diag, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_diag, 12, 0);
    lv_obj_add_flag(s_diag, LV_OBJ_FLAG_HIDDEN);

    label(s_diag, "Diagnostics", UI_FONT_TITLE, UI_C_TEXT);

    lv_obj_t *card = lv_obj_create(s_diag);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &ui_st_card, 0);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_diag_text = label(card, "", UI_FONT_CAPTION, UI_C_MUTED);

    lv_obj_t *close = lv_button_create(s_diag);
    lv_obj_set_size(close, LV_PCT(100), UI_ROW_H);
    lv_obj_set_style_bg_color(close, UI_C_CARD, 0);
    lv_obj_set_style_radius(close, UI_RADIUS_TILE, 0);
    lv_obj_add_event_cb(close, on_diag_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = label(close, "Close", UI_FONT_BODY, UI_C_TEXT);
    lv_obj_center(cl);
}

/* ── The control side ────────────────────────────────────────────────────── */

static void build_controls(lv_obj_t *parent)
{
    lv_obj_t *col = plain(parent, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    /* Centred on the main axis. The four groups are sized to fill the column
     * exactly, but rounding in the arithmetic above can leave a pixel or two,
     * and it should be split top and bottom rather than dumped at the foot where
     * it reads as a mistake against the dial beside it. */
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, UI_GROUP_VGAP, 0);

    /* Jet 1 is one button the size of the Light one, cycling Off -> Low -> High.
     * Three segments across a 272 px column made each one small and the row odd
     * against the three below it; one target the size of every other is both
     * easier to hit wet and simpler to read. The cost is that reaching High from
     * Off takes two presses, which on a control used a few times a session is a
     * fair trade for a target twice the size. */
    lv_obj_t *r1 = control_row(col);
    s_t_pump1 = tile_create(r1, UI_ICON_WAVES, "Jet 1", UI_C_WATER,
                            on_pump1_cycle, NULL);

    lv_obj_t *r2 = control_row(col);
    s_t_pump2 = tile_create(r2, UI_ICON_WAVES, "Jet 2", UI_C_WATER,
                            on_toggle, &s_ui.pump2);
    s_t_pump3 = tile_create(r2, UI_ICON_WAVES, "Jet 3", UI_C_WATER,
                            on_toggle, &s_ui.pump3);

    lv_obj_t *r3 = control_row(col);
    s_t_light = tile_create(r3, UI_ICON_BULB, "Light", UI_C_LIGHT,
                            on_toggle, &s_ui.light);

    lv_obj_t *r4 = control_row(col);
    s_t_eco    = tile_create(r4, UI_ICON_LEAF,  "Eco", UI_C_GOOD,
                             on_eco, NULL);
    s_t_maxjet = tile_create(r4, UI_ICON_FLAME, "Max Jets", UI_C_HEAT,
                             on_max_jet, NULL);
}

void ui_init(ui_brightness_cb_t brightness_cb)
{
    s_brightness_cb = brightness_cb;
    ui_intent_init(&s_ui);
    spa_state_init(&s_last);
    ui_theme_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &ui_st_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_topbar(scr);

    lv_obj_t *content = plain(scr, UI_SCREEN_W,
                              UI_SCREEN_H - UI_TOPBAR_H - UI_STATUSBAR_H);
    lv_obj_set_pos(content, 0, UI_TOPBAR_H);
    lv_obj_set_style_pad_hor(content, UI_GUTTER, 0);
    lv_obj_set_style_pad_ver(content, UI_CONTENT_PAD, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(content, UI_CARD_GAP, 0);

    lv_obj_t *left = plain(content, UI_COL_LEFT, LV_PCT(100));
    ui_dial_create(&s_dial, left, on_dial);

    build_controls(content);
    build_alert(scr);
    build_diag(scr);
}

/* ── The per-tick redraw ─────────────────────────────────────────────────── */

void ui_set_radio(bool wifi_up, bool bt_up) { s_wifi_up = wifi_up; s_bt_up = bt_up; }
void ui_set_clock(int hour, int minute)
{
    /* Clamped here rather than trusted: this is fed from whatever time source
     * turns up, and a wild value would otherwise reach a fixed-size buffer. */
    s_hour = (hour < 0) ? -1 : (hour % 24);
    s_minute = (minute < 0 || minute > 59) ? 0 : minute;
}

/* The link indicator, in the header between the radios and the clock. Icon and
 * label share one colour so the mark and the word always say the same thing. */
static void update_link(const spa_state_t *s)
{
    lv_color_t c;
    char text[20];          /* "LINK " plus the longest ui_format_duration() */

    if (!s->ever_connected) {
        c = UI_C_STALE;
        lv_strlcpy(text, "LINK", sizeof(text));
    } else if (!s->link_up) {
        c = UI_C_FAULT;
        char age[12];
        ui_format_duration(s->stale_ms / 1000u, age, sizeof(age));
        snprintf(text, sizeof(text), "LINK %s", age);
    } else {
        c = UI_C_GOOD;
        lv_strlcpy(text, "LINK", sizeof(text));
    }
    lv_color_t link_c = c;
    static lv_color_t last_link_c;
    static bool link_c_applied;
    if (!link_c_applied || !lv_color_eq(last_link_c, link_c)) {
        ui_icon_set_color(s_dot, link_c);
        last_link_c = link_c;
        link_c_applied = true;
    }
    set_text_color(s_lbl_link, link_c);
    set_text(s_lbl_link, text);
}

static void update_topbar(void)
{
    set_text_color(s_lbl_wifi, s_wifi_up ? UI_C_WATER : UI_C_MUTED);
    set_text_color(s_lbl_bt,   s_bt_up   ? UI_C_WATER : UI_C_MUTED);

    char clock[16];
    if (s_hour < 0) {
        lv_strlcpy(clock, "--:--", sizeof(clock));
    } else {
        snprintf(clock, sizeof(clock), "%d:%02d", s_hour, s_minute);
    }
    set_text(s_lbl_clock, clock);
}

void ui_tick(const spa_state_t *s, uint32_t now_ms, ui_out_t *out)
{
    s_last = *s;
    s_now_ms = now_ms;

    ui_target_settle(&s_ui, s);
    ui_intent_tick(&s_ui, now_ms);

    /* Nothing in the control column may be touched while the controller is not
     * answering. A press we cannot deliver is worse than no button at all: it
     * looks like it worked. */
    const bool live = s->link_up;
    const bool heating = (s->outputs & SPALINK_OUT_HEATER) != 0;

    update_topbar();
    update_link(s);

    /* ── Dial ────────────────────────────────────────────────────────────── */
    /* Mode first, then the heater, then plain water. A mode is a thing the user
     * chose and is still running; it outranks a thermostat that comes and goes
     * on its own. */
    lv_color_t sweep = s_ui.max_jet ? UI_C_HEAT
                     : s_ui.eco     ? UI_C_GOOD
                     : heating      ? UI_C_HEAT
                                    : UI_C_WATER;
    ui_dial_update(&s_dial, spa_state_water_f(s), s->have_temp, heating,
                   sweep, ui_target_f(&s_ui, s), s_ui.target_local, live);

    /* ── JET 1 ───────────────────────────────────────────────────────────── */
    const char *lock_why = NULL;
    bool locked = ui_pumps_locked(&s_ui, &lock_why);
    bool pumps_live = live && !locked;

    /* While a mode owns the pumps, their tiles wear its colour instead of water.
     * The jets are three of the six tiles and sit together, so that plus the
     * ring is most of the colour on the glass — which is the point: what the
     * panel is predominantly showing should be the mode that is running. */
    lv_color_t pump_c = locked ? (s_ui.max_jet ? UI_C_HEAT : UI_C_GOOD) : UI_C_WATER;
    s_t_pump1.accent = pump_c;
    s_t_pump2.accent = pump_c;
    s_t_pump3.accent = pump_c;

    ui_ctl_state_t p1 = ui_control_state(&s_ui, s, UI_CTL_PUMP1, now_ms);
    bool p1_high = (s->outputs & SPALINK_OUT_PUMP1_HIGH) != 0;
    /* Running: say what the plant is actually doing, which is not always what
     * was selected — the S3 runs Jet 1 for the thermostat, and then this reads
     * Low against an Off selection. Otherwise say what was selected. */
    static const char *SPEED[3] = { "Off", "Low", "High" };
    const char *p1_text = locked            ? lock_why
                        : p1 == UI_STATE_ON ? (p1_high ? "High" : "Low")
                                            : SPEED[s_ui.pump1 % 3];
    tile_apply(&s_t_pump1, p1, pumps_live, p1_text);

    tile_apply(&s_t_pump2, ui_control_state(&s_ui, s, UI_CTL_PUMP2, now_ms),
               pumps_live, NULL);
    tile_apply(&s_t_pump3, ui_control_state(&s_ui, s, UI_CTL_PUMP3, now_ms),
               pumps_live, NULL);

    /* ── LIGHT and MODES ─────────────────────────────────────────────────── */
    char light_text[12];
    const char *light_label = NULL;
    ui_ctl_state_t lst = ui_control_state(&s_ui, s, UI_CTL_LIGHT, now_ms);
    if (lst == UI_STATE_ON && s->light_remain_s > 0) {
        ui_format_duration(s->light_remain_s, light_text, sizeof(light_text));
        light_label = light_text;
    } else if (lst == UI_STATE_REFUSED && s->light_remain_s == 0) {
        /* The light has its own run timer on the S3. Once it expires the
         * controller drops the load while our request is still standing, which
         * is a timeout rather than a refusal and should not read as a fault. */
        light_label = "Timed out";
    }
    tile_apply(&s_t_light, lst, live, light_label);

    /* Eco and Max Jets are the UI's own state — the controller has no opinion
     * about them, so they are the only two tiles drawn from intent. */
    char jet_text[12];
    const char *jet_on = NULL;
    uint32_t jet_left = ui_max_jet_remain_s(&s_ui, now_ms);
    if (jet_left > 0) {
        ui_format_duration(jet_left, jet_text, sizeof(jet_text));
        jet_on = jet_text;
    }
    tile_apply(&s_t_eco, s_ui.eco ? UI_STATE_ON : UI_STATE_OFF,
               live && !s_ui.max_jet, NULL);
    tile_apply(&s_t_maxjet, s_ui.max_jet ? UI_STATE_ON : UI_STATE_OFF,
               live && !s_ui.eco, jet_on);

    /* ── The alert ───────────────────────────────────────────────────────────
     * Shown only when there is something to say, and nothing is more urgent than
     * a fault. The link's own state also reads in the header regardless of this,
     * so a dead link is never announced only here. */
    char msg[160];
    if (s->fault_active) {
        snprintf(msg, sizeof(msg), "FAULT - %s.  %s",
                 spa_fault_text(s->fault_code), fault_detail(s));
        set_bg_color(s_alert, UI_C_FAULT);
        set_text_color(s_alert_text, lv_color_white());
        set_text(s_alert_text, msg);
        lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    } else if (!s->ever_connected) {
        set_bg_color(s_alert, UI_C_CARD);
        set_text_color(s_alert_text, UI_C_MUTED);
        set_text(s_alert_text, "Waiting for the control board.");
        lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    } else if (!live) {
        ui_format_duration(s->stale_ms / 1000u, jet_text, sizeof(jet_text));
        snprintf(msg, sizeof(msg),
                 "No link for %s. Everything shown is the last thing the "
                 "controller said, not what it is doing now.", jet_text);
        set_bg_color(s_alert, UI_C_STALE);
        set_text_color(s_alert_text, lv_color_black());
        set_text(s_alert_text, msg);
        lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    }

    /* ── Diagnostics ─────────────────────────────────────────────────────── */
    if (!lv_obj_has_flag(s_diag, LV_OBJ_FLAG_HIDDEN)) {
        uint32_t bad_crc = 0, bad_len = 0, frames = 0;
        spalink_port_stats(&bad_crc, &bad_len, &frames);
        lv_label_set_text_fmt(s_diag_text,
                              "Link        %s\n"
                              "Silent for  %lu ms\n"
                              "Frames in   %lu\n"
                              "Bad CRC     %lu\n"
                              "Bad length  %lu\n"
                              "Peer proto  %u\n"
                              "Peer fw     %u.%u\n"
                              "Requests    0x%02x\n"
                              "Modes       0x%02x\n"
                              "Outputs     0x%02x\n"
                              "Inputs      0x%02x",
                              s->link_up ? "up" : "DOWN",
                              (unsigned long)s->stale_ms,
                              (unsigned long)frames,
                              (unsigned long)bad_crc,
                              (unsigned long)bad_len,
                              s->peer_proto, s->peer_fw_major, s->peer_fw_minor,
                              ui_requests(&s_ui), ui_modes(&s_ui),
                              s->outputs, s->safety_inputs);
    }

    /* ── What to put on the wire ─────────────────────────────────────────── */
    out->requests = ui_requests(&s_ui);
    out->modes = ui_modes(&s_ui);
    out->send_setpoint = s_send_setpoint;
    out->setpoint_f = s_setpoint_f;
    s_send_setpoint = false;
}
