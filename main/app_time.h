// main/app_time.h —— 本地时间(TZ=CST-8)。
// M1 为上电起走的软件钟;M4 由 SNTP 校准(is_synced 变 true)。
#pragma once

#include <stdbool.h>
#include <time.h>

void app_time_now(struct tm *out);
bool app_time_is_synced(void);

// 启动时调用一次:初始化 SNTP 客户端(必须在任何 start 之前)
void app_time_sntp_init(void);
// STA 拿到 IP 后调用一次:启动 SNTP 同步(幂等)
void app_time_sntp_start(void);
