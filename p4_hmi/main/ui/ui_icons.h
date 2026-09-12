/*
 * The app's icons, as polylines.
 *
 * The iOS app draws SF Symbols — water.waves, lightbulb.fill, leaf.fill,
 * flame.fill — and LVGL's built-in symbol font has no equivalent for any of
 * them. Rather than mismatch them onto whatever glyphs do exist, each is a small
 * table of points in a 1000x1000 box, scaled into whatever size a tile asks for
 * and stroked with rounded caps. That keeps the same stroked-outline character
 * as the originals, stays crisp at any size, and leaves the shapes editable as
 * numbers in ui_icons.c rather than as a regenerated font binary.
 */
#ifndef UI_ICONS_H
#define UI_ICONS_H

#include "lvgl.h"

typedef enum {
    UI_ICON_WAVES = 0,   /* water.waves — the pumps */
    UI_ICON_BULB,        /* lightbulb.fill — the light */
    UI_ICON_LEAF,        /* leaf.fill — Eco */
    UI_ICON_FLAME,       /* flame.fill — Max Jets, and "heating" */
    UI_ICON_SUN,         /* the brightness slider in the status bar */
    /* Two opposing arrows, for the link. Stroked and in the link's own state
     * colour rather than the two-tone green/blue of the usual artwork: every
     * other icon here is a thin outline, and on this screen colour means state —
     * a permanently green arrow beside a LINK label that turns red would be
     * telling two different stories at once. */
    UI_ICON_TRANSFER,
    UI_ICON_COUNT,
} ui_icon_id_t;

/* Returns a container of lv_line children, sized `px` square. Not clickable, so
 * a press lands on the tile behind it rather than being swallowed here. */
lv_obj_t *ui_icon_create(lv_obj_t *parent, ui_icon_id_t id, int px,
                         int stroke_w, lv_color_t color);

void ui_icon_set_color(lv_obj_t *icon, lv_color_t color);

#endif /* UI_ICONS_H */
