// main/app_portal.h —— HTTP 配置门户(80 端口),仅通过 STA IP 访问。
// 提供:配置表单/保存、相片与二维码图片上传、录音文件列表与下载。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t app_portal_start(void);  // 重复调用返回 ESP_OK
esp_err_t app_portal_stop(void);
bool app_portal_running(void);
