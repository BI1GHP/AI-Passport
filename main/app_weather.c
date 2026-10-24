// main/app_weather.c —— 天气刷新:Open-Meteo Forecast API,后台任务,缓存摘要。
#include "app_weather.h"

#include "app_config.h"
#include "app_net.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_weather";
#define JSON_BUF_SIZE 8192
#define HTTP_TIMEOUT_MS 12000

// Open-Meteo weather_code -> 中文短语(覆盖 95% 常见场景)。
// https://open-meteo.com/en/docs
static const char *code_to_cn(int code) {
    if (code == 0)       return "晴";
    if (code == 1)       return "大部晴";
    if (code == 2)       return "多云";
    if (code == 3)       return "阴";
    if (code == 45)      return "雾";
    if (code == 48)      return "冻雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code >= 85 && code <= 86) return "阵雪";
    if (code == 95)      return "雷暴";
    if (code >= 96 && code <= 99) return "雷暴冰雹";
    return "--";
}

static SemaphoreHandle_t s_lock;
static volatile bool s_busy;
static volatile bool s_stop;

static char s_summary[64] = "--";    // 缓存摘要,app_weather_summary() 只读
static char s_city_display[32];      // 最近一次刷新时的城市名(供标签显式)

// 读 JSON 到 buf(最多 JSON_BUF_SIZE 字节)。
static int http_read_all(esp_http_client_handle_t client, char *buf, int cap) {
    int total = 0;
    while (total < cap - 1 && !s_stop) {
        int n = esp_http_client_read(client, buf + total, cap - 1 - total);
        if (n < 0) return -1;
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

static void weather_task(void *arg) {
    (void)arg;

    if (!app_net_sta_connected() || !app_net_try_acquire("weather")) {
        ESP_LOGW(TAG, "网络忙或未连接,跳过天气刷新");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    const app_config_t *c = app_config();
    if (c->lat == 0 && c->lon == 0) {
        ESP_LOGW(TAG, "未配置经纬度,无法查天气");
        app_net_release("weather");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.5f&longitude=%.5f"
             "&current=temperature_2m,weather_code&timezone=auto",
             c->lat, c->lon);

    char *body = malloc(JSON_BUF_SIZE);
    esp_http_client_handle_t client = NULL;

    if (!body) { goto release_net; }

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .user_agent = "BadgeFirmware/1.0",
        .buffer_size = 2048,
    };
    client = esp_http_client_init(&cfg);
    if (!client) goto release_net;

    if (esp_http_client_open(client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "Open-Meteo 连接失败");
        goto release_net;
    }

    int len = http_read_all(client, body, JSON_BUF_SIZE);
    if (len <= 0) {
        ESP_LOGE(TAG, "Open-Meteo 无响应");
        goto release_net;
    }

    if (s_stop) goto release_net;

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "JSON 解析失败(%d 字节)", len);
        goto release_net;
    }

    // current 是 nested object
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (!cur) {
        cJSON_Delete(root);
        goto release_net;
    }
    cJSON *temp_obj = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    cJSON *code_obj = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
    if (!cJSON_IsNumber(temp_obj) || !cJSON_IsNumber(code_obj)) {
        ESP_LOGW(TAG, "Open-Meteo 响应字段缺失");
        cJSON_Delete(root);
        goto release_net;
    }

    float temp = (float)temp_obj->valuedouble;
    int code = code_obj->valueint;
    const char *phrase = code_to_cn(code);

    // 拼摘要:"城市 短语 温度°" —— 城市名用配置的 city 字段(空则用默认 "北京")
    const char *city = c->city[0] ? c->city : "北京";
    snprintf(s_summary, sizeof(s_summary),
             "%.12s %.12s %.0f°", city, phrase, temp);
    snprintf(s_city_display, sizeof(s_city_display), "%.16s", city);
    ESP_LOGI(TAG, "天气刷新: %s", s_summary);

    cJSON_Delete(root);

release_net:
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(body);
    app_net_release("weather");
    s_busy = false;
    vTaskDelete(NULL);
}

esp_err_t app_weather_init(void) {
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

bool app_weather_busy(void) { return s_busy; }

const char *app_weather_summary(void) { return s_summary; }

void app_weather_request_refresh(void) {
    if (s_busy) return;
    s_stop = false;
    s_busy = true;
    if (xTaskCreate(weather_task, "badge_weather", 6144, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
    }
}

void app_weather_cancel(void) {
    s_stop = true;
}
