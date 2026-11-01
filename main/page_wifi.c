// main/page_wifi.c —— 独立 Wi-Fi 设置页:扫描 → 选中 → 虚拟键盘输密码 → 连接。
// 键盘:4 列网格,3 页(abc/ABC/1#),光标线性移动,OK 选键,长按 OK 完成。
#include "badge_app.h"

#include "app_config.h"
#include "app_fonts.h"
#include "app_net.h"
#include "app_theme.h"
#include "app_ui.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include <stdio.h>
#include <string.h>

#define LIST_ROWS  5
#define TOP_Y      34
#define ROW_H      30

// ---- 虚拟键盘:4 列 × 8 行,4 页独立(abc / ABC / 123 / 符号) ----
#define KB_COLS 4
#define KB_ROWS 8

// 第 0 页:小写字母 a-z 按序 + 4 个常用符号 + DEL + OK
static const char *KB_LO[KB_ROWS][KB_COLS] = {
    {"a","b","c","d"},
    {"e","f","g","h"},
    {"i","j","k","l"},
    {"m","n","o","p"},
    {"q","r","s","t"},
    {"u","v","w","x"},
    {"y","z","@","#"},
    {"$","%","DEL","OK"},
};
// 第 1 页:大写字母 A-Z 按序 + 4 个常用符号 + DEL + OK
static const char *KB_HI[KB_ROWS][KB_COLS] = {
    {"A","B","C","D"},
    {"E","F","G","H"},
    {"I","J","K","L"},
    {"M","N","O","P"},
    {"Q","R","S","T"},
    {"U","V","W","X"},
    {"Y","Z","_","-"},
    {"!","*","DEL","OK"},
};
// 第 2 页:完整数字 0-9 + 常用符号 + DEL + OK
static const char *KB_NUM[KB_ROWS][KB_COLS] = {
    {"0","1","2","3"},
    {"4","5","6","7"},
    {"8","9",".","@"},
    {"-","_","(",")"},
    {"[","]","{","}"},
    {"|","/","?","\\"},
    {"~","`","'","\""},
    {",",";","DEL","OK"},
};
// 第 3 页:更多符号 + DEL + OK(完整符号页)
static const char *KB_SYM[KB_ROWS][KB_COLS] = {
    {":","+","=","<"},
    {">","&","^","%"},
    {"#","$","!","*"},
    {"@","-","_","."},
    {"/","\\","|","?"},
    {"~","`","'","\""},
    {"(",")","[","]"},
    {"{","}","DEL","OK"},
};

#define KB_PAGES 4
enum { KB_PAGE_LO = 0, KB_PAGE_HI, KB_PAGE_NUM, KB_PAGE_SYM };
static const char *KB_PAGE_LABEL[KB_PAGES] = {"abc", "ABC", "123", "符号"};

#define PW_MAX 64
enum { VIEW_SCAN, VIEW_LIST, VIEW_PASSWORD, VIEW_CONNECT };

static lv_obj_t *s_scr;
static int s_view;
static lv_obj_t *s_rows[LIST_ROWS];
static lv_obj_t *s_lbl[LIST_ROWS];
static lv_obj_t *s_title;
static lv_obj_t *s_hint;
static lv_timer_t *s_tick;
static int s_sel;
static int s_top;

// 密码输入状态
static lv_obj_t *s_kb_grid;
static lv_obj_t *s_kb_cells[KB_ROWS][KB_COLS]; // 单组 cells(跨 3 页复用)
static lv_obj_t *s_kb_lbls[KB_ROWS][KB_COLS];  // 对应 labels
// 密码输入框(带 border 的 panel,里面放 label 显示星号)
static lv_obj_t *s_pw_panel;
static lv_obj_t *s_pw_lbl;
static int s_kb_page;
static int s_kb_r, s_kb_c;
static char s_pw[PW_MAX + 1];
static int  s_pw_len;
static char s_target_ssid[33];
static uint8_t s_target_auth;

static void wifi_refresh(void);
static void switch_view(int v);

