// main/app_mail.c
#include "app_mail.h"

#include "app_config.h"
#include "app_net.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_mail";
#define MAIL_BUF_SIZE 12288
#define READ_TIMEOUT_S 2

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;
static volatile bool s_busy;
static volatile bool s_stop;

static imap_msg_t s_msgs[APP_MAIL_MAX];
static int s_count;
static esp_err_t s_error = ESP_OK;

typedef struct {
    const char *host;
    int port;
} provider_t;

static const provider_t PROVIDERS[] = {
    { "imap.qq.com", 993 },
    { "imap.163.com", 993 },
    { "imap.gmail.com", 993 },
    { "outlook.office365.com", 993 },
};

static const provider_t *provider_for(const char *name) {
    if (strcmp(name, "qq") == 0) return &PROVIDERS[0];
    if (strcmp(name, "163") == 0) return &PROVIDERS[1];
    if (strcmp(name, "gmail") == 0) return &PROVIDERS[2];
    if (strcmp(name, "outlook") == 0) return &PROVIDERS[3];
    return &PROVIDERS[0];
}

bool app_mail_configured(void) {
    const app_config_t *c = app_config();
    return c->mail_user[0] && c->mail_pass[0];
}

esp_err_t app_mail_init(void) {
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_done) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool app_mail_busy(void) {
    return s_busy;
}

