// main/app_ui.c
#include "app_ui.h"

#include "app_fonts.h"
#include "app_theme.h"
#include "bsp_battery.h"

static void battery_cb(lv_timer_t *t) {
    lv_obj_t *lbl = t ? (lv_obj_t *)lv_timer_get_user_data(t) : NULL;
    if (!lbl) return;
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        lv_label_set_text_fmt(lbl, "%d%%", soc);
    } else {
        lv_label_set_text(lbl, "--");
    }
}

static void screen_delete_cb(lv_event_t *e) {
    lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
    if (t) lv_timer_delete(t);
}

lv_obj_t *app_ui_screen_create(const char *title) {
    const app_theme_t *th = app_theme_current();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(th->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    if (title) {
        lv_obj_t *t = app_ui_label(scr, title, app_font_cjk_16(), false);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
    }

    // 电池百分比:每 5 秒刷新,随屏幕删除自动销毁定时器。
    lv_obj_t *bat = lv_label_create(scr);
    lv_obj_set_style_text_font(bat, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bat, lv_color_hex(th->text_dim), 0);
    lv_obj_align(bat, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_timer_t *tm = lv_timer_create(battery_cb, 5000, bat);  // 恢复 battery_cb
    lv_obj_add_event_cb(scr, screen_delete_cb, LV_EVENT_DELETE, tm);
    battery_cb(tm);
    return scr;
}

lv_obj_t *app_ui_panel(lv_obj_t *parent, int w, int h) {
    const app_theme_t *th = app_theme_current();
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(th->panel), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(th->border), 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_set_scrollable(p, false);
    return p;
}

lv_obj_t *app_ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, bool dim) {
    const app_theme_t *th = app_theme_current();
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(dim ? th->text_dim : th->text), 0);
    return l;
}

void app_ui_row_style(lv_obj_t *panel, lv_obj_t *label, bool selected) {
    const app_theme_t *th = app_theme_current();
    lv_obj_set_style_border_color(panel, lv_color_hex(selected ? th->accent : th->border), 0);
    lv_obj_set_style_border_width(panel, selected ? 2 : 1, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(selected ? th->accent : th->text), 0);
}

lv_obj_t *app_ui_hint(lv_obj_t *parent) {
    const app_theme_t *th = app_theme_current();
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, app_font_cjk_14(), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(th->text_dim), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, 224);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}
