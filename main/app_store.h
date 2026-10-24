// main/app_store.h —— FAT 存储(磨损均衡,storage 分区,挂载点 /badge)。
// 存放相片、二维码图片、录音与提醒录音文件。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define APP_STORE_BASE "/badge"

esp_err_t app_store_init(void); // 挂载失败返回错误,相关功能降级(页面自行提示)
bool app_store_ready(void);

// 拼路径:app_store_path(buf, n, "rec/rec001.wav") -> "/badge/rec/rec001.wav"
void app_store_path(char *buf, size_t size, const char *rel);
