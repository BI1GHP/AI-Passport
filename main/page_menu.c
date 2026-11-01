// main/page_menu.c —— 主菜单:六个功能板块 + 设置 + 关于(金属科技风)。
#include "badge_app.h"

#include "app_fonts.h"
#include "app_theme.h"
#include "app_ui.h"
#include "bsp_display.h"

#define ITEM_COUNT 7

static const char *NAMES[ITEM_COUNT] = {
    "数字名片", "邮箱", "小智AI", "网络电台", "录音", "设置", "关于",
};

static const app_page_t *get_card(void)     { return page_card(); }
static const app_page_t *get_mail(void)     { return page_mail(); }
static const app_page_t *get_xiaozhi(void)  { return page_xiaozhi(); }
static const app_page_t *get_radio(void)    { return page_radio(); }
static const app_page_t *get_recorder(void) { return page_recorder(); }
static const app_page_t *get_settings(void) { return page_settings(); }

typedef const app_page_t *(*get_fn)(void);
static const get_fn GETTERS[ITEM_COUNT] = {
    get_card, get_mail, get_xiaozhi,
    get_radio, get_recorder, get_settings, page_about,
};

static lv_obj_t *s_scr;
static lv_obj_t *s_rows[ITEM_COUNT];
static lv_obj_t *s_lbl[ITEM_COUNT];
static lv_obj_t *s_mark[ITEM_COUNT];  // 选中行左侧指示条
static int s_sel;

// 颜色微调:delta 正=变亮,负=变暗
static lv_color_t shade(uint32_t hex, int delta) {
    int r = (int)((hex >> 16) & 0xFF) + delta;
    int g = (int)((hex >> 8) & 0xFF) + delta;
    int b = (int)(hex & 0xFF) + delta;
    return lv_color_make(LV_CLAMP(0, r, 255), LV_CLAMP(0, g, 255), LV_CLAMP(0, b, 255));
}

// 内置符号图标(FontAwesome 字形,montserrat_14 自带)
static void icon_symbol(lv_obj_t *row, const char *sym, lv_color_t c) {
    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 9, 0);
}

// 18x18 自绘图标容器(透明、不可滚动)
static lv_obj_t *icon_box(lv_obj_t *row) {
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 18, 18);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 8, 0);
    return b;
}

// 小方块(自绘图标零件)
static void part(lv_obj_t *box, int x, int y, int w, int h, int radius, lv_color_t c) {
    lv_obj_t *o = lv_obj_create(box);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
}

// 小智AI:机器人头
static void icon_robot(lv_obj_t *box, lv_color_t c) {
    part(box, 8, 1, 2, 3, 0, c);                 // 天线
    part(box, 7, 0, 4, 2, 1, c);                 // 天线头
    lv_obj_t *head = lv_obj_create(box);         // 头:12x9 描边
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 12, 9);
    lv_obj_set_pos(head, 3, 5);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, c, 0);
    lv_obj_set_style_radius(head, 2, 0);
    part(box, 6, 8, 2, 2, 0, c);                 // 左眼
    part(box, 10, 8, 2, 2, 0, c);                // 右眼
}

// 录音:麦克风
static void icon_mic(lv_obj_t *box, lv_color_t c) {
    lv_obj_t *cap = lv_obj_create(box);          // 麦克头:6x9 描边胶囊
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 6, 9);
    lv_obj_set_pos(cap, 6, 1);
    lv_obj_set_style_border_width(cap, 1, 0);
    lv_obj_set_style_border_color(cap, c, 0);
    lv_obj_set_style_radius(cap, 3, 0);
    static const lv_point_precise_t u[4] = { {3, 6}, {3, 11}, {15, 11}, {15, 6} };
    lv_obj_t *hold = lv_line_create(box);        // U 形支架
    lv_line_set_points((lv_obj_t *)hold, (const lv_point_precise_t *)u, 4);
    lv_obj_set_style_line_width((lv_obj_t *)hold, 1, 0);
    lv_obj_set_style_line_color((lv_obj_t *)hold, c, 0);
    part(box, 8, 12, 2, 3, 0, c);                // 立杆
    part(box, 5, 15, 8, 2, 1, c);                // 底座
}

// 关于:圆圈 i
static void icon_info(lv_obj_t *box, lv_color_t c) {
    lv_obj_t *ring = lv_obj_create(box);         // 外圈
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 14, 14);
    lv_obj_set_pos(ring, 2, 2);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, c, 0);
    lv_obj_set_style_radius(ring, 7, 0);
    part(box, 8, 5, 2, 2, 1, c);                 // 点
    part(box, 8, 9, 2, 5, 0, c);                 // 竖
}

