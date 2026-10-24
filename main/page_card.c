// main/page_card.c —— 数字名片:工牌信息 + 名片/QSL 二维码切换。
#include "badge_app.h"

#include "app_config.h"
#include "app_fonts.h"
#include "app_store.h"
#include "app_theme.h"
#include "app_ui.h"
#include "bsp_display.h"
#include "misc/cache/instance/lv_image_cache.h"

#include <stdio.h>

#define VIEW_INFO 0
#define VIEW_CARD 1
#define VIEW_QSL 2

static lv_obj_t *s_scr;
static lv_obj_t *s_info;
static lv_obj_t *s_qr_panel;
static lv_obj_t *s_qr_img;
static lv_obj_t *s_qr_hint;
static lv_obj_t *s_hint;
static int s_view;

static bool file_exists(const char *rel) {
    if (!app_store_ready()) return false;
    char path[64];
    app_store_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void refresh(void) {
    const app_theme_t *th = app_theme_current();

    if (s_view == VIEW_INFO) {
        lv_obj_set_hidden(s_info, false);
        lv_obj_set_hidden(s_qr_panel, true);
        lv_label_set_text(s_hint, "OK 切换二维码 · 上/下 名片/QSL");
        return;
    }

    lv_obj_set_hidden(s_info, true);
    lv_obj_set_hidden(s_qr_panel, false);
    const char *rel = s_view == VIEW_CARD ? "qr_card.jpg" : "qr_qsl.jpg";
    const char *src = s_view == VIEW_CARD ? "S:/qr_card.jpg" : "S:/qr_qsl.jpg";
    if (file_exists(rel)) {
        lv_image_set_src(s_qr_img, NULL);
        lv_image_cache_drop(src);
        lv_image_set_src(s_qr_img, src);
        lv_obj_set_hidden(s_qr_img, false);
        lv_obj_set_hidden(s_qr_hint, true);
        lv_label_set_text(s_hint, s_view == VIEW_CARD ? "名片二维码 · 上/下 切换" : "QSL 二维码 · 上/下 切换");
    } else {
        lv_image_set_src(s_qr_img, NULL);
        lv_obj_set_hidden(s_qr_img, true);
        lv_obj_set_hidden(s_qr_hint, false);
        lv_label_set_text(s_qr_hint, "尚未上传二维码\n请在配网门户上传");
        lv_label_set_text(s_hint, "上/下 切换 · OK 返回名片");
    }
    (void)th;
}

static void card_enter(void) {
    const app_config_t *c = app_config();
    const app_theme_t *th = app_theme_current();
    s_scr = app_ui_screen_create("数字名片");
    s_view = VIEW_INFO;

    s_info = app_ui_panel(s_scr, 210, 186);
    lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_pad_all(s_info, 10, 0);

    lv_obj_t *name = app_ui_label(s_info, c->name, app_font_cjk_16(), false);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *nick = app_ui_label(s_info, c->nickname, app_font_cjk_14(), true);
    lv_obj_align(nick, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *org = app_ui_label(s_info, "", app_font_cjk_14(), false);
    lv_label_set_text_fmt(org, "%s · %s", c->company, c->department);
    lv_label_set_long_mode(org, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(org, 180);
    lv_obj_set_style_text_align(org, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(org, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_t *call = app_ui_label(s_info, "", app_font_cjk_14(), false);
    lv_label_set_text_fmt(call, "呼号 %s", c->callsign);
    lv_obj_set_style_text_color(call, lv_color_hex(th->accent), 0);
    lv_obj_align(call, LV_ALIGN_TOP_MID, 0, 88);

    lv_obj_t *mail = app_ui_label(s_info, "", app_font_cjk_14(), true);
    if (c->mail_user[0]) {
        lv_label_set_text(mail, c->mail_user);
    } else {
        lv_label_set_text(mail, "邮箱未配置");
    }
    lv_label_set_long_mode(mail, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(mail, 180);
    lv_obj_set_style_text_align(mail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mail, LV_ALIGN_TOP_MID, 0, 118);

    lv_obj_t *tip = app_ui_label(s_info, "配网门户可修改工牌信息", app_font_cjk_14(), true);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -4);

    s_qr_panel = app_ui_panel(s_scr, 222, 220);
    lv_obj_align(s_qr_panel, LV_ALIGN_TOP_MID, 0, 42);
    s_qr_img = lv_image_create(s_qr_panel);
    lv_obj_center(s_qr_img);
    s_qr_hint = app_ui_hint(s_qr_panel);
    lv_obj_center(s_qr_hint);

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    refresh();
    lv_screen_load(s_scr);
}

static void card_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_info = s_qr_panel = s_qr_img = s_qr_hint = s_hint = NULL;
    }
}

static void card_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    int next = s_view;
    if (btn == BSP_BTN_OK) {
        next = s_view == VIEW_INFO ? VIEW_CARD : VIEW_INFO;
    } else if (btn == BSP_BTN_UP) {
        next = (s_view + 2) % 3;
    } else if (btn == BSP_BTN_DOWN) {
        next = (s_view + 1) % 3;
    } else {
        return;
    }
    if (next == s_view) return;
    s_view = next;
    if (bsp_lvgl_lock(200)) {
        refresh();
        bsp_lvgl_unlock();
    }
}

static const app_page_t PAGE = {
    .name = "数字名片",
    .enter = card_enter,
    .exit = card_exit,
    .key = card_key,
};

const app_page_t *page_card(void) {
    return &PAGE;
}
