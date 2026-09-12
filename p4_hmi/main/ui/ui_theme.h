/*
 * The SpaControl look, ported from the iOS app.
 *
 * The palette is Theme.swift verbatim, and the card geometry is the app's
 * (16 px radius on the dial card, 12 px elsewhere, a 1.5 px active border in the
 * control's own colour). The one thing that is *not* a straight copy is size:
 * the phone works in points at ~163 ppi and this panel is 480x800 over 4.3", so
 * ~218 ppi. A point here is about 1.33 px if a control is to stay the same size
 * under a wet thumb, and every dimension below is scaled that way rather than
 * copied as a number.
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

/* ── Palette — Theme.swift ───────────────────────────────────────────────── */
#define UI_C_WATER   lv_color_hex(0x40D4E2)   /* cool water / interactive */
#define UI_C_HEAT    lv_color_hex(0xFF9F45)   /* heat / temperature / target */
#define UI_C_GOOD    lv_color_hex(0x3ED67F)   /* connected / ok */
#define UI_C_STALE   lv_color_hex(0xFFC24B)   /* stale feed */
#define UI_C_FAULT   lv_color_hex(0xFF5A5F)   /* fault */
#define UI_C_MUTED   lv_color_hex(0x8CA1B4)   /* secondary text */
#define UI_C_CARD    lv_color_hex(0x243347)   /* Color(0.14, 0.20, 0.28) */
#define UI_C_BG      lv_color_hex(0x1A2638)   /* Color(0.10, 0.15, 0.22) */
#define UI_C_TEXT    lv_color_hex(0xFFFFFF)
#define UI_C_LIGHT   lv_color_hex(0xFFD43B)   /* the light's own yellow */

/* ── Touch sizing ────────────────────────────────────────────────────────────
 * This panel is worked with wet hands, which is a harder problem than a phone in
 * a dry one. A wet fingertip spreads, so the contact patch is larger and less
 * certain than the pointer the user thinks they are aiming, and they cannot see
 * what is under it. Phone guidance lands around 7 mm; that is not enough here.
 *
 * The rule: anything that moves the plant gets at least 9 mm in its smallest
 * dimension. At 218 ppi one pixel is 0.117 mm, so 9 mm is 77 px — hence 78.
 * Every control on this screen now clears it; the brightness slider does so
 * through its slop rather than its track.
 */
/* How much of a control's own colour goes into its filled state. White text on
 * the accents at full strength measures 1.4-2.0:1, which is under the 4.5:1
 * floor and, on the yellow, close to unreadable. Mixed this far over the screen
 * ground every accent clears 4.5:1 with white on top. The full-strength colour
 * is not lost — it goes to the border, where nothing has to be legible against
 * it, so the tile still reads as that control's colour from across the room. */
#define UI_FILL_MIX      115     /* of 255: about 45 % accent, 55 % ground */

#define UI_TOUCH_MIN     78      /* 9.1 mm — the floor for any plant control */
#define UI_HIT_SLOP      6       /* invisible margin around a target, each side */
#define UI_DIAL_DEADZONE 56      /* radius of the dial centre that ignores a press */
/* Diagnostics has no button of its own — see the note in ui.c. This is how long
 * the link indicator has to be held to open it: long enough that no ordinary
 * press reaches it by accident, short enough to be a gesture rather than a
 * puzzle. LVGL's own long-press fires at 400 ms, which is well inside the time a
 * wet finger rests on a control it is already pressing. */
#define UI_SERVICE_HOLD_MS 1500

