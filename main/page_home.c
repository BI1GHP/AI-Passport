// main/page_home.c —— 首页电子工牌。
// 布局(240x320,互不重叠):
//   y12   右上:电量(app_ui)+ WiFi 4 格信号条
//   y14   日期(周几)
//   y32   时间 HH:MM(48px 大字体,双层偏移加粗,accent)
//   y90   天气一行
//   y126  左侧:证件照(白边 104x138);右侧:姓名(accent 1.5 倍)+ 公司/部门垂直铺开
// 上/下键:直接切换 工牌视图 ↔ 名片二维码 ↔ QSL 二维码。
#include "badge_app.h"

#include "app_config.h"
#include "app_fonts.h"
#include "app_net.h"
#include "app_store.h"
#include "app_theme.h"
#include "app_time.h"
#include "app_ui.h"
#include "app_weather.h"
#include "bsp_display.h"

#include "misc/cache/instance/lv_image_cache.h"

#include <stdio.h>

#define VIEW_BADGE 0
#define VIEW_CARD 1
#define VIEW_QSL 2

// 照片区域(左侧证件照,白边 2px,内图 100x134 ≈ 3:4)
// 照片+信息作为组合块水平居中:104 + 12 间距 + 96 文字区 = 212,左右各留 14px
#define PHOTO_X 14
#define PHOTO_Y 126
#define PHOTO_W 104
#define PHOTO_H 138
// 右侧文字区
#define TEXT_X 130
#define TEXT_W 96

static lv_obj_t *s_scr;
static lv_obj_t *s_date_l, *s_time_l, *s_time_b, *s_wea_l;
static lv_obj_t *s_name_l, *s_org_l, *s_rule;
static lv_obj_t *s_photo;          // 照片容器(无边框)
static lv_obj_t *s_qr_panel, *s_qr_img, *s_qr_hint;
static lv_obj_t *s_hint;
static lv_obj_t *s_wifi_bars[4];   // 信号格:高度递增的 4 个小条
static lv_timer_t *s_tick;
static lv_timer_t *s_wifi_timer;
static uint32_t s_last_gen;
static int s_view;

