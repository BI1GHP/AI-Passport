// main/badge_app.h —— 电子工牌应用:页面统一接口与导航。
// 每个页面 = 实现 enter/exit/key(慢服务按需 start/stop),由 main.c 的导航栈管理。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

typedef struct {
    const char *name;                          // 页面名(日志用)
    void (*enter)(void);                       // 持 LVGL 锁创建并载入屏幕
    void (*exit)(void);                        // 持 LVGL 锁删除屏幕(stop 成功后调用)
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev); // 输入任务调用;操作 LVGL 时自行加锁
    esp_err_t (*start)(void);                  // 可选:页面载入后启动后台服务(不持锁)
    esp_err_t (*stop)(void);                   // 可选:删页面前停止后台服务(不持锁)
} app_page_t;

// 导航(main.c 实现)。OK 长按 = 返回上一级,由输入分发统一拦截。
void app_nav_menu(void);                     // 首页 -> 菜单
void app_nav_open(const app_page_t *page);   // 菜单 -> 子页
void app_nav_back(void);                     // 返回上一级

const app_page_t *page_home(void);           // page_home.c
const app_page_t *page_menu(void);           // page_menu.c
const app_page_t *page_about(void);          // page_about.c
const app_page_t *page_settings(void);       // page_settings.c
const app_page_t *page_wifi(void);           // page_wifi.c
const app_page_t *page_placeholder_named(const char *name); // page_placeholder.c

const app_page_t *page_card(void);           // page_card.c
const app_page_t *page_mail(void);           // page_mail.c
const app_page_t *page_xiaozhi(void);        // page_xiaozhi.c
const app_page_t *page_radio(void);          // page_radio.c
const app_page_t *page_recorder(void);       // page_recorder.c
