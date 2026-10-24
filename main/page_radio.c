// main/page_radio.c —— 网络电台:国内内置精选台列表 → MP3 流播放。
// 北京交通广播 / 音乐之声固定置顶;无需联网探测,进入页面即显示列表。
#include "badge_app.h"

#include "app_config.h"
#include "app_fonts.h"
#include "app_net.h"
#include "app_radio.h"
#include "app_theme.h"
#include "app_ui.h"
#include "bsp_display.h"

#include <stdio.h>

#define VIEW_LIST 1
#define VIEW_PLAYER 2
#define ROW_VISIBLE 7

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ROW_VISIBLE];
static lv_obj_t *s_lbl[ROW_VISIBLE];
static lv_obj_t *s_status;
static lv_obj_t *s_player;
static lv_obj_t *s_p_name;
static lv_obj_t *s_p_state;
static lv_obj_t *s_live;                    // LIVE 红点(播放中脉冲)
static lv_obj_t *s_bars[APP_RADIO_SPEC_BANDS]; // 频谱柱
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;
static lv_timer_t *s_spec_timer;
static bool s_live_on;

static int s_view;
static int s_sel;
static int s_top;

static void refresh(void) {
    const bool list_view = s_view == VIEW_LIST;
    lv_obj_set_hidden(s_player, list_view);

    const int total = app_radio_count();
    for (int i = 0; i < ROW_VISIBLE; i++) {
        const int idx = s_top + i;
        if (!list_view || idx >= total) {
            lv_obj_set_hidden(s_rows[i], true);
            continue;
        }
        lv_obj_set_hidden(s_rows[i], false);
        app_radio_station_t st;
        char buf[52];
        if (app_radio_station(idx, &st)) {
            snprintf(buf, sizeof(buf), "%s", st.name);
        } else {
            buf[0] = '\0';
        }
        lv_label_set_text(s_lbl[i], buf);
        app_ui_row_style(s_rows[i], s_lbl[i], idx == s_sel);
    }

    if (list_view) {
        if (!app_net_sta_connected()) {
            lv_label_set_text(s_status, "Wi-Fi 未连接\n可浏览列表,播放需联网");
        } else {
            lv_label_set_text(s_status, "");
        }
        lv_label_set_text(s_hint, "上/下 选择电台\nOK 播放 · 双击上/下 调音量");
    } else {
        app_radio_station_t st;
        char buf[52] = "未知电台";
        if (app_radio_station(app_radio_current(), &st)) {
            snprintf(buf, sizeof(buf), "%s", st.name);
        }
        lv_label_set_text(s_p_name, buf);
        const bool playing = app_radio_playing();
        lv_label_set_text(s_p_state, playing ? "正在播放" : "连接中断");
        lv_obj_set_hidden(s_live, !playing);
        lv_label_set_text(s_hint, playing ? "OK 停止播放" : "OK 返回列表");
    }
}