/* ── Metrics ─────────────────────────────────────────────────────────────────
 * The panel is 480x800 in the glass and driven rotated, so the UI is 800x480.
 *
 * The arrangement is the v2.0 HMI's, which has been on the wall of this tub for
 * a year: a thin status strip at the top carrying brightness and the radios, the
 * temperature panel on the left, the four control groups stacked down the right
 * under their own headings, and an indicator strip along the bottom. What
 * changed is the left panel — where v2.0 had two columns of seven-segment digits
 * and a pair of +/- buttons, there is now one dial you turn.
 *
 *   0          40                                                      800
 *   ┌────────────────────────────────────────────────────────────────────┐ 0
 *   │ brightness                          BT   wifi   ⇄ LINK      clock  │
 *   ├──────────────────────────────────────────┬─────────────────────────┤ 40
 *   │  WATER TEMP                              │ ≋ Jet 1             Low │
 *   │              ╭──────────────╮            │ ≋ Jet 2   │  ≋ Jet 3    │
 *   │             │      96        │           │ ☀ Light                 │
 *   │             │  TARGET 102°   │           │ 🌿 Eco    │  🔥 Max Jets │
 *   │              ╰──────────────╯            │                         │
 *   │  ┌──────────────┬──────────────┐         │                         │
 *   │  │       -      │      +       │         │                         │
 *   │  └──────────────┴──────────────┘         │                         │
 *   └──────────────────────────────────────────┴─────────────────────────┘ 480
 *
 * There is no strip along the bottom. The indicators that were there — heat,
 * jets, light, fault — each duplicated something a control already said, and the
 * 40 px they cost is worth more to the dial, which is now 280 px. What they did
 * carry that nothing else did is the *sentence* for a fault or a dead link, and
 * that moved to an alert that covers the foot of the screen only when there is
 * something to say. See s_alert in ui.c.
 *
 * Landscape buys the thing portrait could not: it all fits at once. Nothing
 * scrolls, which matters more here than any button size — scrolling to reach the
 * light with a wet hand is a worse interaction than a small target is.
 */
#define UI_SCREEN_W      800
#define UI_SCREEN_H      480
#define UI_TOPBAR_H      40
/* No bottom strip. The alert overlay takes its place, and only when needed. */
#define UI_STATUSBAR_H   0
#define UI_GUTTER        16
#define UI_CARD_GAP      16
#define UI_CARD_PAD      12
#define UI_RADIUS_CARD   16
#define UI_RADIUS_TILE   12
/* The temperature panel takes 60 % of the width. The dial is bounded by height,
 * not width, so the spare width goes to the ring's surroundings rather than the
 * ring: the 60 and 110 end labels sit either side of it instead of beneath, and
 * the step keys below it run the full width of the card. */
#define UI_COL_LEFT      ((UI_SCREEN_W * 60) / 100)

/* The control side: four rows, no headings over them. The buttons carry their
 * own names, so a heading above each was a second label for the same thing —
 * and the 16 px it cost, four times over, is what lets the rows be 91 px.
 *
 * Every row is one or two tiles of the same height, Jet 1 included: it cycles
 * Off -> Low -> High on press rather than offering three small segments, so no
 * row has a target smaller than any other. */
#define UI_CONTENT_H     (UI_SCREEN_H - UI_TOPBAR_H - UI_STATUSBAR_H)
#define UI_CONTENT_PAD   6
#define UI_GROUP_VGAP    8
#define UI_GROUP_HGAP    12
/* Whatever the strips leave, split four ways. ui.c asserts at compile time that
 * this clears UI_TOUCH_MIN, so trimming a strip fails the build rather than
 * quietly shipping a control nobody can hit with a wet hand. */
#define UI_ROW_H         ((UI_CONTENT_H - 2 * UI_CONTENT_PAD - 3 * UI_GROUP_VGAP) / 4)

/* ── The dial ────────────────────────────────────────────────────────────────
 * The knob rides the centre of the arc's stroke, so it reaches
 * (size - arc_w)/2 + knob_r from the middle while the widget's own half-width is
 * only size/2. It fits without overhanging only when knob_r <= arc_w/2 — and at
 * r=22 against a 24 px stroke it overhung by 10 px, which is precisely what was
 * clipped at 3 and 9 o'clock, where the overhang points straight at the edge.
 *
 * So the arc sits *inside* a box that is larger by the overhang, and the box is
 * what the column actually leaves. The knob is 19, which is also the size the
 * iOS app's works out to at this panel's density.
 */
#define UI_DIAL_HEAD_H   16
#define UI_DIAL_GAP      8
#define UI_DIAL_BOX      (UI_CONTENT_H - 2 * UI_CONTENT_PAD - 2 * UI_CARD_PAD \
                          - UI_DIAL_HEAD_H - 2 * UI_DIAL_GAP - UI_STEPPER_H)
#define UI_DIAL_ARC_W    24
#define UI_KNOB_R        19      /* an indicator, not a target: the ring drags */
#define UI_DIAL_OVERHANG (UI_KNOB_R - UI_DIAL_ARC_W / 2)
#define UI_DIAL_SIZE     (UI_DIAL_BOX - 2 * UI_DIAL_OVERHANG)

