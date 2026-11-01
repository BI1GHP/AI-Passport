// main/app_net.c —— Wi-Fi 网络管理:STA 连接 + 网络活动互斥。
// 门户仅通过所连 Wi-Fi 的 STA IP 访问,不再提供 BadgeSetup 热点。
#include "app_net.h"

#include "app_config.h"
#include "app_portal.h"
#include "app_time.h"
#include "app_weather.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_net";

static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_ev;
static esp_event_handler_instance_t s_ip_ev;
static bool s_netif_ready;
static bool s_event_loop_ready;
static bool s_wifi_initialized;
static bool s_wifi_started;
static volatile bool s_sta_connected;

static SemaphoreHandle_t s_lock;
static const char *s_lock_owner;

// ---------------------------------------------------------------------------
// 已保存 Wi-Fi 记录:最多 5 个,按"最近连接成功"顺序排列(NVS 命名空间 wlst)。
// 启动/断线时按顺序尝试,哪个可用就自动连哪个。
// ---------------------------------------------------------------------------
typedef struct {
    char ssid[33];
    char pass[65];
} wifi_rec_t;

static wifi_rec_t s_wrec[APP_NET_WIFI_MAX];
static int s_wrec_n;
static int s_try_idx;     // 当前正在尝试的记录下标
static int s_fail_cnt;    // 当前记录连续失败次数
static nvs_handle_t s_wnvs;
static bool s_wnvs_ok;

