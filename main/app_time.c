// main/app_time.c —— 时间:硬件计时器基准(UTC+8)+ SNTP 联网校时。
#include "app_time.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>

static const char *TAG = "app_time";

// 软件钟基准:2026-01-01 00:00:00 UTC(占位,SNTP 同步后替换)
static time_t s_base_epoch = 1767225600;
static int64_t s_base_us = -1;
static bool s_synced;
static bool s_sntp_started;

static void on_sntp_sync(struct timeval *tv) {
    (void)tv;
    time_t now = time(NULL);
    s_base_epoch = now;
    s_base_us = esp_timer_get_time();
    s_synced = true;
    struct tm t;
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "SNTP 同步成功: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

void app_time_sntp_init(void) {
    // 时区 UTC+8,只设一次
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "时区设为 UTC+8");
}

void app_time_sntp_start(void) {
    if (s_sntp_started) return;   // 防止重复 init
    s_sntp_started = true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP 已启动,等待同步...");
}

void app_time_now(struct tm *out) {
    if (s_base_us < 0) s_base_us = esp_timer_get_time();
    time_t t = s_base_epoch + (time_t)((esp_timer_get_time() - s_base_us) / 1000000);
    localtime_r(&t, out);
}

bool app_time_is_synced(void) {
    return s_synced;
}
