// main/app_weather.h —— 天气摘要(Open-Meteo Forecast API,后台任务刷新)。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t app_weather_init(void);            // 创建互斥锁
bool app_weather_busy(void);                 // 刷新任务运行中?
const char *app_weather_summary(void);       // "北京 晴 23°",未刷新时 "--"
void app_weather_request_refresh(void);      // 触发一次后台刷新(联网后调用)
void app_weather_cancel(void);               // 请求正在运行的刷新任务停止
