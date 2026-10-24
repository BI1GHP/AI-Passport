// main/page_xiaozhi.c —— 小智AI 语音对话页:OK 开始/结束说话,显示状态与回复文本。
#include "badge_app.h"

#include "app_fonts.h"
#include "app_net.h"
#include "app_theme.h"
#include "app_ui.h"
#include "app_xiaozhi.h"
#include "bsp_display.h"

#include <string.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_state;
static lv_obj_t *s_text;
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;
static xz_state_t s_shown = XZ_ST_IDLE;

static const char *state_text(xz_state_t st) {
    switch (st) {
    case XZ_ST_CONNECTING: return "连接中";
    case XZ_ST_ACTIVATING: return "待激活";
    case XZ_ST_READY:      return "就绪";
    case XZ_ST_LISTENING:  return "聆听中";
    case XZ_ST_THINKING:   return "思考中";
    case XZ_ST_SPEAKING:   return "回复中";
    case XZ_ST_ERROR:      return "连接失败";
    default:               return "未连接";
    }
}

static const char *hint_text(xz_state_t st) {
    switch (st) {
    case XZ_ST_ACTIVATING: return "请到小智控制台\n添加设备并输入激活码";
    case XZ_ST_READY:      return "OK 开始说话 · 长按返回";
    case XZ_ST_LISTENING:  return "请说话 · OK 结束并发送";
    case XZ_ST_ERROR:      return "OK 重新连接 · 长按返回";
    default:               return "正在连接小智服务器…";
    }
}

static void refresh(void) {
    xz_state_t st = app_xiaozhi_state();
    lv_label_set_text(s_state, state_text(st));
    if (st == XZ_ST_ACTIVATING) {
        lv_label_set_text_fmt(s_text, "激活码\n%s", app_xiaozhi_activation_code());
    } else {
        char txt[96];
        app_xiaozhi_last_text(txt, sizeof(txt));
        lv_label_set_text(s_text, txt);
    }
    lv_label_set_text(s_hint, hint_text(st));
    s_shown = st;
}

static void poll_cb(lv_timer_t *t) {
    (void)t;
    if (app_xiaozhi_state() != s_shown || s_shown == XZ_ST_SPEAKING ||
        s_shown == XZ_ST_ACTIVATING || s_shown == XZ_ST_CONNECTING) {
        refresh();
    }
}

static void xz_enter(void) {
    const app_theme_t *th = app_theme_current();
    s_scr = app_ui_screen_create("小智AI");
    s_shown = XZ_ST_IDLE;

    s_state = app_ui_label(s_scr, "", app_font_cjk_16(), false);
    lv_obj_set_style_text_color(s_state, lv_color_hex(th->accent), 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 96);

    s_text = app_ui_label(s_scr, "", app_font_cjk_16(), true);
    lv_obj_set_width(s_text, 200);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, 130);

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    if (!app_net_sta_connected()) {
        lv_label_set_text(s_state, "无网络");
        lv_label_set_text(s_hint, "需先连接 Wi-Fi\n长按返回");
    } else if (app_xiaozhi_open() != ESP_OK) {
        lv_label_set_text(s_state, "启动失败");
        lv_label_set_text(s_hint, "内存不足或网络忙\n长按返回");
    }

    refresh();
    s_poll = lv_timer_create(poll_cb, 250, NULL);
    lv_screen_load(s_scr);
}

static void xz_exit(void) {
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_state = s_text = s_hint = NULL;
    }
}

static esp_err_t xz_stop(void) {
    app_xiaozhi_close(); // 阻塞等待任务退出,不持 LVGL 锁
    return ESP_OK;
}

static void xz_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK || btn != BSP_BTN_OK) return;
    xz_state_t st = app_xiaozhi_state();
    if (st == XZ_ST_READY || st == XZ_ST_LISTENING) {
        app_xiaozhi_talk_toggle();
    } else if (st == XZ_ST_ERROR || st == XZ_ST_IDLE) {
        app_xiaozhi_close();
        app_xiaozhi_open(); // 失败后 OK 重连
    }
    if (bsp_lvgl_lock(200)) {
        refresh();
        bsp_lvgl_unlock();
    }
}

static const app_page_t PAGE = {
    .name = "小智AI",
    .enter = xz_enter,
    .exit = xz_exit,
    .key = xz_key,
    .stop = xz_stop,
};

const app_page_t *page_xiaozhi(void) {
    return &PAGE;
}
