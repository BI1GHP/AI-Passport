// main/main.c —— FoloToy AI Passport 应用入口 + 导航栈 + 按键分发。
// 按键 ADC 回调只入 FreeRTOS 队列,独立任务消费 → 调页面 key 回调。
// 导航栈:首页 → 菜单 → 子页(深度<=4)。OK 长按 = 全局返回上一级。
#include "badge_app.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "app_boot.h"
#include "app_config.h"
#include "app_fonts.h"
#include "app_net.h"
#include "app_time.h"
#include "app_store.h"
#include "app_weather.h"
#include "app_radio.h"
#include "app_recorder.h"
#include "app_mail.h"
#include "app_xiaozhi.h"
#include "bsp_audio.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

// ---- 音量条(全局覆盖层,调音量时短暂显示) ----
static lv_obj_t *s_vol_panel;
static lv_obj_t *s_vol_bar;
static lv_obj_t *s_vol_label;
static lv_timer_t *s_vol_timer;

static void vol_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_vol_panel) lv_obj_set_hidden(s_vol_panel, true);
}

static void vol_show(uint8_t v) {
    if (!bsp_lvgl_lock(300)) {
        ESP_LOGW(TAG, "音量条: LVGL 锁超时,未能显示");
        return;
    }
    if (!s_vol_panel) {
        s_vol_panel = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_vol_panel, 180, 44);
        lv_obj_align(s_vol_panel, LV_ALIGN_BOTTOM_MID, 0, -30);
        lv_obj_set_style_radius(s_vol_panel, 10, 0);
        // 深色底 + 高不透明度:浅色主题下白字/默认色底原先几乎看不见
        lv_obj_set_style_bg_color(s_vol_panel, lv_color_hex(0x111418), 0);
        lv_obj_set_style_bg_opa(s_vol_panel, LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_vol_panel, 0, 0);
        lv_obj_set_style_pad_all(s_vol_panel, 6, 0);
        lv_obj_set_scrollable(s_vol_panel, false);
        lv_obj_move_foreground(s_vol_panel);

        s_vol_label = lv_label_create(s_vol_panel);
        lv_obj_set_style_text_font(s_vol_label, app_font_cjk_14(), 0);
        lv_obj_set_style_text_color(s_vol_label, lv_color_white(), 0);
        lv_obj_align(s_vol_label, LV_ALIGN_TOP_MID, 0, 0);

        s_vol_bar = lv_bar_create(s_vol_panel);
        lv_obj_set_size(s_vol_bar, 160, 10);
        lv_obj_align(s_vol_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_bar_set_range(s_vol_bar, 0, 100);
        lv_obj_set_style_bg_opa(s_vol_bar, LV_OPA_30, 0);
        lv_obj_set_style_bg_color(s_vol_bar, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    }
    lv_bar_set_value(s_vol_bar, v, LV_ANIM_ON);
    char buf[16];
    snprintf(buf, sizeof(buf), "音量 %u%%", (unsigned)v);
    lv_label_set_text(s_vol_label, buf);
    lv_obj_set_hidden(s_vol_panel, false);
    if (s_vol_timer) lv_timer_delete(s_vol_timer);
    s_vol_timer = lv_timer_create(vol_timer_cb, 2000, NULL);
    bsp_lvgl_unlock();
}

// ---- 导航栈 ----
#define NAV_MAX 4
static const app_page_t *s_stack[NAV_MAX];
static int s_depth = 0;

static const app_page_t *page_top(void) {
    return s_depth > 0 ? s_stack[s_depth - 1] : NULL;
}

void app_nav_menu(void) {
    if (s_depth >= NAV_MAX) return;
    const app_page_t *cur = page_top();
    if (cur) {
        if (cur->stop) cur->stop();
        if (cur->exit && bsp_lvgl_lock(200)) { cur->exit(); bsp_lvgl_unlock(); }
    }
    s_stack[s_depth++] = page_menu();
    if (page_menu()->start) page_menu()->start();
    if (bsp_lvgl_lock(200)) { page_menu()->enter(); bsp_lvgl_unlock(); }
}

void app_nav_open(const app_page_t *page) {
    if (!page || s_depth >= NAV_MAX) return;
    const app_page_t *cur = page_top();
    if (cur) {
        if (cur->stop) cur->stop();
        if (cur->exit && bsp_lvgl_lock(200)) { cur->exit(); bsp_lvgl_unlock(); }
    }
    s_stack[s_depth++] = page;
    if (page->start) page->start();
    if (bsp_lvgl_lock(200)) { page->enter(); bsp_lvgl_unlock(); }
}

void app_nav_back(void) {
    if (s_depth <= 1) return; // 首页不能再退
    const app_page_t *cur = page_top();
    if (cur) {
        if (cur->stop) cur->stop();
        if (cur->exit && bsp_lvgl_lock(200)) { cur->exit(); bsp_lvgl_unlock(); }
    }
    s_depth--;
    const app_page_t *prev = page_top();
    if (prev) {
        if (prev->start) prev->start();
        if (bsp_lvgl_lock(200)) { prev->enter(); bsp_lvgl_unlock(); }
    }
}

// ---- 按键:ADC 回调入队,独立任务消费 ----
typedef struct { bsp_btn_t btn; bsp_btn_ev_t ev; } key_evt_t;
static QueueHandle_t s_key_q;

// 自定义双击检测:iot_button 的双击窗口=short_press_time(260ms)。
// 窗口取 350ms:既能覆盖稍慢的双击,又不会把连续快速导航(间隔通常>350ms)
// 误判为连击(900ms 窗口曾吞掉导航点击,实测复现)。仅对 UP/DOWN 生效(调音量)。
// 注意:首次单击仍会入队(触发页面导航),第二次在窗口内则转为 DOUBLE 调音量。
#define DOUBLE_CLICK_WINDOW_US  350000
static int64_t s_last_up_click_us;
static int64_t s_last_down_click_us;

static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // 忽略 RELEASE(深睡唤醒用)
    if (ev == BSP_BTN_RELEASE) return;

    // 双击检测分两条互斥路径,覆盖不同节奏:
    // 1) 快速双击(按下间隔 <260ms):iot_button 内部识别为原生 DOUBLE,此时不产生
    //    任何 CLICK 事件,直接透传 → 调音量。
    // 2) 较慢双击(260ms~350ms):iot_button 拆成两个独立 CLICK,由下方自定义窗口
    //    配对成 DOUBLE。两条路径天然互斥,不会重复触发。
    if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        int64_t now = esp_timer_get_time();
        int64_t *last = (btn == BSP_BTN_UP) ? &s_last_up_click_us : &s_last_down_click_us;
        if (now - *last < DOUBLE_CLICK_WINDOW_US) {
            // 第二次点击在窗口内 → 视为双击(调音量),不入队单击
            *last = 0; // 重置,避免三连击误触
            key_evt_t e = { .btn = btn, .ev = BSP_BTN_DOUBLE };
            if (s_key_q) xQueueSend(s_key_q, &e, 0);
            return;
        }
        *last = now;
    }

    key_evt_t e = { .btn = btn, .ev = ev };
    if (s_key_q) xQueueSend(s_key_q, &e, 0);
}

