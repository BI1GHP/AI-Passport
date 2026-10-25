// main/app_xiaozhi.c —— 小智AI 语音对话(xiaozhi-esp32 协议精简版,半双工)。
// 参考协议:OTA POST 取 websocket.url/token → WS(Authorization Bearer) →
// hello JSON → listen start + PCM 帧上行 → listen stop → 收 Opus 帧解码播放。
// ESP32-C3 无 PSRAM:编码器要 ~25KB 连续堆且与 OTA 的 TLS 峰值冲突,故上行改用
// PCM 直传(服务器按 hello 的 format 解上行);下行 TTS 固定 opus/24k,只保留
// 解码器(~19KB),OTA 完成后按需创建。
#include "app_xiaozhi.h"

#include "app_config.h"
#include "app_net.h"
#include "bsp_audio.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_opus_dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_xiaozhi";

#define XZ_RATE 16000
#define XZ_FRAME_MS 60
#define XZ_FRAME_SAMPLES (XZ_RATE / 1000 * XZ_FRAME_MS) // 960
#define XZ_PCM_BYTES (XZ_FRAME_SAMPLES * 2)             // 1920
#define XZ_DEFAULT_OTA "https://api.tenclass.net/xiaozhi/ota/"
#define XZ_QUEUE_LEN 16
#define XZ_RX_ACC 4096  // 单帧 PCM 上行上限(约 128ms@16k 单声道)
#define XZ_OUT_RATE 24000
#define XZ_OUT_BYTES (XZ_OUT_RATE / 1000 * XZ_FRAME_MS * 2) // 2880

// 队列事件类型
enum {
    EV_CLOSE = 1,     // 页面要求关闭
    EV_TALK_TOGGLE,   // 页面 OK 键
    EV_WS_CONNECTED,
    EV_WS_DISCONNECTED,
    EV_WS_TEXT,       // data = 堆上 C 字符串
    EV_WS_BIN,        // data = 堆上 PCM 帧(len 字节)
};

typedef struct {
    uint8_t type;
    uint16_t len;
    void *data;
} xz_evt_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_done;
static volatile bool s_running;
static volatile xz_state_t s_state;
static char s_act_code[8];
static char s_last_text[256];
static char s_mac_str[18];
static char s_client_id[40];

// WS 事件回调里做分片重组的静态缓冲(回调单线程,无需锁)
static uint8_t s_rx_acc[XZ_RX_ACC];

static void set_state(xz_state_t st) {
    s_state = st;
    ESP_LOGI(TAG, "状态 -> %d (空闲堆 %u)", (int)st,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

static bool post_evt_wait(uint8_t type, const void *data, uint16_t len, TickType_t wait) {
    if (!s_q) return false;
    xz_evt_t e = { .type = type, .len = len, .data = NULL };
    if (data && len) {
        e.data = malloc(len);
        if (!e.data) return false;
        memcpy(e.data, data, len);
    }
    if (xQueueSend(s_q, &e, wait) != pdTRUE) {
        free(e.data);
        return false;
    }
    return true;
}

static bool post_evt(uint8_t type, const void *data, uint16_t len) {
    return post_evt_wait(type, data, len, 0);
}

// 控制类文本(hello/stt/tts)不能丢:队列满时短暂等待;PCM 帧仍为非阻塞,丢帧只影响音频。
static void post_ws_payload(uint8_t op_code, const void *data, uint16_t len) {
    if (op_code == 0x01) {
        post_evt_wait(EV_WS_TEXT, data, len, pdMS_TO_TICKS(200));
    } else {
        post_evt(EV_WS_BIN, data, len);
    }
}

static void save_last_text(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return;
    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(t) && t->valuestring[0]) {
        strlcpy(s_last_text, t->valuestring, sizeof(s_last_text));
    }
    cJSON_Delete(root);
}

// ---- WebSocket 事件回调(运行在 esp_websocket_client 任务上下文) ----
static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    (void)arg; (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        post_evt(EV_WS_CONNECTED, NULL, 0);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        post_evt(EV_WS_DISCONNECTED, NULL, 0);
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (d->op_code != 0x01 && d->op_code != 0x02 && d->op_code != 0x00) break;
        if (d->data_len <= 0 || d->data_len > XZ_RX_ACC) break;
        if (d->payload_offset == 0) {
            if (d->payload_len >= d->data_len) {
                // 单帧完整送达
                post_ws_payload(d->op_code, d->data_ptr, (uint16_t)d->data_len);
            } else {
                memcpy(s_rx_acc, d->data_ptr, (size_t)d->payload_len);
            }
        } else {
            // 分片:拼接后投递
            size_t end = (size_t)d->payload_offset + d->payload_len;
            if (end > XZ_RX_ACC) break;
            memcpy(s_rx_acc + d->payload_offset, d->data_ptr, (size_t)d->payload_len);
            if (end >= (size_t)d->data_len) {
                post_ws_payload(d->op_code, s_rx_acc, (uint16_t)d->data_len);
            }
        }
        break;
    }
    default:
        break;
    }
}

