// main/page_radio.c —— 网络电台:内置精选台列表 → MP3 流播放。
// 播放页双风格(设置中切换):LED 风格(深底琥珀点光)/ 液晶风格(浅绿 LCD)。
// 共同元素:大频率数字 + 调谐表盘指针 + 8 频段频谱 + LIVE 脉冲。
#include "badge_app.h"

#include "app_config.h"
#include "app_fonts.h"
#include "app_net.h"
#include "app_radio.h"
#include "app_theme.h"
#include "app_ui.h"
#include "bsp_display.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define VIEW_LIST 1
#define VIEW_PLAYER 2
#define ROW_VISIBLE 7

// LED 风格配色
#define LED_BG 0x05070A
#define LED_AMBER 0xFFB000
#define LED_AMBER_DIM 0x5C3D00
// 液晶风格配色
#define LCD_BG 0xA8B435
#define LCD_INK 0x232B10
#define LCD_INK_DIM 0x5A6338

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ROW_VISIBLE];
static lv_obj_t *s_lbl[ROW_VISIBLE];
static lv_obj_t *s_status;
static lv_obj_t *s_player;
static lv_obj_t *s_p_name;
static lv_obj_t *s_p_freq;                // 大频率数字
static lv_obj_t *s_p_mhz;                 // "MHz" 单位
static lv_obj_t *s_p_state;
static lv_obj_t *s_live;                  // LIVE 红点(播放中脉冲)
static lv_obj_t *s_dial;                  // 调谐表盘轨道
static lv_obj_t *s_dial_tick[5];          // 表盘刻度
static lv_obj_t *s_dial_ptr;              // 表盘指针
static lv_obj_t *s_bars[APP_RADIO_SPEC_BANDS]; // 频谱柱
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;
static lv_timer_t *s_spec_timer;
static bool s_live_on;

static int s_view;
static int s_sel;
static int s_top;

// 真实 FM 频点表(按台名匹配,已按各台官方页面核对)。
// 频点用"十分位整数"存(103.9 → 1039):LVGL 未启用 LV_USE_FLOAT,
// lv_label_set_text_fmt 的 %f 不可用,必须走整数格式化。
// 未收录的台(全国性/网络音乐台)显示 "--.-" 并隐藏指针,不用虚假频率误导。
typedef struct {
    const char *name;
    uint16_t mhz10;
} fm_entry_t;

static const fm_entry_t FM_TABLE[] = {
    {"中国音乐之声", 900},        // 北京频点
    {"北京交通广播", 1039},
    {"北京音乐广播", 974},
    {"北京文艺广播", 876},
    {"北京新闻广播", 945},
    {"北京城市广播", 1073},
    {"京津冀之声", 1006},
    {"HIT FM 88.7", 887},
    {"上海新闻广播", 934},
    {"上海动感101", 1017},
    {"第一财经", 909},
    {"上海经典金曲", 1037},
    {"上海经典音乐", 947},
    {"珠江经济台", 974},
    {"羊城交通台", 1052},
    {"江苏经典音乐", 897},
    {"温州音乐之声", 1003},
    {"内蒙古音乐之声", 936},      // 呼和浩特频点
    {"安徽小说评书", 1074},       // 现安徽老年广播,合肥频点
    {"济南故事广播", 1043},
};

// 台名 → 频点(×10);无对应频点返回 0
static int station_mhz10(int idx) {
    app_radio_station_t st;
    if (!app_radio_station(idx, &st)) return 0;
    for (size_t i = 0; i < sizeof(FM_TABLE) / sizeof(FM_TABLE[0]); i++) {
        if (strcmp(FM_TABLE[i].name, st.name) == 0) return (int)FM_TABLE[i].mhz10;
    }
    return 0;
}

// ---- 统一风格:列表与播放页共用同一套 LED/液晶配色 ----
static uint32_t s_bg = LED_BG, s_ink = LED_AMBER, s_dim = LED_AMBER_DIM;
static bool s_led = true;

