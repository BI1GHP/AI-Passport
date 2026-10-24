// main/app_fonts.h —— 中文字体挂载(思源黑体 CJK 子集)。
#pragma once

#include "lvgl.h"

const lv_font_t *app_font_cjk_14(void); // 正文(中文)
const lv_font_t *app_font_cjk_16(void); // 标题/姓名(中文)

// 启动时校验关键汉字 glyph 覆盖,缺失打警告日志(Montserrat 无 CJK 字形)。
void app_fonts_init(void);