// 按菜单项序号装配图标,统一 accent 色
static void add_icon(int i, lv_obj_t *row) {
    const lv_color_t c = lv_color_hex(app_theme_current()->accent);
    switch (i) {
        case 0: icon_symbol(row, LV_SYMBOL_LIST, c); break;      // 数字名片
        case 1: icon_symbol(row, LV_SYMBOL_ENVELOPE, c); break;  // 邮箱
        case 2: icon_robot(icon_box(row), c); break;             // 小智AI
        case 3: icon_symbol(row, LV_SYMBOL_AUDIO, c); break;     // 网络电台
        case 4: icon_mic(icon_box(row), c); break;               // 录音
        case 5: icon_symbol(row, LV_SYMBOL_SETTINGS, c); break;  // 设置
        default: icon_info(icon_box(row), c); break;             // 关于
    }
}

static void refresh(void) {
    const app_theme_t *th = app_theme_current();
    for (int i = 0; i < ITEM_COUNT; i++) {
        const bool sel = i == s_sel;
        app_ui_row_style(s_rows[i], s_lbl[i], sel);
        lv_obj_set_hidden(s_mark[i], !sel);
        // 金属拉丝行:垂直渐变;选中行整体提亮
        lv_obj_set_style_bg_color(s_rows[i], shade(th->panel, sel ? +28 : +10), 0);
        lv_obj_set_style_bg_grad_color(s_rows[i], shade(th->panel, sel ? -6 : -16), 0);
    }
}

static void menu_enter(void) {
    const app_theme_t *th = app_theme_current();
    s_scr = app_ui_screen_create("菜单");

    // 金属拉丝底:垂直渐变(上暗下亮)
    lv_obj_set_style_bg_color(s_scr, shade(th->bg, -10), 0);
    lv_obj_set_style_bg_grad_color(s_scr, shade(th->bg, +20), 0);
    lv_obj_set_style_bg_grad_dir(s_scr, LV_GRAD_DIR_VER, 0);

    // 标题下渐变装饰线
    lv_obj_t *bar = lv_obj_create(s_scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 80, 2);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 33);
    lv_obj_set_style_bg_color(bar, shade(th->accent, -110), 0);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(th->accent), 0);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 1, 0);

    for (int i = 0; i < ITEM_COUNT; i++) {
        const int y = 40 + i * 31;
        s_rows[i] = app_ui_panel(s_scr, 180, 28);
        lv_obj_set_pos(s_rows[i], 30, y);
        lv_obj_set_style_radius(s_rows[i], 6, 0); // 小圆角:机械切削感
        lv_obj_set_style_bg_grad_dir(s_rows[i], LV_GRAD_DIR_VER, 0);
        s_lbl[i] = app_ui_label(s_rows[i], NAMES[i], app_font_cjk_14(), false);
        lv_obj_align(s_lbl[i], LV_ALIGN_LEFT_MID, 34, 0);
        add_icon(i, s_rows[i]);
        // 左侧选中指示条
        s_mark[i] = lv_obj_create(s_scr);
        lv_obj_remove_style_all(s_mark[i]);
        lv_obj_set_size(s_mark[i], 3, 18);
        lv_obj_set_pos(s_mark[i], 22, y + 5);
        lv_obj_set_style_bg_color(s_mark[i], lv_color_hex(th->accent), 0);
        lv_obj_set_style_bg_opa(s_mark[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_mark[i], 1, 0);
    }
    s_sel = 0;
    refresh();
    lv_screen_load(s_scr);
}

static void menu_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        for (int i = 0; i < ITEM_COUNT; i++) s_rows[i] = s_lbl[i] = s_mark[i] = NULL;
    }
}

static void menu_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        app_nav_open(GETTERS[s_sel]());
        return;
    }
    if (bsp_lvgl_lock(200)) {
        if (btn == BSP_BTN_UP) {
            s_sel = (s_sel + ITEM_COUNT - 1) % ITEM_COUNT;
        } else if (btn == BSP_BTN_DOWN) {
            s_sel = (s_sel + 1) % ITEM_COUNT;
        }
        refresh();
        bsp_lvgl_unlock();
    }
}

static const app_page_t PAGE = {
    .name = "菜单",
    .enter = menu_enter,
    .exit = menu_exit,
    .key = menu_key,
};

const app_page_t *page_menu(void) {
    return &PAGE;
}
