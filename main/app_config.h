// main/app_config.h —— NVS 持久化配置(命名空间 "badge")。
#pragma once

#include "esp_err.h"
#include <stdint.h>

#define APP_CFG_NAME_LEN   32
#define APP_CFG_COMP_LEN   48
#define APP_CFG_CALL_LEN   16
#define APP_CFG_CITY_LEN   32
#define APP_CFG_SSID_LEN   32
#define APP_CFG_PASS_LEN   64
#define APP_CFG_PROV_LEN   16
#define APP_CFG_USER_LEN   64
#define APP_CFG_URL_LEN   160

// 待机息屏模式:0=30 秒,1=1 分钟,2=5 分钟,3=不息屏。
#define APP_CFG_SCREEN_OFF_MODES 4

typedef struct {
    char name[APP_CFG_NAME_LEN];          // 姓名
    char nickname[APP_CFG_NAME_LEN];      // 昵称
    char company[APP_CFG_COMP_LEN];       // 公司
    char department[APP_CFG_NAME_LEN];    // 部门
    char callsign[APP_CFG_CALL_LEN];      // 呼号
    char city[APP_CFG_CITY_LEN];          // 天气城市(geocode 查询名,如 Beijing)
    double lat;                           // 城市经纬度缓存(geocode 成功后保存)
    double lon;
    uint8_t theme;                        // 0=金属 1=可爱 2=商务
    char wifi_ssid[APP_CFG_SSID_LEN];
    char wifi_pass[APP_CFG_PASS_LEN];
    char mail_provider[APP_CFG_PROV_LEN]; // qq / 163 / gmail / outlook
    char mail_user[APP_CFG_USER_LEN];
    char mail_pass[APP_CFG_PASS_LEN];
    uint8_t radio_region;                 // 电台地区索引(0=全球热门)
    uint8_t radio_style;                  // 电台界面风格:0=LED 1=液晶
    uint8_t volume;                       // 播放音量 0..100(双击上/下调节)
    uint8_t screen_off;                   // 待机息屏:0=30s 1=1min 2=5min 3=不息屏
    uint8_t boot_sound;                   // 开机提示音:0=关 1=开
    char xz_server[APP_CFG_URL_LEN];      // 小智AI服务器(OTA 地址,留空用官方)
} app_config_t;

// 电台风格显示名
const char *app_config_radio_style_name(void);

esp_err_t app_config_load(void);  // 读 NVS,缺失项填默认值;首次启动返回 ESP_OK
esp_err_t app_config_save(void);  // 整体写回 NVS,并递增 generation
app_config_t *app_config(void);   // 全局单例;仅任务上下文使用,勿在中断里访问
uint32_t app_config_generation(void);  // 每次 save 后 +1,UI 可轮询检测变更并刷新

uint32_t app_config_screen_off_seconds(void); // 待机秒数;0 表示不息屏
const char *app_config_screen_off_name(void); // 待机息屏选项显示名