static void style_row(int i, bool selected) {
    const uint32_t row_bg = s_led ? (selected ? 0x1A1205 : 0x0B1016)
                                  : (selected ? 0xC6D151 : 0xB9C445);
    lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(row_bg), 0);
    lv_obj_set_style_bg_opa(s_rows[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_rows[i], lv_color_hex(selected ? s_ink : s_dim), 0);
    lv_obj_set_style_border_width(s_rows[i], selected ? 2 : 1, 0);
    lv_obj_set_style_radius(s_rows[i], s_led ? 4 : 0, 0);
    lv_obj_set_style_text_color(s_lbl[i], lv_color_hex(s_ink), 0);
}

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
        style_row(i, idx == s_sel);
    }

    if (list_view) {
        if (!app_net_sta_connected()) {
            lv_label_set_text(s_status, "Wi-Fi 未连接\n可浏览列表,播放需联网");
        } else {
            lv_label_set_text(s_status, "");
        }
        lv_label_set_text(s_hint, "上/下 选择电台\nOK 播放 · 双击上/下 调音量");
    } else {
        const int cur = app_radio_current();
        app_radio_station_t st;
        char buf[52] = "未知电台";
        const char *platform = "网络电台";
        if (app_radio_station(cur, &st)) {
            snprintf(buf, sizeof(buf), "%s", st.name);
            // 从 URL 判断来源平台
            if (strstr(st.url, "qtfm") || strstr(st.url, "qingting")) {
                platform = "蜻蜓FM";
            }
        }
        lv_label_set_text(s_p_name, buf);
        const int mhz10 = station_mhz10(cur);
        if (mhz10 > 0) {
            lv_label_set_text_fmt(s_p_freq, "%d.%d", mhz10 / 10, mhz10 % 10);
            // 表盘指针:875~1080(×10) → 轨道像素位置(x10..226)
            int px = 10 + (mhz10 - 875) * 216 / 205;
            lv_obj_set_x(s_dial_ptr, px);
            lv_obj_set_hidden(s_dial_ptr, false);
            lv_obj_set_hidden(s_p_mhz, false);
        } else {
            // 无官方频点的台:不显示假频率,指针也隐藏
            lv_label_set_text(s_p_freq, "--.-");
            lv_obj_set_hidden(s_dial_ptr, true);
            lv_obj_set_hidden(s_p_mhz, true);
        }
        const bool playing = app_radio_playing();
        lv_label_set_text_fmt(s_p_state, "%s · %s", platform,
                              playing ? "正在播放" : "连接中断");
        lv_obj_set_hidden(s_live, !playing);
        lv_label_set_text(s_hint, playing ? "上/下 切台 · OK 停止" : "上/下 切台 · OK 返回");
    }
}

