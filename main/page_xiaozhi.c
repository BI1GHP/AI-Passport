// main/page_xiaozhi.c —— 小智AI 语音对话页:动态人像 + 状态光环 + 声波 + 回复文本。
// 人像为 Flash 中的 RGB565 图(140x140),动画全部程序化生成,不占额外 RAM:
//   · 待机/各类状态:人像缓慢上下浮动 + 身后光环按状态节奏呼吸
//   · 回复中:状态文字替换为跳动的声波条
#include "badge_app.h"

#include "app_fonts.h"
#include "app_net.h"
#include "app_theme.h"
#include "app_ui.h"
#include "app_xiaozhi.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "xz_portrait.h"

#include <string.h>

#define BAR_NUM 5
#define BAR_BASE_Y 208 // 声波条底部基线
#define BAR_W 6
#define BAR_GAP 8

static lv_obj_t *s_scr;
static lv_obj_t *s_aura;   // 人像身后的呼吸光环
static lv_obj_t *s_card;   // 白色圆角卡片(与人像一起浮动)
static lv_obj_t *s_img;    // 人物形象
static lv_obj_t *s_state;
static lv_obj_t *s_text;
static lv_obj_t *s_hint;
static lv_obj_t *s_bars[BAR_NUM];
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
    case XZ_ST_ACTIVATING: return "控制台添加设备并输入激活码";
    case XZ_ST_READY:      return "OK 开始说话 · 长按返回";
    case XZ_ST_LISTENING:  return "请说话 · OK 结束并发送";
    case XZ_ST_THINKING:   return "OK 打断并说话 · 长按返回";
    case XZ_ST_SPEAKING:   return "OK 打断并说话 · 长按返回";
    case XZ_ST_ERROR:      return "OK 重新连接 · 长按返回";
    default:               return "正在连接小智服务器…";
    }
}

