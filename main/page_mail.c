// main/page_mail.c —— 邮箱:后台 IMAP 收取最新 5 封邮件头,列表与详情。
#include "badge_app.h"

#include "app_fonts.h"
#include "app_mail.h"
#include "app_ui.h"
#include "bsp_display.h"

#include <stdio.h>
#include <string.h>

#define ROW_COUNT 5
#define STATE_LIST 0
#define STATE_DETAIL 1

static lv_obj_t *s_scr;
static lv_obj_t *s_status;
static lv_obj_t *s_rows[ROW_COUNT];
static lv_obj_t *s_from[ROW_COUNT];
static lv_obj_t *s_subj[ROW_COUNT];
static lv_obj_t *s_detail;
static lv_obj_t *s_d_from;
static lv_obj_t *s_d_subj;
static lv_obj_t *s_d_date;
static lv_obj_t *s_del_hint;
static lv_timer_t *s_poll;

static imap_msg_t s_msgs[ROW_COUNT];
static int s_count;
static int s_sel;
static int s_state;
static esp_err_t s_start_err = ESP_OK;

static void update_rows(void) {
    for (int i = 0; i < ROW_COUNT; i++) {
        if (i < s_count) {
            lv_obj_set_hidden(s_rows[i], false);
            lv_label_set_text(s_from[i], s_msgs[i].from);
            lv_label_set_text(s_subj[i], s_msgs[i].subject[0] ? s_msgs[i].subject : "(无主题)");
            app_ui_row_style(s_rows[i], s_subj[i], i == s_sel);
        } else {
            lv_obj_set_hidden(s_rows[i], true);
        }
    }
}

static void show_detail(void) {
    if (s_sel < 0 || s_sel >= s_count) return;
    s_state = STATE_DETAIL;
    lv_obj_set_hidden(s_rows[0], true);
    for (int i = 0; i < ROW_COUNT; i++) lv_obj_set_hidden(s_rows[i], true);
    lv_obj_set_hidden(s_status, true);
    lv_obj_set_hidden(s_del_hint, true);
    lv_obj_set_hidden(s_detail, false);
    lv_label_set_text(s_d_from, s_msgs[s_sel].from);
    lv_label_set_text(s_d_subj, s_msgs[s_sel].subject[0] ? s_msgs[s_sel].subject : "(无主题)");
    lv_label_set_text(s_d_date, s_msgs[s_sel].date[0] ? s_msgs[s_sel].date : "(无日期)");
}

static void show_list(void) {
    s_state = STATE_LIST;
    lv_obj_set_hidden(s_detail, true);
    lv_obj_set_hidden(s_status, false);
    lv_obj_set_hidden(s_del_hint, false);
    update_rows();
}

static void delete_selected(void) {
    if (s_count <= 0) return;
    app_mail_delete(s_sel);
    esp_err_t err = ESP_OK;
    s_count = app_mail_copy(s_msgs, ROW_COUNT, &err);
    (void)err;
    if (s_sel >= s_count) s_sel = s_count > 0 ? s_count - 1 : 0;
    show_list();
    lv_label_set_text(s_status, s_count == 0 ? "收件箱为空" : "");
}

static void poll_cb(lv_timer_t *t) {
    (void)t;
    if (s_state == STATE_DETAIL) return;
    if (s_start_err == ESP_ERR_INVALID_STATE && !app_mail_configured()) {
        lv_label_set_text(s_status, "未配置邮箱\n请在配网门户填写账号与授权码");
        s_count = 0;
        update_rows();
        return;
    }
    if (app_mail_busy()) {
        lv_label_set_text(s_status, "正在收取邮件…");
        return;
    }
    imap_msg_t msgs[ROW_COUNT];
    esp_err_t err = ESP_OK;
    int n = app_mail_copy(msgs, ROW_COUNT, &err);
    if (err != ESP_OK || s_start_err != ESP_OK) {
        lv_label_set_text(s_status, "收取失败\n请检查 Wi-Fi、邮箱账号与授权码");
        s_count = 0;
        update_rows();
        return;
    }
    s_count = n;
    if (n > 0) memcpy(s_msgs, msgs, (size_t)n * sizeof(imap_msg_t));
    if (n == 0) {
        lv_label_set_text(s_status, "收件箱为空");
    } else {
        lv_label_set_text(s_status, "");
    }
    if (s_sel >= s_count) s_sel = s_count > 0 ? s_count - 1 : 0;
    update_rows();
}

