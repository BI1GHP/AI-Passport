// main/app_xiaozhi.h —— 小智AI 语音对话(xiaozhi-esp32 协议精简版)。
// 流程:OTA POST 激活/取 WebSocket 地址 → WS 连接 → hello → 手动模式对讲。
// 半双工:OK 开始说话,再按 OK 结束并发送,等待并播放回复。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    XZ_ST_IDLE = 0,   // 未启动
    XZ_ST_CONNECTING, // OTA/WS 连接中
    XZ_ST_ACTIVATING, // 待激活(显示激活码,后台自动重试)
    XZ_ST_READY,      // 就绪,可开始说话
    XZ_ST_LISTENING,  // 录音并实时上行
    XZ_ST_THINKING,   // 已发送,等待回复
    XZ_ST_SPEAKING,   // 播放回复音频
    XZ_ST_ERROR,      // 出错(可 OK 重连)
} xz_state_t;

esp_err_t app_xiaozhi_init(void);

// 进入页面:启动会话任务(幂等)。未连 Wi-Fi 返回 ESP_ERR_INVALID_STATE。
esp_err_t app_xiaozhi_open(void);
// 离开页面:断开并回收(阻塞等待任务退出,最长约 8 秒)。
void app_xiaozhi_close(void);

xz_state_t app_xiaozhi_state(void);
// 说话开关:READY→LISTENING 返回 ESP_OK;LISTENING→发送并进入 THINKING。
esp_err_t app_xiaozhi_talk_toggle(void);

// ACTIVATING 状态下的 6 位激活码;无则返回空串。
const char *app_xiaozhi_activation_code(void);
// 最近一条服务器文本(识别结果/回复),拷贝到 buf。
void app_xiaozhi_last_text(char *buf, size_t len);
