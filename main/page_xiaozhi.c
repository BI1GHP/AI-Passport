// main/page_xiaozhi.c —— 小智AI 语音对话页:动漫卡通人像作全屏底图 + 状态光环 + 声波条。
// 人像为 Flash 中的 I8 索引图(240x320,零 RAM 占用),整幅静态不叠加口型/眨眼图层
// (叠加的嘴与眼睑在实测中显假),情绪张力由屏幕描边光环与声波条承担;
// 标题、字幕直接叠在画面上,不加面板,与画面融为一体。
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
#define BAR_W 6
#define BAR_GAP 8

// 人像作为全屏底图铺满 240×320,标题栏/电池/提示/字幕都叠在画面上
#define XZ_IMG_W 240
#define XZ_IMG_H 320
#define BAR_BASE_Y 258 // 声波条底部基线(压在人物下方的浅蓝卫衣区)

static lv_obj_t *s_scr;
static lv_obj_t *s_stage;  // 人像容器(裁切,铺满内容区)
static lv_obj_t *s_img;    // 动漫卡通人像(静态)
static lv_obj_t *s_glow;   // 状态光环(描边,呼吸动画作用于此)
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
static void anim_glow_opa(void *obj, int32_t v) {
    lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_bar_h(void *obj, int32_t v) {
    lv_obj_set_height((lv_obj_t *)obj, v);
    lv_obj_set_y((lv_obj_t *)obj, BAR_BASE_Y - v); // 从基线向上生长
}

// 状态光环(屏幕内容区描边)呼吸:按状态换节奏与强度
static void glow_pulse(uint32_t dur_ms, int32_t opa_max) {
    lv_anim_delete(s_glow, anim_glow_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_glow);
    lv_anim_set_exec_cb(&a, anim_glow_opa);
    lv_anim_set_values(&a, 30, opa_max);
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
    int32_t opa = 90;
    switch (st) {
    case XZ_ST_LISTENING: dur = 420;  opa = 220; break;
    case XZ_ST_THINKING:  dur = 320;  opa = 240; break;
    case XZ_ST_SPEAKING:  dur = 560;  opa = 180; break;
    case XZ_ST_READY:     dur = 1400; opa = 140; break;
    case XZ_ST_ERROR:     dur = 800;  opa = 60;  break;
    default:              break;
    }
    lv_obj_set_style_border_color(s_glow,
        lv_color_hex(st == XZ_ST_ERROR ? 0x808080 : th->accent), 0);
    glow_pulse(dur, opa);

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
    // 每 250ms 都刷新:文本/状态可能在不换状态时到达(如 STT 结果),只按状态刷会漏更新
    refresh();
}

// 叠在彩色画面上的文字加半透明白色描边:保证可读,又不出现面板遮挡人物
static void text_outline(lv_obj_t *o) {
    if (!o) return;
    lv_obj_set_style_text_outline_stroke_color(o, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_outline_stroke_width(o, 2, 0);
    lv_obj_set_style_text_outline_stroke_opa(o, LV_OPA_80, 0);
}

// 叠在浅色画面上的文字:改用深色 + 同色描边把笔画加粗,对比度足够又不用面板遮挡画面
static void text_bold(lv_obj_t *o, uint32_t color, int32_t w) {
    if (!o) return;
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_outline_stroke_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_outline_stroke_width(o, w, 0);
    lv_obj_set_style_text_outline_stroke_opa(o, LV_OPA_COVER, 0);
}

static void xz_enter(void) {
    const app_theme_t *th = app_theme_current();
    s_scr = app_ui_screen_create("小智AI");
    s_shown = XZ_ST_IDLE;

    // screen_create 的第 0/1 个子对象是标题与电量,先取句柄(后面底图会被压到最底层)
    lv_obj_t *title = lv_obj_get_child(s_scr, 0);
    lv_obj_t *bat = lv_obj_get_child(s_scr, 1);

    // 1) 人像作全屏底图(240×320)——先置底,让标题栏/电池/字幕都叠在画面上
    s_stage = lv_obj_create(s_scr);
    lv_obj_set_size(s_stage, XZ_IMG_W, XZ_IMG_H);
    lv_obj_set_pos(s_stage, 0, 0);
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_stage, 0, 0);
    lv_obj_set_style_clip_corner(s_stage, true, 0);
    lv_obj_set_style_pad_all(s_stage, 0, 0);
    lv_obj_set_style_bg_opa(s_stage, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_stage, 0, 0);

    s_img = lv_image_create(s_stage);
    lv_image_set_src(s_img, &xz_portrait);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_move_background(s_stage); // 压到最底层,标题与电池保持在画面上可见

    // 状态光环:独立透明描边层铺满屏幕边缘,动画只改描边透明度(不重绘大图)
    s_glow = lv_obj_create(s_scr);
    lv_obj_set_size(s_glow, XZ_IMG_W, XZ_IMG_H);
    lv_obj_set_pos(s_glow, 0, 0);
    lv_obj_remove_flag(s_glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_glow, LV_OPA_0, 0);
    lv_obj_set_style_radius(s_glow, 0, 0);
    lv_obj_set_style_pad_all(s_glow, 0, 0);
    lv_obj_set_style_border_width(s_glow, 2, 0);
    lv_obj_set_style_border_color(s_glow, lv_color_hex(th->accent), 0);
    lv_obj_set_style_border_opa(s_glow, LV_OPA_30, 0);

    // 2) 状态文字:压在人物下方的浅蓝卫衣区(回复中隐藏,让位给声波)
    // 浅蓝卫衣上主题强调色(黄/粉/金)对比不足,改用深藏蓝 + 同色描边加粗
    s_state = app_ui_label(s_scr, "", app_font_cjk_16(), false);
    text_bold(s_state, 0x16324F, 1);
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 220);

    // 3) 声波条
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

    // 4) 对话文字:直接压在人物底部的干净渐变上,不加面板/边框/底色,与画面融为一体
    s_text = app_ui_label(s_scr, "", app_font_cjk_14(), false);
    lv_obj_set_size(s_text, 216, 36);
    lv_obj_set_pos(s_text, 12, 262);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(0x2E3A5A), 0); // 深蓝灰,浅蓝卫衣上清晰又不突兀

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    // 标题/电量/字幕都叠在彩色画面上,统一加半透明白色描边,保证可读又不遮挡人物
    text_outline(title);
    text_outline(bat);
    text_outline(s_text);
    // 底部提示也在浅蓝卫衣区,主题暗色偏灰,换成深灰 + 同色描边加粗
    text_bold(s_hint, 0x39445C, 1);

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
    if (s_glow) {
        lv_anim_delete(s_glow, anim_glow_opa);
    }
    for (int i = 0; i < BAR_NUM; i++) {
        if (s_bars[i]) lv_anim_delete(s_bars[i], anim_bar_h);
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_stage = s_img = s_glow = NULL;
        s_state = s_text = s_hint = NULL;
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