static const char *WD[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

static bool file_exists(const char *rel) {
    if (!app_store_ready()) return false;
    char path[64];
    app_store_path(path, sizeof(path), rel);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void refresh_profile(void) {
    const app_config_t *c = app_config();
    lv_label_set_text(s_name_l, c->name);
    lv_label_set_text_fmt(s_org_l, "%s\n%s", c->company, c->department);
}

static void tick(lv_timer_t *t) {
    (void)t;
    struct tm now;
    app_time_now(&now);
    lv_label_set_text_fmt(s_time_l, "%02d:%02d", now.tm_hour, now.tm_min);
    lv_label_set_text_fmt(s_time_b, "%02d:%02d", now.tm_hour, now.tm_min);
    lv_label_set_text_fmt(s_date_l, "%04d-%02d-%02d %s",
                          now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, WD[now.tm_wday % 7]);
    lv_label_set_text(s_wea_l, app_weather_summary());
    uint32_t g = app_config_generation();
    if (g != s_last_gen) { s_last_gen = g; refresh_profile(); }
}

// WiFi 状态刷新(每 5 秒):用 4 格信号条显示强度
static void wifi_cb(lv_timer_t *t) {
    (void)t;
    app_config_t *c = app_config();
    int active = 0;
    lv_color_t on_color, off_color;
    if (app_net_sta_connected()) {
        int8_t rssi = app_net_sta_rssi();
        if (rssi >= -55) active = 4;
        else if (rssi >= -65) active = 3;
        else if (rssi >= -75) active = 2;
        else active = 1;
        on_color = lv_color_hex(0xFFFFFF);
        off_color = lv_color_hex(0x444444);
    } else if (c->wifi_ssid[0]) {
        active = 0;
        on_color = lv_color_hex(0x444444);
        off_color = lv_color_hex(0x444444);
    } else {
        active = 0;
        on_color = lv_color_hex(0x222222);
        off_color = lv_color_hex(0x222222);
    }
    for (int i = 0; i < 4; i++) {
        lv_color_t col = (i < active) ? on_color : off_color;
        lv_obj_set_style_bg_color(s_wifi_bars[i], col, 0);
        lv_obj_set_style_bg_opa(s_wifi_bars[i], LV_OPA_COVER, 0);
    }
}

static void apply_view(void) {
    const bool badge = s_view == VIEW_BADGE;
    lv_obj_set_hidden(s_photo, !badge);
    lv_obj_set_hidden(s_name_l, !badge);
    lv_obj_set_hidden(s_org_l, !badge);
    lv_obj_set_hidden(s_rule, !badge);
    lv_obj_set_hidden(s_qr_panel, badge);

    if (badge) {
        lv_label_set_text(s_hint, "上/下 切换名片 · OK 进菜单\n双击上/下 调节音量");
        return;
    }
    const char *rel = s_view == VIEW_CARD ? "qr_card.jpg" : "qr_qsl.jpg";
    const char *src = s_view == VIEW_CARD ? "S:/qr_card.jpg" : "S:/qr_qsl.jpg";
    if (file_exists(rel)) {
        lv_image_set_src(s_qr_img, NULL);
        lv_image_cache_drop(src);
        lv_image_set_src(s_qr_img, src);
        lv_obj_set_hidden(s_qr_img, false);
        lv_obj_set_hidden(s_qr_hint, true);
    } else {
        lv_image_set_src(s_qr_img, NULL);
        lv_obj_set_hidden(s_qr_img, true);
        lv_obj_set_hidden(s_qr_hint, false);
        lv_label_set_text(s_qr_hint, "尚未上传二维码\n请在配网门户上传");
    }
    lv_label_set_text(s_hint, s_view == VIEW_CARD
                      ? "电子名片二维码\n上/下 切换视图 · OK 进菜单"
                      : "QSL 二维码\n上/下 切换视图 · OK 进菜单");
}

static void home_enter(void) {
    app_config_t *c = app_config();
    const app_theme_t *th = app_theme_current();
    s_scr = app_ui_screen_create(NULL);
    s_view = VIEW_BADGE;

    s_date_l = app_ui_label(s_scr, "", app_font_cjk_14(), true);
    lv_obj_align(s_date_l, LV_ALIGN_TOP_MID, 0, 14);

    // 时间 48px 大字体;双层偏移 1px 叠加模拟加粗
    s_time_l = app_ui_label(s_scr, "00:00", &lv_font_montserrat_48, false);
    lv_obj_set_style_text_color(s_time_l, lv_color_hex(th->accent), 0);
    lv_obj_align(s_time_l, LV_ALIGN_TOP_MID, 0, 32);
    s_time_b = app_ui_label(s_scr, "00:00", &lv_font_montserrat_48, false);
    lv_obj_set_style_text_color(s_time_b, lv_color_hex(th->accent), 0);
    lv_obj_align(s_time_b, LV_ALIGN_TOP_MID, 1, 32);

    s_wea_l = app_ui_label(s_scr, "--", app_font_cjk_14(), true);
    lv_obj_align(s_wea_l, LV_ALIGN_TOP_MID, 0, 90);

    // 证件照(左侧,白边)
    s_photo = lv_obj_create(s_scr);
    lv_obj_set_size(s_photo, PHOTO_W, PHOTO_H);
    lv_obj_set_pos(s_photo, PHOTO_X, PHOTO_Y);
    lv_obj_set_style_border_width(s_photo, 0, 0);
    lv_obj_set_style_radius(s_photo, 0, 0);
    lv_obj_set_style_pad_all(s_photo, 2, 0); // 白边 2px
    lv_obj_set_style_bg_color(s_photo, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_photo, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_photo, false);
    bool has_photo = file_exists("photo.jpg");
    if (has_photo) {
        lv_obj_t *img = lv_image_create(s_photo);
        lv_image_set_src(img, "S:/photo.jpg");
        lv_obj_center(img);
    } else {
        lv_obj_t *pht = app_ui_label(s_photo, "相片", app_font_cjk_14(), true);
        lv_obj_center(pht);
    }

    // 右侧:姓名(accent 色,1.5 倍放大)+ 分隔线 + 公司/部门,垂直铺开填满照片高度
    s_name_l = app_ui_label(s_scr, c->name, app_font_cjk_16(), false);
    lv_obj_set_style_text_color(s_name_l, lv_color_hex(th->accent), 0);
    lv_obj_set_style_transform_zoom(s_name_l, 384, 0); // 1.5x,运行时缩放不占字体空间
    lv_obj_set_style_transform_pivot_x(s_name_l, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_name_l, 0, 0);
    lv_obj_set_pos(s_name_l, TEXT_X, 124);

    s_rule = lv_obj_create(s_scr);
    lv_obj_set_scrollable(s_rule, false);
    lv_obj_set_style_pad_all(s_rule, 0, 0);
    lv_obj_set_style_border_width(s_rule, 0, 0);
    lv_obj_set_style_radius(s_rule, 1, 0);
    lv_obj_set_style_bg_color(s_rule, lv_color_hex(th->accent), 0);
    lv_obj_set_style_bg_opa(s_rule, LV_OPA_COVER, 0);
    lv_obj_set_size(s_rule, 40, 2);
    lv_obj_set_pos(s_rule, TEXT_X, 162);

    s_org_l = app_ui_label(s_scr, "", app_font_cjk_14(), false);
    lv_obj_set_pos(s_org_l, TEXT_X, 174);
    lv_label_set_long_mode(s_org_l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_org_l, TEXT_W);
    lv_obj_set_style_text_line_space(s_org_l, 8, 0);

    // 二维码视图(名片/QSL),默认隐藏
    s_qr_panel = app_ui_panel(s_scr, 214, 214);
    lv_obj_align(s_qr_panel, LV_ALIGN_TOP_MID, 0, 44);
    s_qr_img = lv_image_create(s_qr_panel);
    lv_obj_center(s_qr_img);
    s_qr_hint = app_ui_hint(s_qr_panel);
    lv_obj_center(s_qr_hint);
    lv_obj_set_hidden(s_qr_panel, true);

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_last_gen = app_config_generation();
    s_tick = lv_timer_create(tick, 1000, NULL);
    tick(s_tick);

    // WiFi 信号:左上角,4 格递增信号条,每 5 秒刷新
    int bar_w = 4, gap = 1, base_y = 26;
    int heights[4] = {5, 9, 13, 17};
    for (int i = 0; i < 4; i++) {
        s_wifi_bars[i] = lv_obj_create(s_scr);
        lv_obj_set_scrollable(s_wifi_bars[i], false);
        lv_obj_set_style_pad_all(s_wifi_bars[i], 0, 0);
        lv_obj_set_style_border_width(s_wifi_bars[i], 0, 0);
        lv_obj_set_style_radius(s_wifi_bars[i], 1, 0);
        lv_obj_set_size(s_wifi_bars[i], bar_w, heights[i]);
        lv_obj_set_pos(s_wifi_bars[i], 8 + i * (bar_w + gap), base_y - heights[i]);
        lv_obj_set_style_bg_color(s_wifi_bars[i], lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(s_wifi_bars[i], LV_OPA_COVER, 0);
    }
    s_wifi_timer = lv_timer_create(wifi_cb, 5000, NULL);
    wifi_cb(s_wifi_timer);

    refresh_profile();
    apply_view();
    lv_screen_load(s_scr);
}

static void home_exit(void) {
    if (s_tick) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    if (s_wifi_timer) {
        lv_timer_delete(s_wifi_timer);
        s_wifi_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_photo = s_qr_panel = s_qr_img = s_qr_hint = s_hint = s_rule = NULL;
    }
}

static void home_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn != BSP_BTN_UP && btn != BSP_BTN_DOWN) return;
    s_view = (s_view + (btn == BSP_BTN_DOWN ? 1 : 2)) % 3;
    if (bsp_lvgl_lock(200)) {
        apply_view();
        bsp_lvgl_unlock();
    }
}

static const app_page_t PAGE = {
    .name = "电子工牌",
    .enter = home_enter,
    .exit = home_exit,
    .key = home_key,
};

const app_page_t *page_home(void) {
    return &PAGE;
}
