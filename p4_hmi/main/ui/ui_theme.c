#include "ui_theme.h"

lv_style_t ui_st_screen;
lv_style_t ui_st_card;
lv_style_t ui_st_tile;
lv_style_t ui_st_section;
lv_style_t ui_st_caption;
lv_style_t ui_st_banner;

void ui_theme_init(void)
{
    lv_style_init(&ui_st_screen);
    lv_style_set_bg_color(&ui_st_screen, UI_C_BG);
    lv_style_set_bg_opa(&ui_st_screen, LV_OPA_COVER);
    lv_style_set_text_color(&ui_st_screen, UI_C_TEXT);
    lv_style_set_text_font(&ui_st_screen, UI_FONT_BODY);
    lv_style_set_border_width(&ui_st_screen, 0);
    lv_style_set_pad_all(&ui_st_screen, 0);

    lv_style_init(&ui_st_card);
    lv_style_set_bg_color(&ui_st_card, UI_C_CARD);
    lv_style_set_bg_opa(&ui_st_card, LV_OPA_COVER);
    lv_style_set_radius(&ui_st_card, UI_RADIUS_CARD);
    lv_style_set_border_width(&ui_st_card, 0);
    lv_style_set_pad_all(&ui_st_card, UI_CARD_PAD);

    /* Same card, tighter radius, and a border that is transparent until the
     * control is on — so switching state changes the colour, never the layout. */
    lv_style_init(&ui_st_tile);
    lv_style_set_bg_color(&ui_st_tile, UI_C_CARD);
    lv_style_set_bg_opa(&ui_st_tile, LV_OPA_COVER);
    lv_style_set_radius(&ui_st_tile, UI_RADIUS_TILE);
    lv_style_set_border_width(&ui_st_tile, 3);
    lv_style_set_border_color(&ui_st_tile, UI_C_CARD);
    lv_style_set_border_opa(&ui_st_tile, LV_OPA_TRANSP);
    lv_style_set_pad_all(&ui_st_tile, 6);
    lv_style_set_shadow_width(&ui_st_tile, 0);

    /* "PUMPS", "FEATURES" — the app's small, wide-tracked, muted section caps. */
    lv_style_init(&ui_st_section);
    lv_style_set_text_font(&ui_st_section, UI_FONT_CAPTION);
    lv_style_set_text_color(&ui_st_section, UI_C_MUTED);
    lv_style_set_text_letter_space(&ui_st_section, 2);

    lv_style_init(&ui_st_caption);
    lv_style_set_text_font(&ui_st_caption, UI_FONT_CAPTION);
    lv_style_set_text_color(&ui_st_caption, UI_C_MUTED);

    lv_style_init(&ui_st_banner);
    lv_style_set_radius(&ui_st_banner, UI_RADIUS_TILE);
    lv_style_set_bg_opa(&ui_st_banner, LV_OPA_COVER);
    lv_style_set_border_width(&ui_st_banner, 0);
    lv_style_set_pad_all(&ui_st_banner, 12);
    lv_style_set_text_font(&ui_st_banner, UI_FONT_LABEL);
}
