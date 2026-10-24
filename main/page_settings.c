// main/page_settings.c —— 设置:配网门户开关、皮肤主题、Wi-Fi。
#include "badge_app.h"

#include "app_config.h"
#include "app_fonts.h"
#include "app_net.h"
#include "app_portal.h"
#include "app_theme.h"
#include "app_ui.h"
#include "bsp_display.h"
#include "esp_log.h"

#define ROW_COUNT 3

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ROW_COUNT];
static lv_obj_t *s_lbl[ROW_COUNT];
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;
static int s_sel;

static void refresh(void);

static void poll_timer(lv_timer_t *t) {
    (void)t;
    if (!s_scr) return;
    if (bsp_lvgl_lock(100)) {
        refresh();
        bsp_lvgl_unlock();
    }
}

static void refresh(void) {
    const app_theme_t *th = app_theme_current();
    app_config_t *c = app_config();
    lv_label_set_text_fmt(s_lbl[0], "配网门户  %s", app_portal_running() ? "开" : "关");
    lv_label_set_text_fmt(s_lbl[1], "皮肤主题  %s", th->name);
    if (!c->wifi_ssid[0]) {
        lv_label_set_text(s_lbl[2], "Wi-Fi  未配置");
    } else if (app_net_sta_connected()) {
        lv_label_set_text_fmt(s_lbl[2], "Wi-Fi  已连 %s", c->wifi_ssid);
    } else {
        lv_label_set_text_fmt(s_lbl[2], "Wi-Fi  连接中 %s", c->wifi_ssid);
    }
    for (int i = 0; i < ROW_COUNT; i++) {
        app_ui_row_style(s_rows[i], s_lbl[i], i == s_sel);
    }
    if (app_portal_running() && app_net_sta_connected()) {
        char ip[16];
        app_net_sta_ip(ip, sizeof(ip));
        lv_label_set_text_fmt(s_hint,
                              "门户已开启\n手机/电脑连接同一 Wi-Fi(%s)\n浏览器输入 http://%s 进入设置\n可修改工牌信息、照片、二维码、录音",
                              c->wifi_ssid, ip);
    } else if (app_portal_running()) {
        lv_label_set_text(s_hint, "门户已开启,等待 Wi-Fi 连接\n连接后此处显示访问地址");
    } else {
        lv_label_set_text(s_hint, "OK 开启配网门户\n开启后此处显示网页访问地址\n网页内可改工牌信息、照片、二维码\n并下载录音文件");
    }
}

// 主题切换后整体重建页面以应用新配色(调用方须持锁)。
static void settings_exit(void);
static void settings_enter(void);

static void settings_enter(void) {
    s_scr = app_ui_screen_create("设置");
    for (int i = 0; i < ROW_COUNT; i++) {
        s_rows[i] = app_ui_panel(s_scr, 180, 28);
        lv_obj_set_pos(s_rows[i], 30, 34 + i * 29);
        s_lbl[i] = app_ui_label(s_rows[i], "", app_font_cjk_14(), false);
        lv_obj_center(s_lbl[i]);
    }
    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 154);
    s_sel = 0;
    refresh();
    s_poll = lv_timer_create(poll_timer, 2000, NULL);
    lv_screen_load(s_scr);
}

static void settings_exit(void) {
    if (s_poll) { lv_timer_delete(s_poll); s_poll = NULL; }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
}

static void toggle_portal(void) {
    esp_err_t e;
    if (app_portal_running()) {
        e = app_portal_stop();
    } else {
        e = app_portal_start();
    }
    if (e != ESP_OK) ESP_LOGE("settings", "门户切换失败: %s", esp_err_to_name(e));
}

static void cycle_theme(void) {
    app_config_t *c = app_config();
    c->theme = (c->theme + 1) % APP_THEME_COUNT;
    app_config_save();
}

static void settings_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        if (s_sel == 0) {
            toggle_portal();
        } else if (s_sel == 1) {
            cycle_theme();
            if (bsp_lvgl_lock(200)) {
                settings_exit();
                settings_enter();
                bsp_lvgl_unlock();
            }
            return;
        } else {
            app_nav_open(page_wifi());
            return;
        }
        if (bsp_lvgl_lock(200)) {
            refresh();
            bsp_lvgl_unlock();
        }
        return;
    }
    if (bsp_lvgl_lock(200)) {
        if (btn == BSP_BTN_UP) {
            s_sel = (s_sel + ROW_COUNT - 1) % ROW_COUNT;
        } else if (btn == BSP_BTN_DOWN) {
            s_sel = (s_sel + 1) % ROW_COUNT;
        }
        refresh();
        bsp_lvgl_unlock();
    }
}

static const app_page_t PAGE = {
    .name = "设置",
    .enter = settings_enter,
    .exit = settings_exit,
    .key = settings_key,
};

const app_page_t *page_settings(void) {
    return &PAGE;
}