// ---- 字体可显示字符范围(safe_ssid 白名单) ----
static bool cp_is_displayable(uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) return true;
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
    switch (cp) {
    case 0x00B0: case 0x00B7:
    case 0x2014: case 0x201C: case 0x201D:
    case 0x2026: case 0x2192:
    case 0x25C0: case 0x25CB: case 0x25CF:
    case 0x2713:
    case 0x3001: case 0x3002: case 0x3010: case 0x3011:
    case 0xFF0C: case 0xFF1A: case 0xFF1B:
        return true;
    }
    return false;
}
static void safe_ssid(const char *src, char *dst, size_t cap) {
    size_t j = 0;
    const uint8_t *p = (const uint8_t *)src;
    while (*p && j + 2 < cap) {
        uint8_t b = *p; uint32_t cp = 0; int n = 1;
        if (b < 0x80) { cp = b; n = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = (b & 0x1F) << 6 | (p[1] & 0x3F); n = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = (b & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); n = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = (b & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F); n = 4; }
        else { dst[j++] = '?'; p++; continue; }
        if (cp_is_displayable(cp)) {
            for (int i = 0; i < n && j + 1 < cap; i++) dst[j++] = (char)p[i];
        } else { dst[j++] = '?'; }
        p += n;
    }
    dst[j] = '\0';
}
static void rssi_str(int8_t rssi, char *out, size_t cap) {
    if (rssi == 0) { snprintf(out, cap, "??"); return; }
    if (rssi >= -55) snprintf(out, cap, "|||");
    else if (rssi >= -65) snprintf(out, cap, "||");
    else if (rssi >= -75) snprintf(out, cap, "|");
    else snprintf(out, cap, ".");
}

// ---- 扫描轮询 ----
static void wifi_tick(lv_timer_t *t) {
    (void)t;
    if (!s_scr) return;
    if (bsp_lvgl_lock(100)) { wifi_refresh(); bsp_lvgl_unlock(); }
}
static void start_scan(void) { app_net_scan_start(); switch_view(VIEW_SCAN); }

static void switch_view(int v) {
    s_view = v;
    // 先隐藏所有视图元素
    for (int i = 0; i < LIST_ROWS; i++) lv_obj_set_hidden(s_rows[i], true);
    if (s_kb_grid)   lv_obj_set_hidden(s_kb_grid, true);
    if (s_pw_panel)  lv_obj_set_hidden(s_pw_panel, true);

    if (v == VIEW_SCAN) {
        lv_label_set_text(s_title, "正在扫描 Wi-Fi...");
        lv_label_set_text(s_hint, "请稍候");
    } else if (v == VIEW_CONNECT) {
        lv_label_set_text(s_title, "连接中...");
        lv_label_set_text(s_hint, "稍等片刻");
    } else if (v == VIEW_PASSWORD) {
        lv_obj_set_hidden(s_pw_panel, false);
        lv_obj_set_hidden(s_kb_grid, false);
    }
    s_sel = 0; s_top = 0;
}

// ---- 渲染 ----
static void render_list(void) {
    int count = app_net_scan_count();
    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = s_top + i;
        if (idx >= count) { lv_obj_set_hidden(s_rows[i], true); continue; }
        lv_obj_set_hidden(s_rows[i], false);
        const app_net_ap_t *ap = app_net_scan_get(idx);
        char sig[16]; rssi_str(ap->rssi, sig, sizeof(sig));
        char ssid_safe[48]; safe_ssid(ap->ssid, ssid_safe, sizeof(ssid_safe));
        char buf[96];
        const char *lock = (ap->auth != WIFI_AUTH_OPEN) ? "[锁]" : "   ";
        const char *saved = ap->saved ? "*" : (ap->auth != WIFI_AUTH_OPEN ? "?" : " ");
        const char *cur = ap->connected ? " <=" : "";
        snprintf(buf, sizeof(buf), "%s %s%s%s  %s", ssid_safe, lock, saved, cur, sig);
        lv_label_set_text(s_lbl[i], buf);
        app_ui_row_style(s_rows[i], s_lbl[i], idx == s_sel);
    }
}

// 取当前页的键值表(4 页)
static const char *(*kb_page_keys(void))[KB_ROWS][KB_COLS] {
    static const char *(*pages[KB_PAGES])[KB_ROWS][KB_COLS] = {&KB_LO, &KB_HI, &KB_NUM, &KB_SYM};
    return pages[s_kb_page];
}