/* The step keys. Dragging a ring is a fine gesture and not everyone wants it —
 * wet hands, cold hands, or simply preferring a button that moves one degree at
 * a time. These do exactly that, and hold to repeat. Full targets like any other
 * plant control, not a pair of small chevrons tucked under the dial. */
#define UI_STEPPER_H     UI_TOUCH_MIN
#define UI_STEPPER_STEP  1       /* °F per press */

/* Fonts. The big readout wants ~82 px to match the phone's 62 pt; LVGL's stock
 * Montserrat stops at 48, which is the largest that needs no font generation
 * step in the build. Swap UI_FONT_TEMP for a generated face when one is added —
 * nothing else in the UI depends on the size. */
#define UI_FONT_TEMP     (&lv_font_montserrat_48)
#define UI_FONT_TITLE    (&lv_font_montserrat_20)
#define UI_FONT_BODY     (&lv_font_montserrat_16)
#define UI_FONT_LABEL    (&lv_font_montserrat_14)
#define UI_FONT_CAPTION  (&lv_font_montserrat_12)

/* ── Bold ────────────────────────────────────────────────────────────────────
 * LVGL's stock Montserrat is one weight — Medium — so there is no bold to ask
 * for. A bold face has to be generated and added to the build:
 *
 *   lv_font_conv --font Montserrat-Bold.ttf --size 48 --bpp 4 --format lvgl \
 *                --range 0x20-0x7F,0xB0 -o ui_font_bold_48.c
 *
 * and the same for 16, 14 and 12. Until those land these resolve to the Medium
 * faces, so the layout is already final and the only thing that changes when the
 * files appear is the weight: set UI_FONT_BOLD to 1, add the sources to
 * main/CMakeLists.txt, and every label below picks them up.
 *
 * The readouts in the dial and the names on the buttons are the two places it is
 * asked for — the things read from across a room, as against the state text
 * beside them, which is read when you are already looking. */
/* The two radio glyphs come out of the same symbol font but are not drawn to the
 * same height in it: its bluetooth is a tall narrow mark and its wifi a wide
 * short one, roughly two thirds the height. Set at one size the wifi reads as
 * half-lit beside the clock, so they are sized separately — the ratio below is
 * the correction, and is the one thing here worth checking with your own eyes on
 * the panel, since it balances two glyphs against each other rather than against
 * a number. */
#define UI_FONT_GLYPH_BT   UI_FONT_BODY     /* 16 px */
#define UI_FONT_GLYPH_WIFI UI_FONT_TITLE    /* 20 px, for a mark ~0.65x as tall */

#define UI_FONT_BOLD 0

#if UI_FONT_BOLD
LV_FONT_DECLARE(ui_font_bold_48);
LV_FONT_DECLARE(ui_font_bold_16);
LV_FONT_DECLARE(ui_font_bold_14);
LV_FONT_DECLARE(ui_font_bold_12);
#define UI_FONT_TEMP_B    (&ui_font_bold_48)
#define UI_FONT_BODY_B    (&ui_font_bold_16)
#define UI_FONT_LABEL_B   (&ui_font_bold_14)
#define UI_FONT_CAPTION_B (&ui_font_bold_12)
#else
#define UI_FONT_TEMP_B    UI_FONT_TEMP
#define UI_FONT_BODY_B    UI_FONT_BODY
#define UI_FONT_LABEL_B   UI_FONT_LABEL
#define UI_FONT_CAPTION_B UI_FONT_CAPTION
#endif

/* Shared styles, initialised once by ui_theme_init(). Per-control colour is set
 * as a local style on the object, because each tile's accent follows what it
 * controls — water for the pumps, yellow for the light, green for Eco, orange
 * for Max Jets — exactly as in the app. */
extern lv_style_t ui_st_screen;
extern lv_style_t ui_st_card;       /* the dial card: 16 px radius */
extern lv_style_t ui_st_tile;       /* the control tiles: 12 px */
extern lv_style_t ui_st_section;    /* the "PUMPS" / "FEATURES" headings */
extern lv_style_t ui_st_caption;
extern lv_style_t ui_st_banner;

void ui_theme_init(void);

#endif /* UI_THEME_H */
