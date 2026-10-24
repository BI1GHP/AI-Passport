// main/page_menu.c —— 主菜单:五个功能板块 + 设置 + 关于。
#include "badge_app.h"

#include "app_fonts.h"
#include "app_ui.h"
#include "bsp_display.h"

#define ITEM_COUNT 7

static const char *NAMES[ITEM_COUNT] = {
    "数字名片", "邮箱", "小智AI", "网络电台", "录音", "设置", "关于",
};

static const app_page_t *get_card(void)     { return page_card(); }
static const app_page_t *get_mail(void)     { return page_mail(); }
static const app_page_t *get_xiaozhi(void)  { return page_xiaozhi(); }
static const app_page_t *get_radio(void)    { return page_radio(); }
static const app_page_t *get_recorder(void) { return page_recorder(); }
static const app_page_t *get_settings(void) { return page_settings(); }

typedef const app_page_t *(*get_fn)(void);
static const get_fn GETTERS[ITEM_COUNT] = {
    get_card, get_mail, get_xiaozhi,
    get_radio, get_recorder, get_settings, page_about,
};

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ITEM_COUNT];
static lv_obj_t *s_lbl[ITEM_COUNT];
static int s_sel;

static void refresh(void) {
    for (int i = 0; i < ITEM_COUNT; i++) {
        app_ui_row_style(s_rows[i], s_lbl[i], i == s_sel);
    }
}

static void menu_enter(void) {
    s_scr = app_ui_screen_create("菜单");
    for (int i = 0; i < ITEM_COUNT; i++) {
        s_rows[i] = app_ui_panel(s_scr, 180, 28);
        lv_obj_set_pos(s_rows[i], 30, 40 + i * 31);
        s_lbl[i] = app_ui_label(s_rows[i], NAMES[i], app_font_cjk_14(), false);
        lv_obj_center(s_lbl[i]);
    }
    s_sel = 0;
    refresh();
    lv_screen_load(s_scr);
}

static void menu_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

static void menu_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        app_nav_open(GETTERS[s_sel]());
        return;
    }
    if (bsp_lvgl_lock(200)) {
        if (btn == BSP_BTN_UP) {
            s_sel = (s_sel + ITEM_COUNT - 1) % ITEM_COUNT;
        } else if (btn == BSP_BTN_DOWN) {
            s_sel = (s_sel + 1) % ITEM_COUNT;
        }
        refresh();
        bsp_lvgl_unlock();
    }
}

static const app_page_t PAGE = {
    .name = "菜单",
    .enter = menu_enter,
    .exit = menu_exit,
    .key = menu_key,
};

const app_page_t *page_menu(void) {
    return &PAGE;
}
