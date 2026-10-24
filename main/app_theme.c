// main/app_theme.c
#include "app_theme.h"
#include "app_config.h"

static const app_theme_t THEMES[APP_THEME_COUNT] = {
    // name   bg       panel    border   text     accent   text_dim
    { "金属", 0x2B2F36, 0x3A4048, 0xC0C6CE, 0xE8EAED, 0xE5B80B, 0x9AA2AD },
    { "可爱", 0xFFE3EF, 0xFFFFFF, 0xFF9FC7, 0x6B2D45, 0xFF6FA5, 0xC77B9B },
    { "商务", 0x0F1B2D, 0x16263F, 0xB08D57, 0xE6E1D3, 0xC9A86A, 0x8A93A6 },
};

const app_theme_t *app_theme_get(uint8_t idx) {
    if (idx >= APP_THEME_COUNT) idx = 0;
    return &THEMES[idx];
}

const app_theme_t *app_theme_current(void) {
    return app_theme_get(app_config()->theme);
}
