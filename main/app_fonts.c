// main/app_fonts.c
#include "app_fonts.h"

#include "esp_log.h"

static const char *TAG = "app_fonts";

// 由 tools/gen_badge_font.py 生成,只含 main/ 源码里实际出现的中文字形。
extern const lv_font_t badge_font_cjk_14;
extern const lv_font_t badge_font_cjk_16;

const lv_font_t *app_font_cjk_14(void) {
    return &badge_font_cjk_14;
}

const lv_font_t *app_font_cjk_16(void) {
    return &badge_font_cjk_16;
}

static int probe_font(const lv_font_t *font) {
    // 电子工牌呼号菜单设置关于姓名昵称公司部门天气建设返回
    static const uint32_t probe[] = {
        0x7535, 0x5B50, 0x5DE5, 0x724C, 0x547C, 0x53F7, 0x83DC, 0x5355,
        0x8BBE, 0x7F6E, 0x5173, 0x4E8E, 0x59D3, 0x540D, 0x6635, 0x79F0,
        0x516C, 0x53F8, 0x90E8, 0x95E8, 0x5929, 0x6C14, 0x5EFA, 0x8BBE,
        0x8FD4, 0x56DE,
    };
    int missing = 0;
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
        lv_font_glyph_dsc_t dsc;
        if (!lv_font_get_glyph_dsc(font, &dsc, probe[i], 0)) missing++;
    }
    return missing;
}

void app_fonts_init(void) {
    int m14 = probe_font(app_font_cjk_14());
    int m16 = probe_font(app_font_cjk_16());
    if (m14 || m16) {
        ESP_LOGW(TAG, "中文字形缺失:14号缺%d 16号缺%d(重新运行 tools/gen_badge_font.py)",
                 m14, m16);
    } else {
        ESP_LOGI(TAG, "中文字体 glyph 校验通过");
    }
}
