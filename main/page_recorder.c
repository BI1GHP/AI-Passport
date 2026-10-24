// main/page_recorder.c —— 录音:开始/停止录音(WAV 存卡)与回放录音列表。
#include "badge_app.h"

#include "app_fonts.h"
#include "app_recorder.h"
#include "app_theme.h"
#include "app_ui.h"
#include "bsp_display.h"

#include <stdio.h>
#include <string.h>

#define ROW_VISIBLE 7
#define REC_MAX 16
#define LIST_PATH_LEN 64

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ROW_VISIBLE];
static lv_obj_t *s_lbl[ROW_VISIBLE];
static lv_obj_t *s_active;
static lv_obj_t *s_a_title;
static lv_obj_t *s_a_big;
static lv_obj_t *s_a_note;
static lv_obj_t *s_hint;
static lv_timer_t *s_poll;

static char s_names[REC_MAX][APP_REC_NAME_LEN];
static int s_count;      // 录音文件数
static int s_sel;        // 0 = 开始录音,1..count = 文件
static int s_top;
static bool s_was_active;
static bool s_confirm;   // 已选中某文件并进入"确认删除"状态

static int item_count(void) {
    return s_count + 1;
}

static void text_at(int index, char *buf, size_t size) {
    if (index == 0) {
        snprintf(buf, size, "开始录音");
    } else {
        snprintf(buf, size, "%s", s_names[index - 1]);
    }
}

static void reload_list(void) {
    s_count = app_recorder_list(s_names, REC_MAX);
    if (s_sel >= item_count()) s_sel = item_count() - 1;
    if (s_sel < 0) s_sel = 0;
    s_top = 0;
    if (s_sel >= s_top + ROW_VISIBLE) s_top = s_sel - ROW_VISIBLE + 1;
}

