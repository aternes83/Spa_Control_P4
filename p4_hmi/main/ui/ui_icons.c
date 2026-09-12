#include "ui_icons.h"

/* One stroke of a glyph: x,y pairs in a 1000x1000 box, y downwards. */
typedef struct {
    const int16_t *pts;
    uint8_t        n;
} ui_stroke_t;

typedef struct {
    const ui_stroke_t *strokes;
    uint8_t            n;
} ui_glyph_t;

/* ── The shapes ──────────────────────────────────────────────────────────────
 * Generated as sampled circles and sines rather than eyeballed, so they stay
 * symmetrical: three sine periods for the waves, a circle plus three bands for
 * the bulb, two 90 degree arcs meeting at a tip for the leaf, a hand-drawn
 * silhouette for the flame, and the usual broken ring for power.
 */
static const int16_t G_waves_0[] = { 0, 150, 125, 86, 250, 60, 375, 86, 500, 150, 625, 214, 750, 240, 875, 214, 1000, 150 };
static const int16_t G_waves_1[] = { 0, 490, 125, 426, 250, 400, 375, 426, 500, 490, 625, 554, 750, 580, 875, 554, 1000, 490 };
static const int16_t G_waves_2[] = { 0, 830, 125, 766, 250, 740, 375, 766, 500, 830, 625, 894, 750, 920, 875, 894, 1000, 830 };
static const ui_stroke_t G_waves[] = {
    { G_waves_0, 9 },
    { G_waves_1, 9 },
    { G_waves_2, 9 },
};

static const int16_t G_bulb_0[] = { 800, 380, 760, 230, 650, 120, 500, 80, 350, 120, 240, 230, 200, 380, 240, 530, 350, 640, 500, 680, 650, 640, 760, 530, 800, 380 };
static const int16_t G_bulb_1[] = { 370, 690, 630, 690 };
static const int16_t G_bulb_2[] = { 390, 800, 610, 800 };
static const int16_t G_bulb_3[] = { 430, 910, 570, 910 };
static const ui_stroke_t G_bulb[] = {
    { G_bulb_0, 13 },
    { G_bulb_1, 2 },
    { G_bulb_2, 2 },
    { G_bulb_3, 2 },
};

static const int16_t G_leaf_0[] = { 150, 850, 163, 713, 203, 582, 268, 461, 355, 355, 461, 268, 582, 203, 713, 163, 850, 150, 837, 287, 797, 418, 732, 539, 645, 645, 539, 732, 418, 797, 287, 837, 150, 850 };
static const int16_t G_leaf_1[] = { 200, 800, 800, 200 };
static const ui_stroke_t G_leaf[] = {
    { G_leaf_0, 17 },
    { G_leaf_1, 2 },
};

static const int16_t G_flame_0[] = { 500, 960, 215, 800, 150, 520, 300, 565, 330, 250, 560, 40, 520, 330, 700, 250, 835, 520, 790, 800, 500, 960 };
static const ui_stroke_t G_flame[] = {
    { G_flame_0, 11 },
};

static const int16_t G_sun_0[] = { 730, 500, 699, 385, 615, 301, 500, 270, 385, 301, 301, 385, 270, 500, 301, 615, 385, 699, 500, 730, 615, 699, 699, 615, 730, 500 };
static const int16_t G_sun_1[] = { 840, 500, 970, 500 };
static const int16_t G_sun_2[] = { 740, 260, 832, 168 };
static const int16_t G_sun_3[] = { 500, 160, 500, 30 };
static const int16_t G_sun_4[] = { 260, 260, 168, 168 };
static const int16_t G_sun_5[] = { 160, 500, 30, 500 };
static const int16_t G_sun_6[] = { 260, 740, 168, 832 };
static const int16_t G_sun_7[] = { 500, 840, 500, 970 };
static const int16_t G_sun_8[] = { 740, 740, 832, 832 };
static const ui_stroke_t G_sun[] = {
    { G_sun_0, 13 },
    { G_sun_1, 2 },
    { G_sun_2, 2 },
    { G_sun_3, 2 },
    { G_sun_4, 2 },
    { G_sun_5, 2 },
    { G_sun_6, 2 },
    { G_sun_7, 2 },
    { G_sun_8, 2 },
};

static const int16_t G_transfer_0[] = { 340, 40, 340, 190, 960, 190, 960, 440, 40, 440, 340, 40 };
static const int16_t G_transfer_1[] = { 660, 960, 660, 810, 40, 810, 40, 560, 960, 560, 660, 960 };
static const ui_stroke_t G_transfer[] = {
    { G_transfer_0, 6 },
    { G_transfer_1, 6 },
};

static const ui_glyph_t GLYPHS[UI_ICON_COUNT] = {
    [UI_ICON_WAVES] = { G_waves, 3 },
    [UI_ICON_BULB]  = { G_bulb,  4 },
    [UI_ICON_LEAF]  = { G_leaf,  2 },
    [UI_ICON_FLAME] = { G_flame, 1 },
    [UI_ICON_SUN]   = { G_sun,   9 },
    [UI_ICON_TRANSFER] = { G_transfer, 2 },
};

lv_obj_t *ui_icon_create(lv_obj_t *parent, ui_icon_id_t id, int px,
                         int stroke_w, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, px, px);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if ((unsigned)id >= (unsigned)UI_ICON_COUNT) {
        return box;
    }

    /* Inset by half the stroke so a rounded cap on the outermost point is not
     * clipped by the box, which is what makes the waves look chopped at the
     * edges if you do not. */
    const int inset = (stroke_w + 1) / 2;
    const int span  = px - 2 * inset;

    const ui_glyph_t *g = &GLYPHS[id];
    for (int s = 0; s < g->n; s++) {
        const ui_stroke_t *st = &g->strokes[s];
        /* LVGL keeps the caller's pointer rather than copying the points, so
         * this array has to outlive the widget. The dashboard is built once at
         * boot and never torn down, so it is never freed — deliberately, and
         * noted here so nobody later reads it as a leak to chase. */
        lv_point_precise_t *p = lv_malloc(sizeof(lv_point_precise_t) * st->n);
        if (p == NULL) {
            return box;
        }
        for (int i = 0; i < st->n; i++) {
            p[i].x = inset + (int32_t)st->pts[i * 2]     * span / 1000;
            p[i].y = inset + (int32_t)st->pts[i * 2 + 1] * span / 1000;
        }
        lv_obj_t *line = lv_line_create(box);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_set_style_line_width(line, stroke_w, 0);
        lv_obj_set_style_line_color(line, color, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_line_set_points(line, p, st->n);
    }
    return box;
}

void ui_icon_set_color(lv_obj_t *icon, lv_color_t color)
{
    uint32_t n = lv_obj_get_child_count(icon);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_set_style_line_color(lv_obj_get_child(icon, i), color, 0);
    }
}
