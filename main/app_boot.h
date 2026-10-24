// main/app_boot.h —— 开机画面:自定义 logo + 启动进度条。
// 用法:bsp_lvgl_init() 后 app_boot_begin();各初始化阶段调 app_boot_progress();
// app_store_init() 后调 app_boot_logo() 尝试加载 S:/logo.jpg;进首页后 app_boot_end()。
#pragma once

void app_boot_begin(void);
void app_boot_progress(int pct, const char *msg);
void app_boot_logo(void);
void app_boot_end(void);