// ---- OTA:激活检查 + 取 WS 地址 ----
// 成功返回 ESP_OK 并填充 url/token;待激活返回 ESP_ERR_NOT_FOUND(激活码已记录)。
static esp_err_t xz_ota(char *url, size_t url_len, char *token, size_t token_len) {
    const char *ota = app_config()->xz_server;
    if (!ota[0]) ota = XZ_DEFAULT_OTA;

    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "{\"application\":{\"version\":\"1.0.0\"},\"board\":{\"type\":\"badge-c3\"},"
        "\"mac_address\":\"%s\"}", s_mac_str);

    esp_http_client_config_t cfg = {
        .url = ota,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "BadgeFirmware/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", s_mac_str);
    esp_http_client_set_header(client, "Client-Id", s_client_id);

    esp_err_t err = ESP_FAIL;
    if (esp_http_client_open(client, body_len) == ESP_OK) {
        esp_http_client_write(client, body, body_len);
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        char *resp = malloc(4096);
        if (resp) {
            int n = esp_http_client_read(client, resp, 4095);
            if (n > 0) {
                resp[n] = '\0';
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    cJSON *act = cJSON_GetObjectItemCaseSensitive(root, "activation");
                    cJSON *code = act ? cJSON_GetObjectItemCaseSensitive(act, "code") : NULL;
                    cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
                    cJSON *jurl = ws ? cJSON_GetObjectItemCaseSensitive(ws, "url") : NULL;
                    cJSON *jtok = ws ? cJSON_GetObjectItemCaseSensitive(ws, "token") : NULL;
                    if (cJSON_IsString(code) && code->valuestring[0]) {
                        strlcpy(s_act_code, code->valuestring, sizeof(s_act_code));
                        err = ESP_ERR_NOT_FOUND; // 待激活
                    } else if (status == 200 && cJSON_IsString(jurl) && cJSON_IsString(jtok)) {
                        strlcpy(url, jurl->valuestring, url_len);
                        strlcpy(token, jtok->valuestring, token_len);
                        s_act_code[0] = '\0';
                        err = ESP_OK;
                    }
                    cJSON_Delete(root);
                }
            }
            free(resp);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void ws_send_text(esp_websocket_client_handle_t ws, const char *s) {
    if (esp_websocket_client_is_connected(ws)) {
        esp_websocket_client_send_text(ws, s, strlen(s), pdMS_TO_TICKS(2000));
    }
}

// 连接阶段的可中断等待:期间收到 EV_CLOSE 返回 true,其余事件丢弃。
static bool wait_or_close(TickType_t ticks) {
    xz_evt_t ev;
    if (xQueueReceive(s_q, &ev, ticks) != pdTRUE) return false;
    free(ev.data);
    return ev.type == EV_CLOSE;
}

// 打断时丢弃队列里已排队的下行音频帧,避免静音后又被旧 TTS 帧续上。
// 控制类事件暂存后按原顺序放回;队列只有 XZ_QUEUE_LEN 个槽,局部数组足够。
static void purge_audio_events(void) {
    if (!s_q) return;
    xz_evt_t keep[XZ_QUEUE_LEN];
    int n = 0;
    xz_evt_t ev;
    while (n < XZ_QUEUE_LEN && xQueueReceive(s_q, &ev, 0) == pdTRUE) {
        if (ev.type == EV_WS_BIN) {
            free(ev.data);
        } else {
            keep[n++] = ev;
        }
    }
    for (int i = 0; i < n; i++) {
        if (xQueueSendToBack(s_q, &keep[i], 0) != pdTRUE) free(keep[i].data);
    }
}

// ---- 会话主任务 ----
static void xz_task(void *arg) {
    (void)arg;
    esp_websocket_client_handle_t ws = NULL;
    void *dec = NULL;
    int16_t *pcm = NULL;
    int16_t *out_pcm = NULL;
    bool ws_ready = false;
    int think_ticks = 0;
    TickType_t recon_tick = 0; // 进入 CONNECTING 的时刻,用于重连超时判定

    set_state(XZ_ST_CONNECTING);
    if (!app_net_try_acquire("xiaozhi")) {
        ESP_LOGW(TAG, "网络忙");
        set_state(XZ_ST_ERROR);
        goto done;
    }

    pcm = malloc(XZ_PCM_BYTES);
    if (!pcm) {
        ESP_LOGE(TAG, "音频缓冲分配失败(空闲堆 %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        set_state(XZ_ST_ERROR);
        goto release_net;
    }

    // OTA 先做:TLS 握手需要 ~20KB, 此时还没有其他大块占用
    char ws_url[160] = {0};
    char ws_token[128] = {0};
connect:
    for (;;) {
        xz_evt_t ev;
        if (xQueueReceive(s_q, &ev, 0) == pdTRUE) { // 允许激活期间直接退出
            if (ev.type == EV_CLOSE) { goto release_net; }
            free(ev.data);
        }
        esp_err_t e = xz_ota(ws_url, sizeof(ws_url), ws_token, sizeof(ws_token));
        if (e == ESP_OK) break;
        if (e == ESP_ERR_NOT_FOUND) {
            set_state(XZ_ST_ACTIVATING);
            ESP_LOGI(TAG, "设备待激活,激活码 %s,5 秒后重试", s_act_code);
            if (wait_or_close(pdMS_TO_TICKS(5000))) goto release_net;
        } else {
            ESP_LOGW(TAG, "OTA 请求失败,3 秒后重试");
            // 退避期间保持可退出:16 秒关闭等待必须覆盖 OTA 15 秒超时后的这一段
            if (wait_or_close(pdMS_TO_TICKS(3000))) goto release_net;
        }
    }
    ESP_LOGI(TAG, "WS 地址: %s (空闲=%u 最大块=%u)", ws_url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // 解码器(~19KB 连续堆):OTA 已结束,此处最大块约 25KB,尚能容纳
    out_pcm = malloc(XZ_OUT_BYTES);
    esp_opus_dec_cfg_t dcfg = {
        .sample_rate = XZ_OUT_RATE, .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS, .self_delimited = false,
    };
    if (!out_pcm || esp_opus_dec_open(&dcfg, sizeof(dcfg), &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "Opus 解码器创建失败 空闲=%u 最大块=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        set_state(XZ_ST_ERROR);
        goto release_net;
    }
    ESP_LOGI(TAG, "解码器就绪 空闲=%u 最大块=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // WebSocket 连接
    {
        char headers[288];
        snprintf(headers, sizeof(headers),
                 "Authorization: Bearer %s\r\nDevice-Id: %s\r\nClient-Id: %s\r\n"
                 "Protocol-Version: 1\r\n", ws_token, s_mac_str, s_client_id);
        esp_websocket_client_config_t wcfg = {
            .uri = ws_url,
            .headers = headers,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .task_stack = 4096,
            .buffer_size = 512,
            .reconnect_timeout_ms = 5000,
            .enable_close_reconnect = true, // 服务器空闲会发 CLOSE 帧,默认不重连
            .network_timeout_ms = 10000,
        };
        ws = esp_websocket_client_init(&wcfg);
    }
    if (!ws) {
        set_state(XZ_ST_ERROR);
        goto release_net;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(ws) != ESP_OK) {
        ESP_LOGE(TAG, "WS 启动失败");
        set_state(XZ_ST_ERROR);
        goto release_net;
    }

    bsp_audio_set_format(XZ_RATE, 16, 1);
    bsp_audio_set_volume(app_config()->volume);
    recon_tick = xTaskGetTickCount(); // 从这里开始计时:20 秒未进 READY 则重建会话

    // 事件主循环
    for (;;) {
        xz_evt_t ev;
        TickType_t wait = (s_state == XZ_ST_LISTENING) ? 0 : pdMS_TO_TICKS(500);
        if (xQueueReceive(s_q, &ev, wait) == pdTRUE) {
            switch (ev.type) {
            case EV_CLOSE:
                goto ws_done;
            case EV_TALK_TOGGLE:
                if (s_state == XZ_ST_READY && ws_ready) {
                    ws_send_text(ws, "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}");
                    set_state(XZ_ST_LISTENING);
                } else if (s_state == XZ_ST_LISTENING) {
                    ws_send_text(ws, "{\"type\":\"listen\",\"state\":\"stop\"}");
                    set_state(XZ_ST_THINKING);
                    think_ticks = 0;
                } else if ((s_state == XZ_ST_THINKING || s_state == XZ_ST_SPEAKING) && ws_ready) {
                    // 打断:先让服务器中止当前回复,再直接开一轮新的聆听。
                    // 之后到达的旧 TTS 帧/文本因状态已是 LISTENING 而被丢弃。
                    ESP_LOGI(TAG, "按键打断,开始新一轮聆听");
                    purge_audio_events(); // 先丢掉已排队的旧回复音频
                    ws_send_text(ws, "{\"type\":\"abort\"}");
                    ws_send_text(ws, "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}");
                    if (s_state == XZ_ST_SPEAKING) {
                        // 播放路径是 24k,切回 16k 同时关闭再打开 codec,清掉已排队的下行 PCM
                        bsp_audio_set_format(XZ_RATE, 16, 1);
                    }
                    set_state(XZ_ST_LISTENING);
                }
                break;
            case EV_WS_CONNECTED:
                recon_tick = xTaskGetTickCount();
                ws_send_text(ws, "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
                                 "\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,"
                                 "\"channels\":1,\"frame_duration\":60}}");
                break;
            case EV_WS_DISCONNECTED:
                // 客户端会自动重连(含服务器空闲 CLOSE);这里只做状态回退
                ws_ready = false;
                if (s_state == XZ_ST_SPEAKING) bsp_audio_set_format(XZ_RATE, 16, 1);
                ESP_LOGW(TAG, "WS 断开,等待自动重连");
                set_state(XZ_ST_CONNECTING);
                recon_tick = xTaskGetTickCount();
                break;
            case EV_WS_TEXT: {
                char *txt = ev.data;
                ESP_LOGI(TAG, "收到文本: %.120s", txt ? txt : "");
                cJSON *root = txt ? cJSON_Parse(txt) : NULL;
                if (root) {
                    cJSON *tp = cJSON_GetObjectItemCaseSensitive(root, "type");
                    const char *t = cJSON_IsString(tp) ? tp->valuestring : "";
                    if (strcmp(t, "hello") == 0) {
                        ws_ready = true;
                        set_state(XZ_ST_READY);
                    } else if (strcmp(t, "stt") == 0 || strcmp(t, "tts") == 0) {
                        save_last_text(txt);
                        if (strcmp(t, "tts") == 0) {
                            cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
                            const char *s = cJSON_IsString(st) ? st->valuestring : "";
                            if (strcmp(s, "start") == 0 && s_state != XZ_ST_LISTENING) {
                                // 下行固定 opus/24k,播放前切到 24k(半双工,不会与录制冲突)
                                bsp_audio_set_format(XZ_OUT_RATE, 16, 1);
                                set_state(XZ_ST_SPEAKING);
                            } else if (strcmp(s, "stop") == 0 && s_state == XZ_ST_SPEAKING) {
                                bsp_audio_set_format(XZ_RATE, 16, 1);
                                set_state(XZ_ST_READY);
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
                break;
            }
            case EV_WS_BIN:
                // 下行是 opus/24k 帧,解码后直接送 codec
                if (s_state == XZ_ST_SPEAKING && dec && ev.len > 0) {
                    esp_audio_dec_in_raw_t raw = {
                        .buffer = (uint8_t *)ev.data, .len = ev.len,
                        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                    };
                    esp_audio_dec_out_frame_t fr = {
                        .buffer = (uint8_t *)out_pcm, .len = XZ_OUT_BYTES,
                    };
                    esp_audio_dec_info_t info;
                    if (esp_opus_dec_decode(dec, &raw, &fr, &info) == ESP_AUDIO_ERR_OK &&
                        fr.decoded_size > 0) {
                        bsp_audio_write(out_pcm, fr.decoded_size);
                    }
                }
                break;
            }
            free(ev.data);
            continue;
        }

        // 队列空:LISTENING 时采集一帧上行
        if (s_state == XZ_ST_LISTENING && ws_ready) {
            if (bsp_audio_read(pcm, XZ_PCM_BYTES) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            esp_websocket_client_send_bin(ws, (const char *)pcm, XZ_PCM_BYTES,
                                          pdMS_TO_TICKS(500));
            continue;
        }

        // THINKING 超时兜底:30 个空转周期(15 秒)未收到回复则回到 READY
        if (s_state == XZ_ST_THINKING && ++think_ticks >= 30) {
            ESP_LOGW(TAG, "等待回复超时");
            set_state(XZ_ST_READY);
        }

        // 兜底:连接已丢失但没收到 DISCONNECTED 事件(如重连被 401 拒绝)
        if (s_state != XZ_ST_CONNECTING && ws && !esp_websocket_client_is_connected(ws)) {
            ws_ready = false;
            if (s_state == XZ_ST_SPEAKING) bsp_audio_set_format(XZ_RATE, 16, 1);
            ESP_LOGW(TAG, "连接已丢失,转为重连");
            set_state(XZ_ST_CONNECTING);
            recon_tick = xTaskGetTickCount();
        }

        // 重连超时:自动重连 20 秒仍未就绪 → 重建会话(顺带刷新 OTA token)
        if (s_state == XZ_ST_CONNECTING &&
            (xTaskGetTickCount() - recon_tick) > pdMS_TO_TICKS(20000)) {
            ESP_LOGW(TAG, "重连超时,重建会话");
            goto restart;
        }
    }

ws_done:
    if (ws) {
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
        ws = NULL;
    }
    goto release_net;

restart:
    // 重建:释放 WS 与解码器,回到 connect 重新 OTA + 连接
    if (ws) {
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
        ws = NULL;
    }
    ws_ready = false;
    if (dec) {
        esp_opus_dec_close(dec);
        dec = NULL;
    }
    free(out_pcm);
    out_pcm = NULL;
    // 丢弃旧会话残留事件,保留 EV_CLOSE
    {
        xz_evt_t stale;
        while (xQueueReceive(s_q, &stale, 0) == pdTRUE) {
            free(stale.data);
            if (stale.type == EV_CLOSE) goto release_net;
        }
    }
    set_state(XZ_ST_CONNECTING);
    goto connect;

release_net:
    if (dec) esp_opus_dec_close(dec);
    free(pcm);
    free(out_pcm);
    app_net_release("xiaozhi");
done:
    set_state(XZ_ST_IDLE);
    s_running = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t app_xiaozhi_init(void) {
    if (s_q) return ESP_OK;
    s_q = xQueueCreate(XZ_QUEUE_LEN, sizeof(xz_evt_t));
    s_done = xSemaphoreCreateBinary();
    if (!s_q || !s_done) return ESP_ERR_NO_MEM;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac_str, sizeof(s_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint32_t r0 = esp_random(), r1 = esp_random(), r2 = esp_random(), r3 = esp_random();
    snprintf(s_client_id, sizeof(s_client_id), "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
             (unsigned long)r0, (unsigned long)(r1 & 0xFFFF),
             (unsigned long)((r1 >> 16) & 0x0FFF) | 0x4000,
             (unsigned long)(r2 & 0x3FFF) | 0x8000,
             (unsigned long)(r2 >> 16), (unsigned long)r3);
    return ESP_OK;
}

esp_err_t app_xiaozhi_open(void) {
    if (!s_q) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_OK;
    if (!app_net_sta_connected()) {
        ESP_LOGW(TAG, "未连接 Wi-Fi,无法开始会话");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "开始会话(空闲=%u 最大块=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    xSemaphoreTake(s_done, 0);
    s_act_code[0] = '\0';
    s_last_text[0] = '\0';
    s_running = true;
    if (xTaskCreate(xz_task, "badge_xz", 10240, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "会话任务创建失败(空闲=%u 最大块=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_xiaozhi_close(void) {
    if (!s_running) return;
    post_evt_wait(EV_CLOSE, NULL, 0, pdMS_TO_TICKS(200));
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(16000)) != pdTRUE) {
        ESP_LOGW(TAG, "小智任务未按时退出");
        s_running = false;
    }
}

xz_state_t app_xiaozhi_state(void) {
    return s_state;
}

esp_err_t app_xiaozhi_talk_toggle(void) {
    if (!s_running) return ESP_ERR_INVALID_STATE;
    // 思考中/回复中也允许切换(相当于打断当前回复,开始新一轮聆听)
    if (s_state != XZ_ST_READY && s_state != XZ_ST_LISTENING &&
        s_state != XZ_ST_THINKING && s_state != XZ_ST_SPEAKING) {
        return ESP_ERR_INVALID_STATE;
    }
    return post_evt(EV_TALK_TOGGLE, NULL, 0) ? ESP_OK : ESP_ERR_NO_MEM;
}

const char *app_xiaozhi_activation_code(void) {
    return s_act_code;
}

void app_xiaozhi_last_text(char *buf, size_t len) {
    if (!buf || !len) return;
    strlcpy(buf, s_last_text, len);
}