// 键盘光标高亮 + 刷新所有 cell 文本 + 显示明文密码
static void render_kb(void) {
    // 明文显示(不用星号,方便检查)
    if (s_pw_len == 0) {
        lv_label_set_text(s_pw_lbl, "(空)");
    } else {
        lv_label_set_text(s_pw_lbl, s_pw);
    }

    const char *(*keys)[KB_ROWS][KB_COLS] = kb_page_keys();
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            lv_label_set_text(s_kb_lbls[r][c], (*keys)[r][c]);
            if (r == s_kb_r && c == s_kb_c) {
                // 选中:白 border 2px, 亮底色
                lv_obj_set_style_border_width(s_kb_cells[r][c], 2, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_color(s_kb_cells[r][c], lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_color(s_kb_cells[r][c], lv_color_hex(0x4A4A4A), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_opa(s_kb_cells[r][c], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            } else {
                // 未选中:border 1px, 正常底色 —— 显式覆盖所有 style 防遗留
                lv_obj_set_style_border_width(s_kb_cells[r][c], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_border_color(s_kb_cells[r][c], lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_color(s_kb_cells[r][c], lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
                lv_obj_set_style_bg_opa(s_kb_cells[r][c], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }
    }
}

static void wifi_refresh(void) {
    if (s_view == VIEW_SCAN) {
        if (!app_net_scan_busy()) {
            int count = app_net_scan_count();
            if (count == 0) {
                lv_label_set_text(s_title, "未找到 Wi-Fi");
                lv_label_set_text(s_hint, "UP 重新扫描 / OK 返回");
                switch_view(VIEW_LIST);
            } else {
                switch_view(VIEW_LIST);
                lv_label_set_text_fmt(s_title, "附近 %d 个网络", count);
                lv_label_set_text(s_hint, "OK 选中 · UP/DOWN 移动");
            }
            render_list();
        } else {
            lv_label_set_text(s_title, "正在扫描 Wi-Fi...");
        }
    } else if (s_view == VIEW_LIST) {
        render_list();
    } else if (s_view == VIEW_PASSWORD) {
        render_kb();
        lv_label_set_text(s_hint, "UP/DOWN 移动 · OK 选 · 长按OK完成 · 长按UP切页");
    } else if (s_view == VIEW_CONNECT) {
        app_config_t *c = app_config();
        if (!c->wifi_ssid[0]) {
            lv_label_set_text(s_title, "未选中网络");
            lv_label_set_text(s_hint, "UP 返回列表");
        } else if (app_net_sta_connected()) {
            char ip[20]; app_net_sta_ip(ip, sizeof(ip));
            int8_t rssi = app_net_sta_rssi();
            char sig[16]; rssi_str(rssi, sig, sizeof(sig));
            lv_label_set_text_fmt(s_title, "OK %s %s", ip, sig);
            lv_label_set_text_fmt(s_hint, "门户 http://%s/", ip);
        } else {
            lv_label_set_text_fmt(s_title, "连接中 %s...", c->wifi_ssid);
            lv_label_set_text(s_hint, "稍等 · DOWN 取消");
        }
    }
}

static void enter_password_view(const char *ssid, uint8_t auth) {
    strlcpy(s_target_ssid, ssid, sizeof(s_target_ssid));
    s_target_auth = auth;
    s_pw_len = 0; s_pw[0] = '\0';
    s_kb_page = KB_PAGE_LO;
    s_kb_r = KB_ROWS - 1; // 光标在 DEL/OK 那行更方便
    s_kb_c = 2;           // 初始在 DEL
    char ssid_safe[48]; safe_ssid(ssid, ssid_safe, sizeof(ssid_safe));
    lv_label_set_text_fmt(s_title, "输入密码 → %s [%s]", ssid_safe, KB_PAGE_LABEL[s_kb_page]);
    switch_view(VIEW_PASSWORD);
}

static void select_ap(int idx) {
    const app_net_ap_t *ap = app_net_scan_get(idx);
    if (!ap) return;
    if (ap->connected) {
        app_config_t *c = app_config();
        c->wifi_ssid[0] = '\0';
        app_config_save();
        esp_wifi_disconnect();
        return;
    }
    app_config_t *c = app_config();
    if (ap->auth == WIFI_AUTH_OPEN) {
        strlcpy(c->wifi_ssid, ap->ssid, sizeof(c->wifi_ssid));
        c->wifi_pass[0] = '\0';
        app_config_save();
        app_net_retry_sta();
        switch_view(VIEW_CONNECT);
        return;
    }
    if (ap->saved) {
        strlcpy(c->wifi_ssid, ap->ssid, sizeof(c->wifi_ssid));
        c->wifi_pass[0] = '\0';   // 清空以触发 app_net_retry_sta() 使用已保存记录里的密码
        app_config_save();
        app_net_retry_sta();
        switch_view(VIEW_CONNECT);
        return;
    }
    enter_password_view(ap->ssid, ap->auth);
}

// ---- 虚拟键盘:取当前光标位置的键字符串 ----
static const char *kb_cell_str(void) {
    const char *(*keys)[KB_ROWS][KB_COLS] = kb_page_keys();
    return (*keys)[s_kb_r][s_kb_c];
}

static void kb_activate(void) {
    const char *s = kb_cell_str();
    if (strcmp(s, "DEL") == 0) {
        if (s_pw_len > 0) { s_pw_len--; s_pw[s_pw_len] = '\0'; }
    } else if (strcmp(s, "OK") == 0) {
        // 长按 OK 也能完成,这里 OK 键也做一次确认(等效)
        app_config_t *c = app_config();
        strlcpy(c->wifi_ssid, s_target_ssid, sizeof(c->wifi_ssid));
        strlcpy(c->wifi_pass, s_pw, sizeof(c->wifi_pass));
        app_config_save();
        app_net_retry_sta();
        switch_view(VIEW_CONNECT);
    } else {
        if (s_pw_len < PW_MAX) {
            // 键可能是多字符(比如符号组),但这里每个键都是单字符或短标签
            // 单字符追加:
            if (strlen(s) == 1) {
                s_pw[s_pw_len++] = s[0]; s_pw[s_pw_len] = '\0';
            }
        }
    }
}

// ---- 按键 ----
static void wifi_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (bsp_lvgl_lock(200)) {
        if (s_view == VIEW_PASSWORD) {
            if (ev == BSP_BTN_LONG) {
                if (btn == BSP_BTN_OK) {
                    // 长按 OK = 确认密码
                    app_config_t *c = app_config();
                    strlcpy(c->wifi_ssid, s_target_ssid, sizeof(c->wifi_ssid));
                    strlcpy(c->wifi_pass, s_pw, sizeof(c->wifi_pass));
                    app_config_save();
                    app_net_retry_sta();
                    switch_view(VIEW_CONNECT);
                } else if (btn == BSP_BTN_UP) {
                    // 长按 UP = 切换键盘页(abc→ABC→1#→abc)
                    s_kb_page = (s_kb_page + 1) % KB_PAGES;
                    char ssid_safe[48]; safe_ssid(s_target_ssid, ssid_safe, sizeof(ssid_safe));
                    lv_label_set_text_fmt(s_title, "输入密码 → %s [%s]", ssid_safe, KB_PAGE_LABEL[s_kb_page]);
                } else if (btn == BSP_BTN_DOWN) {
                    // 长按 DOWN = 取消密码输入
                    switch_view(VIEW_LIST);
                    lv_label_set_text_fmt(s_title, "附近 %d 个网络", app_net_scan_count());
                }
            } else if (ev == BSP_BTN_CLICK) {
                if (btn == BSP_BTN_UP) {
                    // 横向蛇形:左移,到行首跳到上一行末尾
                    s_kb_c--;
                    if (s_kb_c < 0) { s_kb_c = KB_COLS - 1; s_kb_r--; if (s_kb_r < 0) s_kb_r = KB_ROWS - 1; }
                } else if (btn == BSP_BTN_DOWN) {
                    // 横向蛇形:右移,到行尾跳到下一行开头
                    s_kb_c++;
                    if (s_kb_c >= KB_COLS) { s_kb_c = 0; s_kb_r++; if (s_kb_r >= KB_ROWS) s_kb_r = 0; }
                } else if (btn == BSP_BTN_OK) {
                    kb_activate();
                }
            }
        } else if (ev == BSP_BTN_CLICK) {
            if (s_view == VIEW_SCAN) {
                if (btn == BSP_BTN_UP) start_scan();
            } else if (s_view == VIEW_LIST) {
                int count = app_net_scan_count();
                if (count == 0) {
                    if (btn == BSP_BTN_UP) start_scan();
                    else if (btn == BSP_BTN_OK) app_nav_back();
                } else if (btn == BSP_BTN_UP) {
                    if (s_sel > 0) s_sel--;
                    if (s_sel < s_top) s_top = s_sel;
                } else if (btn == BSP_BTN_DOWN) {
                    if (s_sel < count - 1) s_sel++;
                    if (s_sel >= s_top + LIST_ROWS) s_top = s_sel - LIST_ROWS + 1;
                } else if (btn == BSP_BTN_OK) {
                    select_ap(s_sel);
                }
            } else if (s_view == VIEW_CONNECT) {
                if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                    switch_view(VIEW_LIST);
                }
            }
        }
        wifi_refresh();
        bsp_lvgl_unlock();
    }
}

// ---- 创建虚拟键盘 grid(单组 cells,跨页复用,padding=0 防裁剪) ----
static void create_kb_grid(lv_obj_t *parent) {
    s_kb_grid = lv_obj_create(parent);
    lv_obj_set_scrollable(s_kb_grid, false);
    lv_obj_set_style_pad_all(s_kb_grid, 0, 0);       // 关键!清默认 padding=12
    lv_obj_set_style_border_width(s_kb_grid, 0, 0);
    lv_obj_set_style_bg_opa(s_kb_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_size(s_kb_grid, 236, KB_ROWS * 25);   // cell_h=25, 更紧凑
    lv_obj_set_pos(s_kb_grid, 2, 52);                // 密码框 y=30+24=54, 上移 2px
    lv_obj_set_hidden(s_kb_grid, true);

    int cell_w = 58;
    int cell_h = 25;
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(s_kb_grid);
            lv_obj_set_style_pad_all(cell, 0, 0);       // cell 也要 0 padding
            lv_obj_set_size(cell, cell_w - 2, cell_h - 2);
            lv_obj_set_pos(cell, c * cell_w + 1, r * cell_h + 1);
            lv_obj_set_style_radius(cell, 3, 0);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0x333333), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(0x666666), 0);
            lv_obj_t *lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "?");
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
            lv_obj_center(lbl);
            s_kb_cells[r][c] = cell;
            s_kb_lbls[r][c] = lbl;
        }
    }
}

// ---- 生命周期 ----
static void wifi_enter(void) {
    s_scr = app_ui_screen_create("Wi-Fi");

    // 标题(顶部偏上,给密码框留空间)
    s_title = app_ui_label(s_scr, "", app_font_cjk_14(), false);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 4);

    // 扫描列表行(只在 VIEW_LIST/VIEW_SCAN 显示)
    for (int i = 0; i < LIST_ROWS; i++) {
        s_rows[i] = app_ui_panel(s_scr, 200, ROW_H - 4);
        lv_obj_set_pos(s_rows[i], 20, TOP_Y + i * ROW_H);
        s_lbl[i] = app_ui_label(s_rows[i], "", app_font_cjk_14(), false);
        lv_obj_center(s_lbl[i]);
    }

    // 密码输入框:带 border 的 panel,padding=0 防裁剪
    s_pw_panel = lv_obj_create(s_scr);
    lv_obj_set_style_pad_all(s_pw_panel, 0, 0);     // 关键!
    lv_obj_set_scrollable(s_pw_panel, false);
    lv_obj_set_size(s_pw_panel, 236, 24);
    lv_obj_set_pos(s_pw_panel, 2, 30);               // 标题 font16 占 y=10~28,给它留空间
    lv_obj_set_style_radius(s_pw_panel, 3, 0);
    lv_obj_set_style_bg_color(s_pw_panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_pw_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pw_panel, 1, 0);
    lv_obj_set_style_border_color(s_pw_panel, lv_color_hex(0x555555), 0);
    lv_obj_set_hidden(s_pw_panel, true);
    s_pw_lbl = lv_label_create(s_pw_panel);
    lv_label_set_text(s_pw_lbl, "(空)");
    lv_obj_set_style_text_color(s_pw_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_pw_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_pw_lbl, LV_ALIGN_CENTER, 0, 0);

    // 底部提示
    s_hint = app_ui_hint(s_scr);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    create_kb_grid(s_scr);

    s_pw[0] = '\0'; s_pw_len = 0;
    s_kb_page = KB_PAGE_LO; s_kb_r = KB_ROWS - 1; s_kb_c = 3; // 初始在 OK 键

    s_tick = lv_timer_create(wifi_tick, 500, NULL);
    start_scan();
    lv_screen_load(s_scr);
}

static void wifi_exit(void) {
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_scr)  { lv_obj_delete(s_scr); s_scr = NULL; }
}

static const app_page_t PAGE = {
    .name = "Wi-Fi",
    .enter = wifi_enter,
    .exit = wifi_exit,
    .key = wifi_key,
};

const app_page_t *page_wifi(void) { return &PAGE; }