// ---- 动画执行回调 ----
static void anim_float_y(void *obj, int32_t v) {
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_aura_opa(void *obj, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_bar_h(void *obj, int32_t v) {
    lv_obj_set_height((lv_obj_t *)obj, v);
    lv_obj_set_y((lv_obj_t *)obj, BAR_BASE_Y - v); // 从基线向上生长
}

// 光环呼吸:按状态换节奏与强度
static void aura_pulse(uint32_t dur_ms, int32_t opa_max) {
    lv_anim_delete(s_aura, anim_aura_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_aura);
    lv_anim_set_exec_cb(&a, anim_aura_opa);
    lv_anim_set_values(&a, 6, opa_max);
    lv_anim_set_duration(&a, dur_ms);
    lv_anim_set_playback_duration(&a, dur_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// 声波条:回复中持续跳动,其余状态隐藏
static void bars_run(bool on) {
    for (int i = 0; i < BAR_NUM; i++) {
        lv_obj_t *b = s_bars[i];
        lv_anim_delete(b, anim_bar_h);
        if (!on) {
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            anim_bar_h(b, 6);
            continue;
        }
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, b);
        lv_anim_set_exec_cb(&a, anim_bar_h);
        lv_anim_set_values(&a, 6, (i % 2) ? 12 : 18);
        lv_anim_set_duration(&a, (uint32_t)(240 + i * 40));
        lv_anim_set_playback_duration(&a, (uint32_t)(240 + i * 40));
        lv_anim_set_repeat_delay(&a, (uint32_t)(i * 50));
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

static void apply_visual(xz_state_t st) {
    const app_theme_t *th = app_theme_current();
    uint32_t dur = 1000;
    int32_t opa = 26;
    switch (st) {
    case XZ_ST_LISTENING: dur = 420;  opa = 70; break;
    case XZ_ST_THINKING:  dur = 320;  opa = 80; break;
    case XZ_ST_SPEAKING:  dur = 560;  opa = 55; break;
    case XZ_ST_READY:     dur = 1400; opa = 40; break;
    case XZ_ST_ERROR:     dur = 800;  opa = 18; break;
    default:              break;
    }
    lv_obj_set_style_bg_color(s_aura, lv_color_hex(st == XZ_ST_ERROR ? 0x808080 : th->accent), 0);
    aura_pulse(dur, opa);

    bool speak = (st == XZ_ST_SPEAKING);
    if (speak) lv_obj_add_flag(s_state, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_state, LV_OBJ_FLAG_HIDDEN);
    bars_run(speak);
}

static void refresh(void) {
    xz_state_t st = app_xiaozhi_state();
    lv_label_set_text(s_state, state_text(st));
    if (st == XZ_ST_ACTIVATING) {
        lv_label_set_text_fmt(s_text, "激活码\n%s", app_xiaozhi_activation_code());
    } else {
        char txt[256];
        app_xiaozhi_last_text(txt, sizeof(txt));
        lv_label_set_text(s_text, txt);
    }
    lv_label_set_text(s_hint, hint_text(st));
    if (st != s_shown) apply_visual(st);
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

    // 1) 人像身后的呼吸光环
    s_aura = lv_obj_create(s_scr);
    lv_obj_set_size(s_aura, 158, 158);
    lv_obj_set_pos(s_aura, 41, 28);
    lv_obj_remove_flag(s_aura, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_aura, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_aura, lv_color_hex(th->accent), 0);
    lv_obj_set_style_bg_opa(s_aura, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_aura, 0, 0);
    lv_obj_set_style_pad_all(s_aura, 0, 0);

    // 2) 人物卡片(白底,圆角半径小于人像内缩量,避免白角露出卡片外)
    s_card = lv_obj_create(s_scr);
    lv_obj_set_size(s_card, 150, 150);
    lv_obj_set_pos(s_card, 45, 33);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_card, 16, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_card, 0, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);

    s_img = lv_image_create(s_card);
    lv_image_set_src(s_img, &xz_portrait);
    lv_obj_set_pos(s_img, 5, 5);

    // 卡片整体缓慢浮动(呼吸感),人像随卡片走,不会越出圆角
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card);
    lv_anim_set_exec_cb(&a, anim_float_y);
    lv_anim_set_values(&a, 33, 39);
    lv_anim_set_duration(&a, 1500);
    lv_anim_set_playback_duration(&a, 1500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    // 3) 状态文字(回复中隐藏,让位给声波)
    s_state = app_ui_label(s_scr, "", app_font_cjk_16(), false);
    lv_obj_set_style_text_color(s_state, lv_color_hex(th->accent), 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 190);

    // 4) 声波条
    const int x0 = (APP_LCD_W - (BAR_NUM * BAR_W + (BAR_NUM - 1) * BAR_GAP)) / 2;
    for (int i = 0; i < BAR_NUM; i++) {
        lv_obj_t *b = lv_obj_create(s_scr);
        lv_obj_set_size(b, BAR_W, 6);
        lv_obj_set_pos(b, x0 + i * (BAR_W + BAR_GAP), BAR_BASE_Y - 6);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(b, BAR_W / 2, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(th->accent), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_bars[i] = b;
    }

    // 5) 对话气泡:14 号字可容 3 行,超出以省略号收尾
    lv_obj_t *bubble = lv_obj_create(s_scr);
    lv_obj_set_size(bubble, 212, 70);
    lv_obj_set_pos(bubble, 14, 212);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(th->panel), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(th->border), 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    lv_obj_set_style_pad_all(bubble, 6, 0);

    s_text = app_ui_label(bubble, "", app_font_cjk_14(), false);
    lv_obj_set_size(s_text, 200, 57);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_text, LV_ALIGN_TOP_MID, 0, 0);

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    if (!app_net_sta_connected()) {
        lv_label_set_text(s_state, "无网络");
        lv_label_set_text(s_hint, "需先连接 Wi-Fi · 长按返回");
    } else if (app_xiaozhi_open() != ESP_OK) {
        lv_label_set_text(s_state, "启动失败");
        lv_label_set_text(s_hint, "内存不足或网络忙 · 长按返回");
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
    // 先停掉本页所有动画,再删除对象,避免动画回调访问已释放对象
    if (s_card) lv_anim_delete(s_card, anim_float_y);
    if (s_aura) lv_anim_delete(s_aura, anim_aura_opa);
    for (int i = 0; i < BAR_NUM; i++) {
        if (s_bars[i]) lv_anim_delete(s_bars[i], anim_bar_h);
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_state = s_text = s_hint = NULL;
        s_aura = s_card = s_img = NULL;
        memset(s_bars, 0, sizeof(s_bars));
    }
}

static esp_err_t xz_stop(void) {
    app_xiaozhi_close(); // 阻塞等待任务退出,不持 LVGL 锁
    return ESP_OK;
}

static void xz_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK || btn != BSP_BTN_OK) return;
    xz_state_t st = app_xiaozhi_state();
    ESP_LOGI("page_xiaozhi", "OK 键,当前状态 %d", (int)st);
    if (st == XZ_ST_READY || st == XZ_ST_LISTENING ||
        st == XZ_ST_THINKING || st == XZ_ST_SPEAKING) {
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