static int wrec_find(const char *ssid) {
    if (!ssid || !ssid[0]) return -1;
    for (int i = 0; i < s_wrec_n; i++) {
        if (strcmp(s_wrec[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static void wrec_persist(void) {
    if (!s_wnvs_ok) return;
    nvs_set_u8(s_wnvs, "n", (uint8_t)s_wrec_n);
    char k[16];
    for (int i = 0; i < s_wrec_n; i++) {
        snprintf(k, sizeof(k), "s%d", i);
        nvs_set_str(s_wnvs, k, s_wrec[i].ssid);
        snprintf(k, sizeof(k), "p%d", i);
        nvs_set_str(s_wnvs, k, s_wrec[i].pass);
    }
    // 列表缩短时清掉多余旧槽,避免下次读回残留
    for (int i = s_wrec_n; i < APP_NET_WIFI_MAX; i++) {
        snprintf(k, sizeof(k), "s%d", i);
        nvs_erase_key(s_wnvs, k);
        snprintf(k, sizeof(k), "p%d", i);
        nvs_erase_key(s_wnvs, k);
    }
    nvs_commit(s_wnvs);
}

static void wrec_load(void) {
    s_wrec_n = 0;
    if (nvs_open("wlst", NVS_READWRITE, &s_wnvs) != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 记录存储不可用,仅本次运行有效");
        return;
    }
    s_wnvs_ok = true;
    uint8_t n = 0;
    if (nvs_get_u8(s_wnvs, "n", &n) != ESP_OK) return;
    if (n > APP_NET_WIFI_MAX) n = APP_NET_WIFI_MAX;
    char k[16];
    for (int i = 0; i < (int)n; i++) {
        char ssid[33] = { 0 };
        char pass[65] = { 0 };
        size_t len = sizeof(ssid);
        snprintf(k, sizeof(k), "s%d", i);
        if (nvs_get_str(s_wnvs, k, ssid, &len) != ESP_OK || !ssid[0]) continue;
        len = sizeof(pass);
        snprintf(k, sizeof(k), "p%d", i);
        nvs_get_str(s_wnvs, k, pass, &len); // 开放网络无密码也合法
        strlcpy(s_wrec[s_wrec_n].ssid, ssid, sizeof(s_wrec[s_wrec_n].ssid));
        strlcpy(s_wrec[s_wrec_n].pass, pass, sizeof(s_wrec[s_wrec_n].pass));
        s_wrec_n++;
    }
    ESP_LOGI(TAG, "已保存 Wi-Fi %d 个", s_wrec_n);
}

// 连接成功才调用:置顶该记录、去重、最多保留 5 个。
static void wrec_remember(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return;
    const int idx = wrec_find(ssid);
    if (idx == 0 && strcmp(s_wrec[0].pass, pass ? pass : "") == 0) return; // 无变化

    wifi_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    strlcpy(rec.ssid, ssid, sizeof(rec.ssid));
    strlcpy(rec.pass, pass ? pass : "", sizeof(rec.pass));

    if (idx >= 0) {                      // 已存在:移到队首
        for (int i = idx; i > 0; i--) s_wrec[i] = s_wrec[i - 1];
    } else {                             // 新网络:插入队首,满了丢最旧的
        const int top = (s_wrec_n < APP_NET_WIFI_MAX) ? s_wrec_n : APP_NET_WIFI_MAX - 1;
        for (int i = top; i > 0; i--) s_wrec[i] = s_wrec[i - 1];
        if (s_wrec_n < APP_NET_WIFI_MAX) s_wrec_n++;
    }
    s_wrec[0] = rec;
    s_try_idx = 0;
    s_fail_cnt = 0;
    wrec_persist();
    ESP_LOGI(TAG, "记录 Wi-Fi(共 %d 个): %s", s_wrec_n, rec.ssid);
}

// 仅下发配置,不触发连接(供 init 阶段使用)
static esp_err_t wrec_apply(int idx) {
    if (idx < 0 || idx >= s_wrec_n) return ESP_ERR_INVALID_ARG;
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, s_wrec[idx].ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s_wrec[idx].pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = s_wrec[idx].pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    s_try_idx = idx;
    return esp_wifi_set_config(WIFI_IF_STA, &sta);
}

// 切到第 idx 个记录并重连
static void wrec_connect(int idx) {
    if (idx < 0 || idx >= s_wrec_n) return;
    if (wrec_apply(idx) != ESP_OK) return;
    s_sta_connected = false;
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "尝试已保存 Wi-Fi %d/%d: %s", idx + 1, s_wrec_n, s_wrec[idx].ssid);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_STA_START) {
        if (s_wrec_n > 0) {
            wrec_apply(0);
            esp_wifi_connect();
            return;
        }
        app_config_t *c = app_config();
        if (c->wifi_ssid[0]) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        app_config_t *c = app_config();
        // 用户在设置页主动断开(清空了 SSID)时不再自动重连
        if (!c->wifi_ssid[0]) return;
        if (s_wrec_n > 0) {
            s_fail_cnt++;
            if (s_fail_cnt >= 2 && s_wrec_n > 1) {
                s_fail_cnt = 0;
                ESP_LOGW(TAG, "%s 连不上,换下一个已保存网络",
                         s_wrec[s_try_idx].ssid);
                wrec_connect((s_try_idx + 1) % s_wrec_n);
            } else {
                ESP_LOGW(TAG, "STA 断开,重新连接 %s", s_wrec[s_try_idx].ssid);
                esp_wifi_connect();
            }
            return;
        }
        ESP_LOGW(TAG, "STA 断开,重新连接");
        esp_wifi_connect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    s_sta_connected = true;
    s_fail_cnt = 0;
    ESP_LOGI(TAG, "STA 已获取 IP: " IPSTR, IP2STR(&ev->ip_info.ip));

    // 连接成功才记入"已保存 Wi-Fi"(最多 5 个),并同步到 app_config(UI/门户显示用)
    wifi_config_t cur;
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0]) {
        const char *ssid = (const char *)cur.sta.ssid;
        const char *pass = (const char *)cur.sta.password;
        wrec_remember(ssid, pass);
        app_config_t *c = app_config();
        if (strcmp(c->wifi_ssid, ssid) != 0 || strcmp(c->wifi_pass, pass) != 0) {
            strlcpy(c->wifi_ssid, ssid, sizeof(c->wifi_ssid));
            strlcpy(c->wifi_pass, pass, sizeof(c->wifi_pass));
            app_config_save();
        }
    }

    app_portal_start();  // STA 连网后自动启动门户,STA IP 可访问
    app_time_sntp_start();
    app_weather_request_refresh();
}

static esp_err_t net_prepare_once(void) {
    esp_err_t e;
    if (!s_netif_ready) {
        e = esp_netif_init();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        s_netif_ready = true;
    }
    if (!s_event_loop_ready) {
        e = esp_event_loop_create_default();
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
        s_event_loop_ready = true;
    }
    return ESP_OK;
}

esp_err_t app_net_init(void) {
    if (s_wifi_initialized) return ESP_OK;
    esp_err_t e = net_prepare_once();
    if (e != ESP_OK) return e;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    // 用分步创建替代 esp_netif_create_default_wifi_sta(),避免其内部断言重启。
    esp_netif_config_t sta_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&sta_cfg);
    if (!s_sta_netif) return ESP_ERR_NO_MEM;
    e = esp_netif_attach_wifi_station(s_sta_netif);
    if (e != ESP_OK) goto fail;
    e = esp_wifi_set_default_wifi_sta_handlers();
    if (e != ESP_OK) goto fail;

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&wcfg);
    if (e != ESP_OK) goto fail;
    s_wifi_initialized = true;

    e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_wifi_event, NULL, &s_wifi_ev);
    if (e != ESP_OK) goto fail;
    e = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_ip_event, NULL, &s_ip_ev);
    if (e != ESP_OK) goto fail;

    e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e != ESP_OK) goto fail;
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) goto fail;

    app_config_t *c = app_config();
    wrec_load();
    // 老固件只保存了单个 SSID:迁移成第一条记录
    if (s_wrec_n == 0 && c->wifi_ssid[0]) {
        wrec_remember(c->wifi_ssid, c->wifi_pass);
    }
    if (s_wrec_n > 0) {
        esp_err_t ce = wrec_apply(0);
        if (ce != ESP_OK) goto fail;
        ESP_LOGI(TAG, "连接 Wi-Fi: %s(已保存 %d 个)", s_wrec[0].ssid, s_wrec_n);
    } else {
        ESP_LOGI(TAG, "未配置 Wi-Fi;请到 设置 > Wi-Fi 页面选择网络并输入密码");
    }
    e = esp_wifi_start();
    if (e != ESP_OK) goto fail;

    // 默认开 Modem-sleep 省电;电台/小智等低延迟场景会临时切 WIFI_PS_NONE 并恢复
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // 初始化 SNTP 客户端(必须在 esp_netif_init + event_loop_create_default 之后)
    app_time_sntp_init();
    s_wifi_started = true;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi 初始化失败: %s", esp_err_to_name(e));
    return e;
}