static bool send_all(esp_tls_t *tls, const char *data) {
    size_t len = strlen(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = esp_tls_conn_write(tls, data + sent, len - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

// 读到响应标签(A1 OK / A1 NO / A1 BAD)为止。
static bool read_until_tag(esp_tls_t *tls, char *buf, size_t cap, size_t *used,
                           const char *tag, bool *ok) {
    int timeouts = 0;
    for (;;) {
        if (s_stop) return false;
        if (imap_find_tag_status(buf, *used, tag, ok)) return true;
        if (*used + 1 >= cap) return false; // 响应过大
        ssize_t n = esp_tls_conn_read(tls, buf + *used, cap - 1 - *used);
        if (n > 0) {
            *used += (size_t)n;
            timeouts = 0;
            continue;
        }
        if (n == 0) return false;
        if (++timeouts > 1 + (READ_TIMEOUT_S * 2)) return false;
    }
}

static bool read_greeting(esp_tls_t *tls) {
    char buf[256];
    size_t used = 0;
    for (int i = 0; i < 8; i++) {
        if (s_stop) return false;
        if (used + 1 < sizeof(buf)) {
            ssize_t n = esp_tls_conn_read(tls, buf + used, sizeof(buf) - 1 - used);
            if (n > 0) {
                used += (size_t)n;
                buf[used] = '\0';
                if (strstr(buf, "\r\n")) return strstr(buf, "OK") != NULL;
                continue;
            }
            if (n == 0) return false;
        } else {
            return false;
        }
    }
    return false;
}

static void quote(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < cap; i++) {
        if (in[i] == '\\' || in[i] == '"') out[o++] = '\\';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

static void publish(const imap_msg_t *msgs, int count, esp_err_t err) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (count > 0) memcpy(s_msgs, msgs, (size_t)count * sizeof(imap_msg_t));
    s_count = count;
    s_error = err;
    xSemaphoreGive(s_lock);
}

static void mail_task(void *arg) {
    (void)arg;
    char *buf = malloc(MAIL_BUF_SIZE);
    esp_tls_t *tls = NULL;
    imap_msg_t msgs[APP_MAIL_MAX];
    int found = -1;
    esp_err_t err = ESP_FAIL;

    if (!buf) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    if (!app_net_sta_connected()) {
        ESP_LOGW(TAG, "Wi-Fi 未连接");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    if (!app_net_try_acquire("mail")) {
        ESP_LOGW(TAG, "其他网络任务占用中");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    const app_config_t *c = app_config();
    const provider_t *p = provider_for(c->mail_provider);
    char user[APP_CFG_USER_LEN * 2 + 1];
    char pass[APP_CFG_PASS_LEN * 2 + 1];
    quote(c->mail_user, user, sizeof(user));
    quote(c->mail_pass, pass, sizeof(pass));

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    tls = esp_tls_init();
    if (!tls) {
        err = ESP_ERR_NO_MEM;
        goto release_net;
    }
    if (esp_tls_conn_new_sync(p->host, (int)strlen(p->host), p->port, &cfg, tls) != 1) {
        ESP_LOGE(TAG, "连接 %s:%d 失败", p->host, p->port);
        goto release_net;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK) {
        struct timeval tv = { .tv_sec = READ_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    if (!read_greeting(tls)) {
        ESP_LOGE(TAG, "IMAP 问候失败");
        goto release_net;
    }

    char cmd[512];
    size_t used = 0;
    bool ok = false;
    snprintf(cmd, sizeof(cmd), "A1 LOGIN \"%s\" \"%s\"\r\n", user, pass);
    if (!send_all(tls, cmd) || !read_until_tag(tls, buf, MAIL_BUF_SIZE, &used, "A1", &ok) || !ok) {
        ESP_LOGE(TAG, "登录失败");
        goto release_net;
    }

    // 163/126 等服务端要求客户端先声明身份(RFC 2971),否则 SELECT 返回 NO Unsafe Login。
    // 只在 QQ/163 上发送;Gmail/Outlook 不需要且不保证支持。
    if (p == &PROVIDERS[0] || p == &PROVIDERS[1]) {
        send_all(tls, "A0 ID (\"name\" \"FoloToy Passport\" \"version\" \"1.0\" \"vendor\" \"FoloToy\")\r\n");
        used = 0;
        read_until_tag(tls, buf, MAIL_BUF_SIZE, &used, "A0", &ok);
    }

    used = 0;
    if (!send_all(tls, "A2 SELECT \"INBOX\"\r\n") ||
        !read_until_tag(tls, buf, MAIL_BUF_SIZE, &used, "A2", &ok) || !ok) {
        ESP_LOGE(TAG, "打开收件箱失败");
        goto release_net;
    }
    int exists = imap_parse_exists(buf, used);
    if (exists <= 0) {
        found = 0;
        err = ESP_OK;
        goto logout;
    }

    int first = exists - APP_MAIL_MAX + 1;
    if (first < 1) first = 1;
    used = 0;
    snprintf(cmd, sizeof(cmd), "A3 FETCH %d:%d BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)]\r\n",
             first, exists);
    if (!send_all(tls, cmd) ||
        !read_until_tag(tls, buf, MAIL_BUF_SIZE, &used, "A3", &ok) || !ok) {
        ESP_LOGE(TAG, "读取邮件头失败");
        goto release_net;
    }
    found = imap_parse_fetch_headers(buf, used, msgs, APP_MAIL_MAX);
    err = ESP_OK;

logout:
    send_all(tls, "A4 LOGOUT\r\n");
release_net:
    if (tls) esp_tls_conn_destroy(tls);
    tls = NULL;
    app_net_release("mail");
done:
    free(buf);
    publish(msgs, found < 0 ? 0 : found, err);
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_busy = false;
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t app_mail_start(void) {
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!app_mail_configured()) return ESP_ERR_INVALID_STATE;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_done, 0); // 清掉上一次残留的完成信号
    s_busy = true;
    s_stop = false;
    if (xTaskCreate(mail_task, "badge_mail", 8192, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_mail_cancel(void) {
    if (!s_busy) return;
    s_stop = true;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(11000)) != pdTRUE) {
        ESP_LOGW(TAG, "收取任务未在超时内退出");
        s_busy = false;
    }
}

int app_mail_copy(imap_msg_t *out, int max, esp_err_t *err) {
    if (!out || max <= 0) return 0;
    int n = 0;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        n = s_count < max ? s_count : max;
        memcpy(out, s_msgs, (size_t)n * sizeof(imap_msg_t));
        if (err) *err = s_error;
        xSemaphoreGive(s_lock);
    } else if (err) {
        *err = ESP_FAIL;
    }
    return n;
}

void app_mail_delete(int index) {
    if (!s_lock || index < 0) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (index < s_count) {
        for (int i = index; i + 1 < s_count; i++) s_msgs[i] = s_msgs[i + 1];
        s_count--;
    }
    xSemaphoreGive(s_lock);
}
