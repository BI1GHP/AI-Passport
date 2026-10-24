// main/page_placeholder.c —— 占位页:M1 中尚未实现的功能板块共用。
#include "badge_app.h"

#include "app_fonts.h"
#include "app_ui.h"

static const char *s_name = "模块";
static lv_obj_t *s_scr;

static void ph_enter(void) {
    s_scr = app_ui_screen_create(s_name);

    lv_obj_t *l = app_ui_label(s_scr, "模块建设中", app_font_cjk_16(), false);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *h = app_ui_label(s_scr, "长按 OK 返回", app_font_cjk_14(), true);
    lv_obj_align(h, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_screen_load(s_scr);
}

static void ph_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

static const app_page_t PAGE = {
    .name = "占位",
    .enter = ph_enter,
    .exit = ph_exit,
    .key = NULL,
};

const app_page_t *page_placeholder_named(const char *name) {
    s_name = name;
    return &PAGE;
}