static void refresh(void) {
    const bool recording = app_recorder_recording();
    const bool playing = app_recorder_playing();
    const bool active = recording || playing;

    if (!active && s_was_active) reload_list();
    s_was_active = active;

    if (active) {
        lv_obj_set_hidden(s_rows[0], true);
        for (int i = 1; i < ROW_VISIBLE; i++) lv_obj_set_hidden(s_rows[i], true);
        lv_obj_set_hidden(s_active, false);
        lv_label_set_text(s_a_title, recording ? "录音中" : "播放中");
        if (recording) {
            const uint32_t sec = app_recorder_seconds();
            lv_label_set_text_fmt(s_a_big, "%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
            lv_label_set_text_fmt(s_a_note, "最长 %u 秒", (unsigned)60);
        } else {
            lv_label_set_text(s_a_big, "");
            lv_label_set_text(s_a_note, "正在回放录音");
        }
        lv_label_set_text(s_hint, recording ? "OK 停止录音" : "OK 停止播放");
        return;
    }

    lv_obj_set_hidden(s_active, true);

    const int total = item_count();
    for (int i = 0; i < ROW_VISIBLE; i++) {
        const int idx = s_top + i;
        if (idx >= total) {
            lv_obj_set_hidden(s_rows[i], true);
            continue;
        }
        lv_obj_set_hidden(s_rows[i], false);
        char buf[APP_REC_NAME_LEN];
        text_at(idx, buf, sizeof(buf));
        lv_label_set_text(s_lbl[i], buf);
        app_ui_row_style(s_rows[i], s_lbl[i], idx == s_sel);
    }
    lv_label_set_text(s_hint, s_confirm ? "确认删除? OK 删除 · UP 取消"
                                        : "上/下 选择 · OK 录音/播放 · 长按下 删除");
}

static void poll_cb(lv_timer_t *t) {
    (void)t;
    refresh();
}

static void recorder_enter(void) {
    s_scr = app_ui_screen_create("录音");
    s_sel = 0;
    s_top = 0;
    s_was_active = false;
    s_confirm = false;

    for (int i = 0; i < ROW_VISIBLE; i++) {
        s_rows[i] = app_ui_panel(s_scr, 210, 28);
        lv_obj_set_pos(s_rows[i], 15, 42 + i * 31);
        s_lbl[i] = app_ui_label(s_rows[i], "", app_font_cjk_14(), false);
        lv_obj_set_width(s_lbl[i], 190);
        lv_label_set_long_mode(s_lbl[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_center(s_lbl[i]);
    }

    const app_theme_t *th = app_theme_current();
    s_active = app_ui_panel(s_scr, 210, 120);
    lv_obj_align(s_active, LV_ALIGN_TOP_MID, 0, 86);
    s_a_title = app_ui_label(s_active, "", app_font_cjk_16(), false);
    lv_obj_set_style_text_color(s_a_title, lv_color_hex(th->accent), 0);
    lv_obj_align(s_a_title, LV_ALIGN_TOP_MID, 0, 14);
    s_a_big = app_ui_label(s_active, "", &lv_font_montserrat_28, false);
    lv_obj_align(s_a_big, LV_ALIGN_CENTER, 0, -6);
    s_a_note = app_ui_label(s_active, "", app_font_cjk_14(), true);
    lv_obj_align(s_a_note, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_hidden(s_active, true);

    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    reload_list();
    s_poll = lv_timer_create(poll_cb, 250, NULL);
    refresh();
    lv_screen_load(s_scr);
}

static void recorder_exit(void) {
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_active = s_a_title = s_a_big = s_a_note = s_hint = NULL;
    }
}

static esp_err_t recorder_stop(void) {
    return app_recorder_stop();
}

static void move_sel(int delta) {
    const int total = item_count();
    if (total <= 0) return;
    s_sel = (s_sel + delta + total) % total;
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + ROW_VISIBLE) s_top = s_sel - ROW_VISIBLE + 1;
}

static void recorder_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (app_recorder_recording() || app_recorder_playing()) {
        if (ev != BSP_BTN_CLICK || btn != BSP_BTN_OK) return;
        app_recorder_stop(); // 阻塞等待任务退出,不持 LVGL 锁
        if (bsp_lvgl_lock(200)) {
            reload_list();
            refresh();
            bsp_lvgl_unlock();
        }
        return;
    }

    // 删除确认态:OK 删除,UP 取消,其它键忽略(避免长按松开后的残留事件误取消)。
    if (s_confirm) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_DOWN) return;
        const bool do_delete = (btn == BSP_BTN_OK);
        s_confirm = false;
        if (do_delete && s_sel > 0) app_recorder_delete(s_names[s_sel - 1]);
        if (bsp_lvgl_lock(200)) {
            reload_list();
            refresh();
            bsp_lvgl_unlock();
        }
        return;
    }

    // 长按下 = 删除选中录音(需再按 OK 确认,避免误删)。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN && s_sel > 0) {
        if (bsp_lvgl_lock(200)) {
            s_confirm = true;
            refresh();
            bsp_lvgl_unlock();
        }
        return;
    }

    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        esp_err_t e;
        if (s_sel == 0) {
            e = app_recorder_start();
        } else {
            char path[LIST_PATH_LEN];
            snprintf(path, sizeof(path), "/badge/rec/%s", s_names[s_sel - 1]);
            e = app_recorder_play(path);
        }
        if (e != ESP_OK) return;
        if (bsp_lvgl_lock(200)) {
            refresh();
            bsp_lvgl_unlock();
        }
        return;
    }

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (bsp_lvgl_lock(200)) {
            move_sel(btn == BSP_BTN_UP ? -1 : 1);
            refresh();
            bsp_lvgl_unlock();
        }
    }
}

static const app_page_t PAGE = {
    .name = "录音",
    .enter = recorder_enter,
    .exit = recorder_exit,
    .key = recorder_key,
    .stop = recorder_stop,
};

const app_page_t *page_recorder(void) {
    return &PAGE;
}