bool app_net_sta_connected(void) {
    return s_sta_connected;
}

void app_net_sta_ip(char *buf, size_t bufsize) {
    if (!buf || bufsize < 4) return;
    buf[0] = '\0';
    if (!s_sta_connected || !s_sta_netif) return;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK) return;
    esp_ip4addr_ntoa(&info.ip, buf, bufsize);
}

int8_t app_net_sta_rssi(void) {
    if (!s_sta_connected) return 0;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return (int8_t)ap.rssi;
}

esp_err_t app_net_retry_sta(void) {
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    app_config_t *c = app_config();
    if (!c->wifi_ssid[0]) return ESP_OK;
    const bool was_connected = app_net_sta_connected();

    // 从已保存列表里找:页面只传 SSID(未填密码)时用记录里存的密码
    const int sidx = wrec_find(c->wifi_ssid);
    const char *pass = c->wifi_pass;
    if (sidx >= 0 && !c->wifi_pass[0]) pass = s_wrec[sidx].pass;

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, c->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (e != ESP_OK) return e;

    if (sidx >= 0) { // 针对已保存记录:轮到它作为当前尝试项
        s_try_idx = sidx;
        s_fail_cnt = 0;
    }
    s_sta_connected = false;
    if (was_connected) esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "STA 重连 %s", c->wifi_ssid);
    return ESP_OK;
}

