// main/app_radio.h —— 国内内置精选电台(MP3 直链)+ Helix 软件解码流式播放。
// 北京交通广播 / 音乐之声固定置顶;列表无需联网探测,进入页面即可用。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define APP_RADIO_MAX 20

typedef struct {
    char name[48];
    char url[160];
} app_radio_station_t;

esp_err_t app_radio_init(void);

esp_err_t app_radio_load(void); // 同步装载内置国内电台列表
bool app_radio_loading(void);   // 恒 false(内置列表无异步加载),保留以兼容页面轮询
esp_err_t app_radio_load_error(void);

int app_radio_count(void);
bool app_radio_station(int index, app_radio_station_t *out);

esp_err_t app_radio_play(int index); // 异步
void app_radio_stop(void);           // 停止并等待任务退出
bool app_radio_playing(void);
int app_radio_current(void);

#define APP_RADIO_SPEC_BANDS 8
void app_radio_spectrum(uint8_t *out); // 8 频段能量 0..100(停播时全 0)

// 开机提示音:播放 FAT 上的 boot.mp3(门户上传,≤200KB)。无文件时零开销。
void app_radio_boot_sound(void);