static void mail_enter(void) {
    s_scr = app_ui_screen_create("邮箱");
    s_count = 0;
    s_sel = 0;
    s_state = STATE_LIST;

    s_status = app_ui_label(s_scr, "", app_font_cjk_14(), true);
    lv_obj_set_width(s_status, 210);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 38);

    for (int i = 0; i < ROW_COUNT; i++) {
        s_rows[i] = app_ui_panel(s_scr, 210, 40);
        lv_obj_set_pos(s_rows[i], 15, 62 + i * 44);
        s_from[i] = app_ui_label(s_rows[i], "", app_font_cjk_14(), true);
        lv_obj_set_width(s_from[i], 190);
        lv_label_set_long_mode(s_from[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_from[i], LV_ALIGN_TOP_LEFT, 4, 2);
        s_subj[i] = app_ui_label(s_rows[i], "", app_font_cjk_14(), false);
        lv_obj_set_width(s_subj[i], 190);
        lv_label_set_long_mode(s_subj[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_subj[i], LV_ALIGN_TOP_LEFT, 4, 18);
    }

    s_del_hint = app_ui_label(s_scr, "长按上/下键删除本地邮件", app_font_cjk_14(), true);
    lv_obj_set_width(s_del_hint, 224);
    lv_obj_set_style_text_align(s_del_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_del_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_detail = app_ui_panel(s_scr, 214, 236);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_pad_all(s_detail, 8, 0);
    lv_obj_t *k1 = app_ui_label(s_detail, "发件人", app_font_cjk_14(), true);
    lv_obj_align(k1, LV_ALIGN_TOP_LEFT, 0, 0);
    s_d_from = app_ui_label(s_detail, "", app_font_cjk_14(), false);
    lv_obj_set_width(s_d_from, 190);
    lv_label_set_long_mode(s_d_from, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_d_from, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_t *k2 = app_ui_label(s_detail, "主题", app_font_cjk_14(), true);
    lv_obj_align(k2, LV_ALIGN_TOP_LEFT, 0, 62);
    s_d_subj = app_ui_label(s_detail, "", app_font_cjk_14(), false);
    lv_obj_set_width(s_d_subj, 190);
    lv_label_set_long_mode(s_d_subj, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_d_subj, LV_ALIGN_TOP_LEFT, 0, 84);
    lv_obj_t *k3 = app_ui_label(s_detail, "时间", app_font_cjk_14(), true);
    lv_obj_align(k3, LV_ALIGN_TOP_LEFT, 0, 150);
    s_d_date = app_ui_label(s_detail, "", app_font_cjk_14(), true);
    lv_obj_set_width(s_d_date, 190);
    lv_label_set_long_mode(s_d_date, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_d_date, LV_ALIGN_TOP_LEFT, 0, 172);
    lv_obj_t *tip = app_ui_label(s_detail, "OK 返回  长按上/下删除", app_font_cjk_14(), true);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_detail, true);

    s_poll = lv_timer_create(poll_cb, 500, NULL);
    poll_cb(s_poll);
    lv_screen_load(s_scr);
}

static void mail_exit(void) {
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_status = s_detail = NULL;
        s_del_hint = NULL;
        memset(s_rows, 0, sizeof(s_rows));
    }
}

static esp_err_t mail_start(void) {
    s_start_err = app_mail_start();
    return s_start_err;
}

static esp_err_t mail_stop(void) {
    app_mail_cancel();
    return ESP_OK;
}

static void mail_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 长按上/下:删除当前选中邮件的本地快照(不改服务器)。
    if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        if (bsp_lvgl_lock(200)) {
            delete_selected();
            bsp_lvgl_unlock();
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (s_state == STATE_DETAIL) {
        if (btn == BSP_BTN_OK && bsp_lvgl_lock(200)) {
            show_list();
            bsp_lvgl_unlock();
        }
        return;
    }
    if (btn == BSP_BTN_OK) {
        if (s_count > 0 && bsp_lvgl_lock(200)) {
            show_detail();
            bsp_lvgl_unlock();
        }
        return;
    }
    if (s_count <= 0) return;
    if (bsp_lvgl_lock(200)) {
        if (btn == BSP_BTN_UP) {
            s_sel = (s_sel + s_count - 1) % s_count;
        } else if (btn == BSP_BTN_DOWN) {
            s_sel = (s_sel + 1) % s_count;
        }
        update_rows();
        bsp_lvgl_unlock();
    }
}

static const app_page_t PAGE = {
    .name = "邮箱",
    .enter = mail_enter,
    .exit = mail_exit,
    .key = mail_key,
    .start = mail_start,
    .stop = mail_stop,
};

const app_page_t *page_mail(void) {
    return &PAGE;
}