// 双击上/下:全局音量调节(±10),立即生效并保存。
static void volume_adjust(int delta) {
    app_config_t *c = app_config();
    int v = (int)c->volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    c->volume = (uint8_t)v;
    bsp_audio_set_volume(c->volume);
    app_config_save();
    vol_show(c->volume);
    ESP_LOGI(TAG, "音量 -> %u", (unsigned)c->volume);
}

static void input_task(void *arg) {
    (void)arg;
    key_evt_t e;
    while (1) {
        if (xQueueReceive(s_key_q, &e, portMAX_DELAY) == pdTRUE) {
            const app_page_t *cur = page_top();
            if (!cur) continue;

            // OK 长按 = 全局返回(在任何非首页)
            if (e.ev == BSP_BTN_LONG && e.btn == BSP_BTN_OK) {
                if (s_depth > 1) { app_nav_back(); continue; }
            }
            // 双击上/下 = 全局音量调节,不进入页面分发
            if (e.ev == BSP_BTN_DOUBLE && (e.btn == BSP_BTN_UP || e.btn == BSP_BTN_DOWN)) {
                volume_adjust(e.btn == BSP_BTN_UP ? 10 : -10);
                continue;
            }
            // 首页:OK 短按进菜单;其余按键交给首页 key(上/下切换名片内容)
            if (cur == page_home()) {
                if (e.btn == BSP_BTN_OK && e.ev == BSP_BTN_CLICK) {
                    app_nav_menu();
                } else if (cur->key) {
                    cur->key(e.btn, e.ev);
                }
                continue;
            }
            // 非首页:页面自定义 key 回调
            if (cur->key) {
                cur->key(e.btn, e.ev);
            }
        }
    }
}

// ---- 应用入口 ----
void app_main(void) {
    ESP_LOGI(TAG, "电子工牌启动");
    nvs_flash_init();
    // esp_netif_init / esp_event_loop_create_default 由 app_net_init 统一调用(幂等)
    bsp_i2c_init();
    bsp_battery_init();
    bsp_display_init();
    bsp_display_backlight(80);
    bsp_lvgl_init();
    app_boot_begin();          // 开机画面:logo + 启动进度条
    app_boot_progress(15, "加载配置");
    app_config_load();
    app_boot_progress(35, "挂载存储");
    app_store_init();
    app_boot_logo();           // 自定义 logo(门户已上传时淡入)
    app_boot_progress(60, "初始化音频");
    bsp_audio_init();      // I2S 音频:电台/录音共用
    app_weather_init();
    app_radio_init();
    app_recorder_init();
    app_mail_init();
    app_xiaozhi_init();
    app_boot_progress(85, "连接网络");
    app_net_init();   // 自动连已配 STA
    app_boot_progress(100, "启动完成");

    s_key_q = xQueueCreate(16, sizeof(key_evt_t));
    bsp_button_init(on_button, NULL);

    // 首页入栈
    s_stack[s_depth++] = page_home();
    if (bsp_lvgl_lock(500)) { page_home()->enter(); bsp_lvgl_unlock(); }
    app_boot_end();

    xTaskCreatePinnedToCore(input_task, "input", 4096, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "电子工牌就绪");
}
