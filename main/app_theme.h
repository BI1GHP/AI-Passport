// main/app_theme.h —— 三套皮肤主题(金属 / 可爱 / 商务)。
#pragma once

#include <stdint.h>

typedef struct {
    const char *name;  // 主题中文名
    uint32_t bg;       // 屏幕背景
    uint32_t panel;    // 面板背景
    uint32_t border;   // 面板边框
    uint32_t text;     // 正文
    uint32_t accent;   // 强调(选中/呼号/时间)
    uint32_t text_dim; // 次要文本
} app_theme_t;

#define APP_THEME_COUNT 3
#define APP_THEME_METAL 0
#define APP_THEME_CUTE 1
#define APP_THEME_BUSINESS 2

const app_theme_t *app_theme_current(void); // 跟随 app_config()->theme
const app_theme_t *app_theme_get(uint8_t idx);
