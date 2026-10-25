// main/app_mail.h —— IMAP 收件(最新 5 封邮件头),后台任务 + 快照。
#pragma once

#include "esp_err.h"
#include "imap_parse.h"
#include <stdbool.h>

#define APP_MAIL_MAX 5

esp_err_t app_mail_init(void);

bool app_mail_configured(void); // 已填写邮箱账号与授权码
esp_err_t app_mail_start(void); // 异步收取;未配置返回 ESP_ERR_INVALID_STATE
void app_mail_cancel(void);     // 请求停止并等待任务退出(有界)
bool app_mail_busy(void);

// 复制当前结果;返回邮件数。err 返回最近一次任务结果(ESP_OK 或失败码)。
int app_mail_copy(imap_msg_t *out, int max, esp_err_t *err);

// 删除本地快照中的一条(不改服务器端邮件);index 越界时忽略。
void app_mail_delete(int index);