bool app_net_wifi_is_saved(const char *ssid) {
    return wrec_find(ssid) >= 0;
}

bool app_net_try_acquire(const char *owner) {
    if (!s_lock) return false;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return false;
    s_lock_owner = owner;
    return true;
}

void app_net_release(const char *owner) {
    if (!s_lock) return;
    if (s_lock_owner && owner && strcmp(s_lock_owner, owner) != 0) {
        ESP_LOGW(TAG, "网络锁 owner 不匹配: %s -> %s", s_lock_owner, owner);
    }
    s_lock_owner = NULL;
    xSemaphoreGive(s_lock);
}

const char *app_net_lock_owner(void) {
    return s_lock_owner;
}

// ========== Wi-Fi 扫描 ==========
static volatile bool s_scan_busy;
static app_net_ap_t s_scan_cache[APP_NET_SCAN_MAX];
static int s_scan_count;

static void scan_task(void *arg) {
    (void)arg;
    s_scan_busy = true;
    s_scan_count = 0;
    memset(s_scan_cache, 0, sizeof(s_scan_cache));

    // 扫描期间不持 app_net 锁 —— esp_wifi_scan_start 内部会短暂切换模式,
    // 但它不应和天气/电台等联网任务冲突(scan 不占用 TX/RX 通道)。
    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
    };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败: %s", esp_err_to_name(e));
        s_scan_busy = false;
        vTaskDelete(NULL);
        return;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * (found > APP_NET_SCAN_MAX ? APP_NET_SCAN_MAX : found));
    if (!recs) { s_scan_busy = false; vTaskDelete(NULL); return; }
    uint16_t got = (found > APP_NET_SCAN_MAX) ? APP_NET_SCAN_MAX : found;
    esp_wifi_scan_get_ap_records(&got, recs);

    // 按 RSSI 排序(降序,信号强在前)
    for (int i = 0; i < got - 1; i++) {
        for (int j = i + 1; j < got; j++) {
            if (recs[j].rssi > recs[i].rssi) {
                wifi_ap_record_t t = recs[i]; recs[i] = recs[j]; recs[j] = t;
            }
        }
    }

    app_config_t *cfg = app_config();
    const char *cur_ssid = NULL;
    wifi_config_t cur;
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0]) {
        cur_ssid = (const char *)cur.sta.ssid;
    }

    for (int i = 0; i < got && i < APP_NET_SCAN_MAX; i++) {
        s_scan_cache[i].rssi = (int8_t)recs[i].rssi;
        s_scan_cache[i].auth = recs[i].authmode;
        memcpy(s_scan_cache[i].ssid, recs[i].ssid, 32);
        s_scan_cache[i].ssid[32] = '\0';
        if (cur_ssid && strcmp(s_scan_cache[i].ssid, cur_ssid) == 0) {
            s_scan_cache[i].connected = true;
        }
        // 已保存过密码(列表最多 5 个)的网络标记 saved,选中后免输密码
        if (app_net_wifi_is_saved(s_scan_cache[i].ssid) ||
            (cfg->wifi_ssid[0] && strcmp(s_scan_cache[i].ssid, cfg->wifi_ssid) == 0)) {
            s_scan_cache[i].saved = true;
        }
        s_scan_count++;
    }
    free(recs);
    ESP_LOGI(TAG, "扫描完成 %d/%d", s_scan_count, found);
    s_scan_busy = false;
    vTaskDelete(NULL);
}

esp_err_t app_net_scan_start(void) {
    if (!s_wifi_started) return ESP_ERR_INVALID_STATE;
    if (s_scan_busy) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(scan_task, "badge_scan", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_net_scan_busy(void) { return s_scan_busy; }
int app_net_scan_count(void)  { return s_scan_count; }

const app_net_ap_t *app_net_scan_get(int index) {
    if (index < 0 || index >= s_scan_count) return NULL;
    return &s_scan_cache[index];
}
