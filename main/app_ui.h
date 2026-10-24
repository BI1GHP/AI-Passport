// main/app_ui.h —— 主题化 UI 构件(240x320,圆角遮罩由 BSP 应用)。
#pragma once

#include "lvgl.h"
#include <stdbool.h>

#define APP_LCD_W 240
#define APP_LCD_H 320

// 主题背景屏幕;title 为 NULL 时不画标题(首页工牌)。右上角固定显示电池百分比。
lv_obj_t *app_ui_screen_create(const char *title);

// 主题圆角面板。
lv_obj_t *app_ui_panel(lv_obj_t *parent, int w, int h);

// 主题文本标签(dim=次要颜色)。中文文本必须传 CJK 字体。
lv_obj_t *app_ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, bool dim);

// 菜单行选中态。
void app_ui_row_style(lv_obj_t *panel, lv_obj_t *label, bool selected);

// 底部说明提示标签:全宽、多行自动换行、居中,保证提示完整显示不截断。
lv_obj_t *app_ui_hint(lv_obj_t *parent);