// 频谱 + LIVE 脉冲(250ms):数据来自播放任务的 Goertzel 频段能量
static void spec_cb(lv_timer_t *t) {
    (void)t;
    if (!s_scr || s_view != VIEW_PLAYER) return;
    uint8_t bands[APP_RADIO_SPEC_BANDS];
    app_radio_spectrum(bands);
    for (int i = 0; i < APP_RADIO_SPEC_BANDS; i++) {
        // 叠加 3~10% 随机底噪:静音段落柱子也有微幅起伏,避免看起来"死"
        uint8_t floor = 3 + (uint8_t)(esp_random() % 8);
        uint8_t v = bands[i] > floor ? bands[i] : floor;
        lv_bar_set_value(s_bars[i], v, LV_ANIM_OFF);
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

// 按风格给播放页所有元素配色
static void apply_style(uint32_t bg, uint32_t ink, uint32_t dim, bool led) {
    lv_obj_set_style_bg_color(s_player, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(s_player, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_player, lv_color_hex(led ? LED_AMBER_DIM : LCD_INK_DIM), 0);
    lv_obj_set_style_text_color(s_p_name, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(s_p_freq, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(s_p_mhz, lv_color_hex(dim), 0);
    lv_obj_set_style_text_color(s_p_state, lv_color_hex(dim), 0);
    lv_obj_set_style_bg_color(s_dial, lv_color_hex(dim), 0);
    for (int i = 0; i < 5; i++) {
        lv_obj_set_style_bg_color(s_dial_tick[i], lv_color_hex(dim), 0);
    }
    lv_obj_set_style_bg_color(s_dial_ptr, lv_color_hex(ink), 0);
    for (int i = 0; i < APP_RADIO_SPEC_BANDS; i++) {
        // LED:琥珀柱 + 暗琥珀轨道;液晶:墨色柱 + 浅底轨道
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(led ? bg : 0x8F9A2C), 0);
        lv_obj_set_style_bg_opa(s_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(ink), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_bars[i], led ? 2 : 0, 0);
        lv_obj_set_style_radius(s_bars[i], led ? 2 : 0, LV_PART_INDICATOR);
    }
}

static void radio_enter(void) {
    s_scr = app_ui_screen_create("网络电台");
    // 整个电台页(含列表、标题、提示)统一用 LED/液晶配色
    const bool led = app_config()->radio_style == 0;
    s_led = led;
    s_bg = led ? LED_BG : LCD_BG;
    s_ink = led ? LED_AMBER : LCD_INK;
    s_dim = led ? LED_AMBER_DIM : LCD_INK_DIM;

    lv_obj_set_style_bg_color(s_scr, lv_color_hex(s_bg), 0);
    // 标题与电池标签由 app_ui_screen_create 创建(第 0/1 个子对象)
    lv_obj_t *title = lv_obj_get_child(s_scr, 0);
    lv_obj_t *bat = lv_obj_get_child(s_scr, 1);
    if (title) lv_obj_set_style_text_color(title, lv_color_hex(s_ink), 0);
    if (bat) lv_obj_set_style_text_color(bat, lv_color_hex(s_dim), 0);

    for (int i = 0; i < ROW_VISIBLE; i++) {
        s_rows[i] = app_ui_panel(s_scr, 224, 30);
        lv_obj_set_pos(s_rows[i], 8, 40 + i * 33);
        s_lbl[i] = app_ui_label(s_rows[i], "", app_font_cjk_14(), false);
        lv_obj_set_width(s_lbl[i], 208);
        lv_label_set_long_mode(s_lbl[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_center(s_lbl[i]);
    }

    s_status = app_ui_hint(s_scr);
    lv_obj_set_width(s_status, 224);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_color(s_status, lv_color_hex(s_dim), 0);

    // ---- 播放页:整屏铺满(240 宽,标题下到底部) ----
    s_player = app_ui_panel(s_scr, 240, 258);
    lv_obj_set_pos(s_player, 0, 32);
    lv_obj_set_style_pad_all(s_player, 0, 0);
    lv_obj_set_style_radius(s_player, 0, 0);
    lv_obj_set_style_border_width(s_player, 0, 0);

    s_p_name = app_ui_label(s_player, "", app_font_cjk_14(), false); // 14px 全字集(任意台名)
    lv_obj_set_width(s_p_name, 220);
    lv_label_set_long_mode(s_p_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_p_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_p_name, LV_ALIGN_TOP_MID, 0, 10);

    // 大频率数字 + MHz + LIVE
    s_p_freq = lv_label_create(s_player);
    lv_obj_set_style_text_font(s_p_freq, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_p_freq, "87.5");
    lv_obj_set_pos(s_p_freq, 16, 36);

    s_p_mhz = lv_label_create(s_player);
    lv_obj_set_style_text_font(s_p_mhz, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_p_mhz, "MHz");
    lv_obj_set_pos(s_p_mhz, 150, 68);

    s_live = lv_obj_create(s_player);
    lv_obj_set_size(s_live, 8, 8);
    lv_obj_set_style_radius(s_live, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_live, lv_color_hex(0xE84040), 0);
    lv_obj_set_style_border_width(s_live, 0, 0);
    lv_obj_set_style_pad_all(s_live, 0, 0);
    lv_obj_set_scrollable(s_live, false);
    lv_obj_set_pos(s_live, 214, 52);

    // 调谐表盘:轨道(x10..230) + 5 刻度 + 指针
    s_dial = lv_obj_create(s_player);
    lv_obj_remove_style_all(s_dial);
    lv_obj_set_size(s_dial, 220, 2);
    lv_obj_set_pos(s_dial, 10, 112);
    lv_obj_set_style_bg_opa(s_dial, LV_OPA_COVER, 0);
    for (int i = 0; i < 5; i++) {
        s_dial_tick[i] = lv_obj_create(s_player);
        lv_obj_remove_style_all(s_dial_tick[i]);
        lv_obj_set_size(s_dial_tick[i], 1, 6);
        lv_obj_set_pos(s_dial_tick[i], 10 + i * 55, 108);
        lv_obj_set_style_bg_opa(s_dial_tick[i], LV_OPA_COVER, 0);
    }
    s_dial_ptr = lv_obj_create(s_player);
    lv_obj_remove_style_all(s_dial_ptr);
    lv_obj_set_size(s_dial_ptr, 4, 12);
    lv_obj_set_pos(s_dial_ptr, 10, 106);
    lv_obj_set_style_bg_opa(s_dial_ptr, LV_OPA_COVER, 0);

    // 频谱:8 根竖柱铺满宽度(20 宽 + 8 间距 = 216)
    for (int i = 0; i < APP_RADIO_SPEC_BANDS; i++) {
        s_bars[i] = lv_bar_create(s_player);
        lv_obj_set_size(s_bars[i], 20, 110);
        lv_obj_set_pos(s_bars[i], 12 + i * 28, 130);
        lv_bar_set_range(s_bars[i], 0, 100);
    }

    s_p_state = app_ui_label(s_player, "", app_font_cjk_14(), false);
    lv_obj_set_style_text_align(s_p_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_p_state, 224);
    lv_obj_align(s_p_state, LV_ALIGN_TOP_MID, 0, 246);

    if (led) {
        apply_style(LED_BG, LED_AMBER, LED_AMBER_DIM, true);
    } else {
        apply_style(LCD_BG, LCD_INK, LCD_INK_DIM, false);
    }

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(s_dim), 0);

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
        s_status = s_player = s_p_name = s_p_freq = s_p_mhz = s_p_state = s_live = s_hint = NULL;
        s_dial = s_dial_ptr = NULL;
        for (int i = 0; i < 5; i++) s_dial_tick[i] = NULL;
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
        if (btn == BSP_BTN_OK) {
            // 停止并返回列表
            bsp_lvgl_unlock();
            app_radio_stop();
            if (bsp_lvgl_lock(200)) {
                open_list();
                bsp_lvgl_unlock();
            }
            return;
        }
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            // 切换上/下一个电台:停播→等旧任务退出→起播,不持 LVGL 锁(停播阻塞)
            const int total = app_radio_count();
            if (total <= 0) { bsp_lvgl_unlock(); return; }
            int cur = app_radio_current();
            if (cur < 0) cur = s_sel;
            int next = (cur + (btn == BSP_BTN_UP ? -1 : 1) + total) % total;
            s_sel = next;
            bsp_lvgl_unlock();
            app_radio_stop();
            // 旧任务可能正阻塞在 HTTP 读,等待 s_playing 真正清零(最多 1.5s)
            for (int i = 0; i < 15 && app_radio_playing(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            esp_err_t e = app_radio_play(next);
            if (bsp_lvgl_lock(200)) {
                if (e == ESP_ERR_INVALID_STATE && !app_net_sta_connected()) {
                    lv_label_set_text(s_status, "Wi-Fi 未连接");
                } else if (e != ESP_OK) {
                    lv_label_set_text(s_status, "播放失败,按 OK 返回");
                }
                refresh();
                bsp_lvgl_unlock();
            }
            return;
        }
        bsp_lvgl_unlock();
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
