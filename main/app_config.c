// main/app_config.c
#include "app_config.h"

#include "esp_log.h"
#include "nvs.h"

#include <string.h>

static const char *TAG = "app_config";
#define NS "badge"

static app_config_t s_cfg;
static volatile uint32_t s_gen;

static void config_defaults(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    strcpy(s_cfg.name, "您的姓名");
    strcpy(s_cfg.nickname, "昵称");
    strcpy(s_cfg.company, "公司名称");
    strcpy(s_cfg.department, "部门");
    strcpy(s_cfg.callsign, "BI1GHP");
    strcpy(s_cfg.city, "Beijing");
    s_cfg.lat = 39.9042;
    s_cfg.lon = 116.4074;
    s_cfg.theme = 0;
    strcpy(s_cfg.mail_provider, "qq");
    s_cfg.radio_region = 1; // 默认中国台,全球榜在国内多不可达
    s_cfg.volume = 70;
    s_cfg.screen_off = 1; // 默认 1 分钟待机息屏
}

// 键存在且非空时覆盖默认值。
static void load_str(nvs_handle_t h, const char *key, char *out, size_t len) {
    size_t need = 0;
    if (nvs_get_str(h, key, NULL, &need) != ESP_OK || need <= 1 || need > len) return;
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = '\0';
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) *out = v;
}

static void load_double(nvs_handle_t h, const char *key, double *out) {
    double v;
    size_t n = sizeof(v);
    if (nvs_get_blob(h, key, &v, &n) == ESP_OK && n == sizeof(v)) *out = v;
}

esp_err_t app_config_load(void) {
    config_defaults();
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "NVS 命名空间不存在,使用默认配置");
        return ESP_OK; // 首次启动:默认值即可
    }
    load_str(h, "name", s_cfg.name, sizeof(s_cfg.name));
    load_str(h, "nick", s_cfg.nickname, sizeof(s_cfg.nickname));
    load_str(h, "comp", s_cfg.company, sizeof(s_cfg.company));
    load_str(h, "dept", s_cfg.department, sizeof(s_cfg.department));
    load_str(h, "call", s_cfg.callsign, sizeof(s_cfg.callsign));
    load_str(h, "city", s_cfg.city, sizeof(s_cfg.city));
    load_double(h, "lat", &s_cfg.lat);
    load_double(h, "lon", &s_cfg.lon);
    load_u8(h, "theme", &s_cfg.theme);
    load_str(h, "wssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    load_str(h, "wpass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    load_str(h, "mprov", s_cfg.mail_provider, sizeof(s_cfg.mail_provider));
    load_str(h, "muser", s_cfg.mail_user, sizeof(s_cfg.mail_user));
    load_str(h, "mpass", s_cfg.mail_pass, sizeof(s_cfg.mail_pass));
    load_u8(h, "rregion", &s_cfg.radio_region);
    load_u8(h, "vol", &s_cfg.volume);
    load_u8(h, "soff", &s_cfg.screen_off);
    load_str(h, "xzurl", s_cfg.xz_server, sizeof(s_cfg.xz_server));
    nvs_close(h);
    if (s_cfg.theme > 2) s_cfg.theme = 0;
    if (s_cfg.volume > 100) s_cfg.volume = 70;
    if (s_cfg.screen_off >= APP_CFG_SCREEN_OFF_MODES) s_cfg.screen_off = 1;
    return ESP_OK;
}

esp_err_t app_config_save(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "name", s_cfg.name);
    nvs_set_str(h, "nick", s_cfg.nickname);
    nvs_set_str(h, "comp", s_cfg.company);
    nvs_set_str(h, "dept", s_cfg.department);
    nvs_set_str(h, "call", s_cfg.callsign);
    nvs_set_str(h, "city", s_cfg.city);
    nvs_set_blob(h, "lat", &s_cfg.lat, sizeof(s_cfg.lat));
    nvs_set_blob(h, "lon", &s_cfg.lon, sizeof(s_cfg.lon));
    nvs_set_u8(h, "theme", s_cfg.theme);
    nvs_set_str(h, "wssid", s_cfg.wifi_ssid);
    nvs_set_str(h, "wpass", s_cfg.wifi_pass);
    nvs_set_str(h, "mprov", s_cfg.mail_provider);
    nvs_set_str(h, "muser", s_cfg.mail_user);
    nvs_set_str(h, "mpass", s_cfg.mail_pass);
    nvs_set_u8(h, "rregion", s_cfg.radio_region);
    nvs_set_u8(h, "vol", s_cfg.volume);
    nvs_set_u8(h, "soff", s_cfg.screen_off);
    nvs_set_str(h, "xzurl", s_cfg.xz_server);
    e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) s_gen++;
    ESP_LOGI(TAG, "配置已保存");
    return e;
}

app_config_t *app_config(void) {
    return &s_cfg;
}

uint32_t app_config_generation(void) {
    return s_gen;
}

uint32_t app_config_screen_off_seconds(void) {
    switch (s_cfg.screen_off) {
    case 0: return 30;
    case 1: return 60;
    case 2: return 300;
    default: return 0; // 不息屏
    }
}

const char *app_config_screen_off_name(void) {
    switch (s_cfg.screen_off) {
    case 0: return "30 秒";
    case 1: return "1 分钟";
    case 2: return "5 分钟";
    default: return "不息屏";
    }
}
