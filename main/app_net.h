// main/app_net.h —— Wi-Fi 网络管理:STA 连接 + 网络活动互斥。
// 门户仅通过所连 Wi-Fi 的 STA IP 访问,不再提供 BadgeSetup 热点。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 启动时调用一次:初始化 netif/事件循环/Wi-Fi 驱动;
// 若已配置 Wi-Fi 则后台自动连接 STA(断线自动重连)。
esp_err_t app_net_init(void);

bool app_net_sta_connected(void);   // STA 已拿到 IP

// 取当前 STA 的 IPv4 地址,写入 buf(至少 16 字节)。未连返回空串。
void app_net_sta_ip(char *buf, size_t bufsize);

// 取当前 STA 连接的 RSSI(dBm)。未连返回 0。
int8_t app_net_sta_rssi(void);

// 门户保存新 Wi-Fi 凭据后调用:应用新配置并触发连接。
esp_err_t app_net_retry_sta(void);

// ========== 已保存 Wi-Fi(最多 5 个,按连接成功先后保留) ==========
#define APP_NET_WIFI_MAX 5
bool app_net_wifi_is_saved(const char *ssid);  // 该 SSID 是否在已保存列表里

// ========== Wi-Fi 扫描 ==========
#define APP_NET_SCAN_MAX 10
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth;    // WIFI_AUTH_OPEN=0 / WPA2=...
    bool connected;  // 是否为当前已连 AP
    bool saved;      // 是否为门户已存过密码的 SSID
} app_net_ap_t;

esp_err_t app_net_scan_start(void);                   // 触发后台扫描;非阻塞
bool app_net_scan_busy(void);                         // 扫描进行中?
int app_net_scan_count(void);                         // 缓存结果数(scan 完成后有效)
const app_net_ap_t *app_net_scan_get(int index);      // 0..count-1

// Wi-Fi 省电策略。默认关闭省电(响应快);息屏待机时再开 modem-sleep 省电。
// 省电模式下 AP 按 DTIM 批量投递,DNS/TLS 会出现数百毫秒到数秒的停顿。
void app_net_set_power_save(bool low_power);

// 网络活动互斥:天气/邮箱/电台等主动联网任务同时只允许一个。
// 非阻塞;获取成功后在完成任务后必须 release。owner 仅用于日志。
bool app_net_try_acquire(const char *owner);
void app_net_release(const char *owner);
const char *app_net_lock_owner(void);
