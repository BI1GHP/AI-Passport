// main/page_about.c —— 关于:版本与制作者信息。
#include "badge_app.h"

#include "app_fonts.h"
#include "app_ui.h"

static lv_obj_t *s_scr;

static void about_enter(void) {
    s_scr = app_ui_screen_create("关于");

    lv_obj_t *p = app_ui_panel(s_scr, 200, 150);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, -6);

    lv_obj_t *a = app_ui_label(p, "电子工牌", app_font_cjk_16(), false);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *b = app_ui_label(p, "版本号:V1.03", app_font_cjk_14(), false);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t *c = app_ui_label(p, "制作者:BI1GHP", app_font_cjk_14(), false);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 70);

    lv_obj_t *d = app_ui_label(p, "FoloToy AI Passport", &lv_font_montserrat_14, true);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *h = app_ui_label(s_scr, "长按 OK 返回", app_font_cjk_14(), true);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_screen_load(s_scr);
}

static void about_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

static const app_page_t PAGE = {
    .name = "关于",
    .enter = about_enter,
    .exit = about_exit,
    .key = NULL,
};

const app_page_t *page_about(void) {
    return &PAGE;
}
