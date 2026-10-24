// main/app_boot.c —— 开机画面:深色底 + logo(默认圆环文字,可门户上传替换)
// + accent 进度条(动画推进)+ 状态文字。logo 淡入为启动特效。
#include "app_boot.h"

#include "app_fonts.h"
#include "app_store.h"
#include "bsp_display.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

#define BOOT_BG     0x101418
#define BOOT_ACCENT 0x3A86FF
#define BOOT_TRACK  0x2A2F36

static lv_obj_t *s_scr;
static lv_obj_t *s_bar;
static lv_obj_t *s_msg;
static lv_obj_t *s_logo_box;
static bool s_logo_shown;

static void fade_anim_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void app_boot_begin(void) {
    if (!bsp_lvgl_lock(500)) return;
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(BOOT_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_scr, false);

    // logo 容器(120x120,居中偏上)
    s_logo_box = lv_obj_create(s_scr);
    lv_obj_set_size(s_logo_box, 120, 120);
    lv_obj_align(s_logo_box, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(s_logo_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_logo_box, 0, 0);
    lv_obj_set_style_pad_all(s_logo_box, 0, 0);
    lv_obj_set_scrollable(s_logo_box, false);

    // 默认 logo:accent 圆环 + "工牌"文字(自定义 logo 加载后替换)
    lv_obj_t *ring = lv_obj_create(s_logo_box);
    lv_obj_set_size(ring, 84, 84);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, 42, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(BOOT_ACCENT), 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_set_scrollable(ring, false);

    lv_obj_t *name = lv_label_create(ring);
    lv_obj_set_style_text_font(name, app_font_cjk_16(), 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(name, "工牌");
    lv_obj_center(name);

    // 进度条
    s_bar = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar, 180, 8);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 218);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(BOOT_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(BOOT_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    // 状态文字
    s_msg = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_msg, app_font_cjk_14(), 0);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(0x9AA4AF), 0);
    lv_label_set_text(s_msg, "正在启动");
    lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, 236);

    lv_screen_load(s_scr);
    bsp_lvgl_unlock();
}

void app_boot_progress(int pct, const char *msg) {
    if (!s_scr) return;
    if (!bsp_lvgl_lock(200)) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(s_bar, pct, LV_ANIM_ON);
    if (msg) lv_label_set_text(s_msg, msg);
    bsp_lvgl_unlock();
    vTaskDelay(pdMS_TO_TICKS(60)); // 让进度条动画在 LVGL 任务里跑一段
}

void app_boot_logo(void) {
    if (!s_scr || s_logo_shown || !app_store_ready()) return;
    char path[64];
    app_store_path(path, sizeof(path), "logo.jpg");
    FILE *f = fopen(path, "rb");
    if (!f) return; // 未上传,保留默认 logo
    fclose(f);
    if (!bsp_lvgl_lock(200)) return;
    lv_obj_clean(s_logo_box);
    lv_obj_t *img = lv_image_create(s_logo_box);
    lv_image_set_src(img, "S:/logo.jpg");
    lv_obj_center(img);
    // 淡入特效
    lv_obj_set_style_opa(img, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, img);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_exec_cb(&a, fade_anim_cb);
    lv_anim_start(&a);
    s_logo_shown = true;
    bsp_lvgl_unlock();
}

void app_boot_end(void) {
    if (!s_scr) return;
    if (bsp_lvgl_lock(200)) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_bar = s_msg = s_logo_box = NULL;
        bsp_lvgl_unlock();
    }
}