// 频谱 + LIVE 脉冲(120ms):数据来自播放任务的 Goertzel 频段能量
static void spec_cb(lv_timer_t *t) {
    (void)t;
    if (!s_scr || s_view != VIEW_PLAYER) return;
    uint8_t bands[APP_RADIO_SPEC_BANDS];
    app_radio_spectrum(bands);
    for (int i = 0; i < APP_RADIO_SPEC_BANDS; i++) {
        lv_bar_set_value(s_bars[i], bands[i], LV_ANIM_OFF);
    }
    if (app_radio_playing()) {
        s_live_on = !s_live_on;
        lv_obj_set_style_bg_opa(s_live, s_live_on ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

static void poll_cb(lv_timer_t *t) {
    (void)t;
    refresh();
}

static void open_list(void) {
    s_view = VIEW_LIST;
    s_sel = 0;
    s_top = 0;
    refresh();
}

static void radio_enter(void) {
    s_scr = app_ui_screen_create("网络电台");

    for (int i = 0; i < ROW_VISIBLE; i++) {
        s_rows[i] = app_ui_panel(s_scr, 210, 28);
        lv_obj_set_pos(s_rows[i], 15, 42 + i * 31);
        s_lbl[i] = app_ui_label(s_rows[i], "", app_font_cjk_14(), false);
        lv_obj_set_width(s_lbl[i], 190);
        lv_label_set_long_mode(s_lbl[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_center(s_lbl[i]);
    }

    s_status = app_ui_hint(s_scr);
    lv_obj_set_width(s_status, 210);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 38);

    s_player = app_ui_panel(s_scr, 210, 158);
    lv_obj_align(s_player, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_pad_all(s_player, 10, 0);
    const app_theme_t *th = app_theme_current();
    lv_obj_set_style_border_color(s_player, lv_color_hex(th->accent), 0);
    lv_obj_set_style_border_width(s_player, 1, 0);

    s_p_name = app_ui_label(s_player, "", app_font_cjk_16(), false);
    lv_obj_set_width(s_p_name, 186);
    lv_label_set_long_mode(s_p_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_p_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_p_name, LV_ALIGN_TOP_MID, 0, 4);

    // LIVE 红点 + 状态行
    s_live = lv_obj_create(s_player);
    lv_obj_set_size(s_live, 8, 8);
    lv_obj_set_style_radius(s_live, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_live, lv_color_hex(0xE84040), 0);
    lv_obj_set_style_border_width(s_live, 0, 0);
    lv_obj_set_style_pad_all(s_live, 0, 0);
    lv_obj_set_scrollable(s_live, false);
    lv_obj_align(s_live, LV_ALIGN_TOP_MID, -40, 32);

    s_p_state = app_ui_label(s_player, "", app_font_cjk_14(), false);
    lv_obj_set_style_text_color(s_p_state, lv_color_hex(th->accent), 0);
    lv_obj_align(s_p_state, LV_ALIGN_TOP_MID, 12, 28);

    // 频谱:8 根竖柱,随播放能量起伏
    for (int i = 0; i < APP_RADIO_SPEC_BANDS; i++) {
        s_bars[i] = lv_bar_create(s_player);
        lv_obj_set_size(s_bars[i], 13, 68);
        lv_obj_align(s_bars[i], LV_ALIGN_TOP_LEFT, 8 + i * 23, 54);
        lv_bar_set_range(s_bars[i], 0, 100);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_30, 0);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(th->accent), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bars[i], 3, 0);
        lv_obj_set_style_radius(s_bars[i], 3, LV_PART_INDICATOR);
    }

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    if (app_radio_count() == 0) {
        app_radio_load(); // 同步装载内置国内电台
    }
    s_poll = lv_timer_create(poll_cb, 400, NULL);
    s_spec_timer = lv_timer_create(spec_cb, 250, NULL);
    open_list();
    lv_screen_load(s_scr);
}

static void radio_exit(void) {
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    if (s_spec_timer) {
        lv_timer_delete(s_spec_timer);
        s_spec_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status = s_player = s_p_name = s_p_state = s_live = s_hint = NULL;
    }
}

static esp_err_t radio_stop(void) {
    app_radio_stop();
    return ESP_OK;
}

static void move_sel(int delta) {
    const int total = app_radio_count();
    if (total <= 0) return;
    s_sel = (s_sel + delta + total) % total;
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + ROW_VISIBLE) s_top = s_sel - ROW_VISIBLE + 1;
}

static void radio_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (!bsp_lvgl_lock(200)) return;

    if (s_view == VIEW_PLAYER) {
        bsp_lvgl_unlock();
        if (btn != BSP_BTN_OK) return;
        app_radio_stop(); // 停止任务可能阻塞数秒,不持 LVGL 锁
        if (bsp_lvgl_lock(200)) {
            open_list();
            bsp_lvgl_unlock();
        }
        return;
    }

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        move_sel(btn == BSP_BTN_UP ? -1 : 1);
    } else if (btn == BSP_BTN_OK) {
        if (app_radio_count() == 0) {
            app_radio_load();
        } else if (!app_radio_playing()) {
            esp_err_t e = app_radio_play(s_sel);
            if (e == ESP_OK) {
                s_view = VIEW_PLAYER;
            } else if (e == ESP_ERR_INVALID_STATE && !app_net_sta_connected()) {
                lv_label_set_text(s_status, "Wi-Fi 未连接\n请先在设置中连接网络");
            } else {
                lv_label_set_text(s_status, "播放失败,请重试");
            }
        }
    }
    refresh();
    bsp_lvgl_unlock();
}

static const app_page_t PAGE = {
    .name = "网络电台",
    .enter = radio_enter,
    .exit = radio_exit,
    .key = radio_key,
    .stop = radio_stop,
};

const app_page_t *page_radio(void) {
    return &PAGE;
}
