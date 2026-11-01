// main/app_xiaozhi.c —— 小智AI 语音对话(xiaozhi-esp32 协议精简版,半双工)。
// 参考协议:OTA POST 取 websocket.url/token → WS(Authorization Bearer) →
// hello JSON → listen start + Opus 帧上行 → listen stop → 收 Opus 帧解码播放。
// ESP32-C3 无 PSRAM:Opus 编码器要大块连续堆,故会话期间暂停门户(释放 httpd 与
// 其缓冲),并采用半双工——同一时刻只保留编码器或解码器之一。
#include "app_xiaozhi.h"

#include "app_config.h"
#include "app_net.h"
#include "app_portal.h"
#include "app_time.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_xiaozhi";

// 内嵌完整信任链:IDF 自带 bundle 对这两条交叉签名链报 0x4290(签名校验失败),
// 自定义 bundle 也复现同样问题,故直接内嵌 PEM 链并作为 CA 池使用。
// news_ca.pem = 60s 新闻站链, xz_ca.pem = OTA/WS 链(G2 交叉 + G2 自签 + Global Root CA)。
extern const char news_ca_pem_start[] asm("_binary_news_ca_pem_start");
extern const char xz_ca_pem_start[] asm("_binary_xz_ca_pem_start");

#define XZ_RATE 16000
#define XZ_FRAME_MS 60      // 下行(服务端 TTS)帧长
#define XZ_ENC_FRAME_MS 20  // 上行编码帧长:SILK 编码栈占用随帧长剧增,20ms 才放得下
#define XZ_ENC_SAMPLES (XZ_RATE / 1000 * XZ_ENC_FRAME_MS) // 320
#define XZ_PCM_BYTES (XZ_ENC_SAMPLES * 2)                 // 640
#define XZ_DEFAULT_OTA "https://api.tenclass.net/xiaozhi/ota/"
#define XZ_QUEUE_LEN 16
#define XZ_RX_ACC 4096  // 单帧上行上限(约 128ms@16k 单声道)
#define XZ_OUT_RATE 24000
#define XZ_OUT_BYTES (XZ_OUT_RATE / 1000 * XZ_FRAME_MS * 2) // 2880
#define XZ_ENC_OUT_MAX 1024 // Opus 60ms 一帧编码后远小于此
#define XZ_ENC_BITRATE 32000

// 队列事件类型
enum {
    EV_CLOSE = 1,     // 页面要求关闭
    EV_TALK_TOGGLE,   // 页面 OK 键
    EV_ABORT,         // 仅打断当前回复,不开始新聆听
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
static bool s_portal_paused; // 会话期间主动停掉的门户,结束后恢复
static volatile xz_state_t s_state;
static volatile uint8_t s_talk_level; // 回复音频实时强度 0..100(口型同步)
static char s_act_code[8];
static char s_last_text[256];
static char s_last_stt[256];
static char s_mac_str[18];
static char s_client_id[40];
static char s_session_id[64];        // 服务端 hello 下发的会话号,MCP 回包原样带回

// WS 事件回调里做分片重组的静态缓冲(回调单线程,无需锁)
static uint8_t s_rx_acc[XZ_RX_ACC];

static void set_state(xz_state_t st) {
    s_state = st;
    if (st != XZ_ST_SPEAKING) s_talk_level = 0;
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
    cJSON *tp = cJSON_GetObjectItemCaseSensitive(root, "type");
    const bool is_stt = cJSON_IsString(tp) && strcmp(tp->valuestring, "stt") == 0;
    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(t) && t->valuestring[0]) {
        strlcpy(s_last_text, t->valuestring, sizeof(s_last_text));
        if (is_stt) strlcpy(s_last_stt, t->valuestring, sizeof(s_last_stt));
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
        .cert_pem = xz_ca_pem_start,
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

// ---- 编解码器按需创建:半双工,同一时刻只保留一个,避免无 PSRAM 下堆不够 ----
static uint8_t s_enc_out[XZ_ENC_OUT_MAX];
static void *s_enc_pre; // WS 刚连上时预置的编码器(堆最连续),首次聆听直接取用

static void *xz_enc_open(void) {
    if (s_enc_pre) { // 预分配的那份:交出去,释放后由后续 xz_enc_open 重新创建
        void *h = s_enc_pre;
        s_enc_pre = NULL;
        return h;
    }
    esp_opus_enc_config_t ecfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    ecfg.sample_rate = XZ_RATE;
    ecfg.channel = 1;
    ecfg.bits_per_sample = 16;
    ecfg.bitrate = XZ_ENC_BITRATE;
    ecfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    ecfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    ecfg.complexity = 0;
    ecfg.enable_fec = false;
    ecfg.enable_dtx = false;
    ecfg.enable_vbr = false;
    void *h = NULL;
    if (esp_opus_enc_open(&ecfg, sizeof(ecfg), &h) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus 编码器创建失败 空闲=%u 最大块=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return NULL;
    }
    ESP_LOGI(TAG, "Opus 编码器就绪 空闲=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return h;
}

static void *xz_dec_open(int16_t **out_pcm) {
    *out_pcm = malloc(XZ_OUT_BYTES);
    esp_opus_dec_cfg_t dcfg = {
        .sample_rate = XZ_OUT_RATE, .channel = 1,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS, .self_delimited = false,
    };
    void *dec = NULL;
    if (!*out_pcm || esp_opus_dec_open(&dcfg, sizeof(dcfg), &dec) != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "Opus 解码器创建失败 空闲=%u 最大块=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        free(*out_pcm);
        *out_pcm = NULL;
        return NULL;
    }
    ESP_LOGI(TAG, "Opus 解码器就绪 空闲=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return dec;
}

// 进入聆听:先释放解码器(半双工),再创建编码器
static bool xz_prepare_uplink(void **dec, int16_t **out_pcm, void **enc) {
    if (*dec) {
        esp_opus_dec_close(*dec);
        *dec = NULL;
    }
    free(*out_pcm);
    *out_pcm = NULL;
    if (!*enc) {
        ESP_LOGI(TAG, "准备上行 空闲=%u 最大块=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        *enc = xz_enc_open();
    }
    return *enc != NULL;
}

static void xz_end_uplink(void **enc) {
    if (*enc) {
        esp_opus_enc_close(*enc);
        *enc = NULL;
    }
}

// 会话退出时释放进页面预置的编码器(未被聆听用到的那份)
static void xz_free_pre_enc(void) {
    if (s_enc_pre) {
        esp_opus_enc_close(s_enc_pre);
        s_enc_pre = NULL;
    }
}

// ---- MCP 工具(小智协议 type:"mcp",payload 为 JSON-RPC 2.0) ----
#define MCP_TEXT_MAX 640
#define NEWS_URL "https://60s.viki.moe/v2/60s"
#define NEWS_JSON_MAX 4096
#define NEWS_ITEMS 5
#define NEWS_ITEM_BYTES 60

// 会话开始前预取的新闻。会话中空闲堆仅 ~16KB,再开 TLS 必失败。
static char s_news_text[MCP_TEXT_MAX];
static bool s_news_ready;

static void ws_send_mcp(esp_websocket_client_handle_t ws, cJSON *payload) {
    if (!ws || !esp_websocket_client_is_connected(ws)) {
        cJSON_Delete(payload);
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (s_session_id[0]) cJSON_AddStringToObject(root, "session_id", s_session_id);
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload);
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        int rc = esp_websocket_client_send_text(ws, s, strlen(s), pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "MCP 回包(%d): %.360s", rc, s);
        cJSON_free(s);
    }
    cJSON_Delete(root);
}

static void mcp_reply(esp_websocket_client_handle_t ws, const cJSON *id, cJSON *result) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(resp, "result", result);
    ws_send_mcp(ws, resp);
}

static void mcp_reply_text(esp_websocket_client_handle_t ws, const cJSON *id,
                           const char *text, bool is_err) {
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddBoolToObject(result, "isError", is_err);
    mcp_reply(ws, id, result);
}

static cJSON *mcp_schema(cJSON *props, cJSON *required) {
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    cJSON_AddItemToObject(s, "properties", props ? props : cJSON_CreateObject());
    if (required) cJSON_AddItemToObject(s, "required", required);
    return s;
}

static void add_tool(cJSON *arr, const char *name, const char *desc,
                     cJSON *props, cJSON *required) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON_AddItemToObject(t, "inputSchema", mcp_schema(props, required));
    cJSON_AddItemToArray(arr, t);
}

// 工具清单:顺序固定,常用工具在前(利于服务端 prompt cache)
static cJSON *mcp_tools(void) {
    cJSON *arr = cJSON_CreateArray();

    add_tool(arr, "self.get_device_status",
             "获取电子工牌实时状态：电量、电压、Wi-Fi 信号、当前时间、音量。",
             NULL, NULL);

    cJSON *props = cJSON_CreateObject();
    cJSON *vol = cJSON_CreateObject();
    cJSON_AddStringToObject(vol, "type", "integer");
    cJSON_AddNumberToObject(vol, "minimum", 0);
    cJSON_AddNumberToObject(vol, "maximum", 100);
    cJSON_AddStringToObject(vol, "description", "目标音量 0-100");
    cJSON_AddItemToObject(props, "volume", vol);
    cJSON *req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("volume"));
    add_tool(arr, "self.audio_speaker.set_volume",
             "设置扬声器音量，范围 0 到 100。当前音量未知时可先调用 self.get_device_status。",
             props, req);

    add_tool(arr, "self.news.brief",
             "获取今日新闻简报（约五条），用于朗读当天要闻。用户要求听新闻、讲新闻时调用。",
             NULL, NULL);

    return arr;
}

static size_t utf8_prefix(const char *s, size_t max_bytes) {
    size_t n = strlen(s);
    if (n <= max_bytes) return n;
    size_t i = max_bytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--; // 回退到字符起始字节
    return i;
}

// 抓取今日新闻到 out,失败返回 false;用内嵌 GTS Root R4 校验,规避 bundle 的 0x4290
static bool news_fetch(char *out, size_t len) {
    char *body = malloc(NEWS_JSON_MAX);
    if (!body) return false;

    esp_http_client_config_t cfg = {
        .url = NEWS_URL,
        .cert_pem = news_ca_pem_start,
        .timeout_ms = 8000,
        .user_agent = "BadgeFirmware/1.0",
        .buffer_size = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (!c) {
        free(body);
        return false;
    }
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        int n = 0;
        while (n < NEWS_JSON_MAX - 1) {
            int r = esp_http_client_read(c, body + n, NEWS_JSON_MAX - 1 - n);
            if (r <= 0) break;
            n += r;
        }
        body[n] = '\0';
        cJSON *root = n > 0 ? cJSON_Parse(body) : NULL;
        cJSON *data = root ? cJSON_GetObjectItemCaseSensitive(root, "data") : NULL;
        cJSON *news = data ? cJSON_GetObjectItemCaseSensitive(data, "news") : NULL;
        if (cJSON_IsArray(news)) {
            int cnt = cJSON_GetArraySize(news);
            size_t used = 0;
            out[0] = '\0';
            for (int i = 0; i < cnt && i < NEWS_ITEMS; i++) {
                cJSON *it = cJSON_GetArrayItem(news, i);
                if (!cJSON_IsString(it) || !it->valuestring[0]) continue;
                size_t m = utf8_prefix(it->valuestring, NEWS_ITEM_BYTES);
                int w = snprintf(out + used, len - used, "%d、%.*s。",
                                 i + 1, (int)m, it->valuestring);
                if (w < 0 || (size_t)w >= len - used) break;
                used += (size_t)w;
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    if (!ok) out[0] = '\0';
    return ok;
}

static void tool_device_status(char *out, size_t len) {
    char tbuf[24] = "未同步";
    if (app_time_is_synced()) {
        struct tm tm;
        app_time_now(&tm);
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
    }
    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    char bat[48];
    if (soc >= 0) snprintf(bat, sizeof(bat), "电量 %d%%（%.2f 伏）", soc, mv / 1000.0f);
    else snprintf(bat, sizeof(bat), "电量未知（%.2f 伏）", mv / 1000.0f);
    snprintf(out, len, "%s；Wi-Fi %s（信号 %d dBm）；时间 %s；音量 %d%%。",
             bat, app_net_sta_connected() ? "已连接" : "未连接",
             (int)app_net_sta_rssi(), tbuf, (int)app_config()->volume);
}

static void tool_set_volume(cJSON *args, char *out, size_t len, bool *is_err) {
    cJSON *v = args ? cJSON_GetObjectItemCaseSensitive(args, "volume") : NULL;
    if (!cJSON_IsNumber(v)) {
        snprintf(out, len, "缺少音量参数 volume（0-100）。");
        *is_err = true;
        return;
    }
    int vol = v->valueint;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    app_config()->volume = (uint8_t)vol;
    app_config_save();
    bsp_audio_set_volume((uint8_t)vol);
    snprintf(out, len, "音量已设为 %d%%。", vol);
}

static void mcp_handle(esp_websocket_client_handle_t ws, cJSON *root) {
    cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    cJSON *parsed = NULL;
    if (cJSON_IsString(payload)) { // 兼容 payload 为字符串的旧服务端
        parsed = cJSON_Parse(payload->valuestring);
        payload = parsed;
    }
    if (cJSON_IsObject(payload)) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(payload, "id");
        cJSON *method = cJSON_GetObjectItemCaseSensitive(payload, "method");
        const char *m = cJSON_IsString(method) ? method->valuestring : "";
        if (strcmp(m, "initialize") == 0) {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
            cJSON *caps = cJSON_CreateObject();
            cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
            cJSON_AddItemToObject(result, "capabilities", caps);
            cJSON *info = cJSON_CreateObject();
            cJSON_AddStringToObject(info, "name", "badge-c3");
            cJSON_AddStringToObject(info, "version", "1.0.0");
            cJSON_AddItemToObject(result, "serverInfo", info);
            if (id) mcp_reply(ws, id, result); else cJSON_Delete(result);
        } else if (strcmp(m, "tools/list") == 0) {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddItemToObject(result, "tools", mcp_tools());
            if (id) mcp_reply(ws, id, result); else cJSON_Delete(result);
        } else if (strcmp(m, "tools/call") == 0) {
            cJSON *params = cJSON_GetObjectItemCaseSensitive(payload, "params");
            cJSON *name = params ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
            cJSON *args = params ? cJSON_GetObjectItemCaseSensitive(params, "arguments") : NULL;
            const char *nm = cJSON_IsString(name) ? name->valuestring : "";
            char text[MCP_TEXT_MAX];
            text[0] = '\0';
            bool is_err = false;
            if (strcmp(nm, "self.get_device_status") == 0) {
                tool_device_status(text, sizeof(text));
            } else if (strcmp(nm, "self.audio_speaker.set_volume") == 0) {
                tool_set_volume(args, text, sizeof(text), &is_err);
            } else if (strcmp(nm, "self.news.brief") == 0) {
                // 会话中不联网:直接返回进入页面前预取的简报
                if (s_news_ready && s_news_text[0]) {
                    snprintf(text, sizeof(text), "%s", s_news_text);
                } else {
                    snprintf(text, sizeof(text), "新闻暂时没取到，请稍后再试。");
                    is_err = true;
                }
            } else {
                snprintf(text, sizeof(text), "未知工具：%s", nm);
                is_err = true;
            }
            if (id) mcp_reply_text(ws, id, text, is_err);
        } else if (id) { // 未知方法(如 notifications/initialized 无 id,不回复)
            cJSON *error = cJSON_CreateObject();
            cJSON_AddNumberToObject(error, "code", -32601);
            cJSON_AddStringToObject(error, "message", "Method not found");
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
            cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
            cJSON_AddItemToObject(resp, "error", error);
            ws_send_mcp(ws, resp);
        }
    }
    cJSON_Delete(parsed);
}

// ---- 预连接阶段:OTA + 新闻预取 ----
// 两步都要开 TLS(~20KB 连续堆)。若放在 24KB 会话任务里做,会话栈会吃掉唯一的大块,
// OTA 和 Opus 编码器(开机预分配)都拿不到连续内存,故拆成 6KB 栈的短命任务。
typedef struct {
    char url[160];
    char token[128];
    bool ready;
} xz_prep_t;
static xz_prep_t s_prep;

static void xz_task(void *arg);

// ---- 会话主任务 ----
static void xz_task(void *arg) {
    (void)arg;
    esp_websocket_client_handle_t ws = NULL;
    void *dec = NULL;
    void *enc = NULL;
    int16_t *pcm = NULL;
    int16_t *out_pcm = NULL;
    bool ws_ready = false;
    int think_ticks = 0;
    TickType_t recon_tick = 0; // 进入 CONNECTING 的时刻,用于重连超时判定

    set_state(XZ_ST_CONNECTING);
    // 预连接任务刚释放锁,天气/门户可能抢到;重试等待,别一次失败就放弃。
    bool locked = false;
    for (int i = 0; i < 100 && !locked; i++) {
        xz_evt_t ev;
        if (xQueueReceive(s_q, &ev, 0) == pdTRUE) {
            if (ev.type == EV_CLOSE) goto done;
            free(ev.data);
        }
        if (app_net_try_acquire("xiaozhi")) {
            locked = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!locked) {
        ESP_LOGW(TAG, "网络忙(owner=%s)",
                 app_net_lock_owner() ? app_net_lock_owner() : "?");
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
    if (s_prep.ready) {
        // 首次连接:OTA 结果由预连接任务填好,这里直接用,避免在会话栈上再握手
        strlcpy(ws_url, s_prep.url, sizeof(ws_url));
        strlcpy(ws_token, s_prep.token, sizeof(ws_token));
        s_prep.ready = false;
    } else {
        // 重连/重建:token 可能已过期,在本任务内重新 OTA
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
    }
    ESP_LOGI(TAG, "WS 地址: %s (空闲=%u 最大块=%u)", ws_url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // 新闻预取:OTA 的 TLS 峰值已结束、WS 还没连,此刻堆最充裕。
    // 会话中空闲堆仅 ~16KB,放不下第二次 TLS 握手。
    if (!s_news_ready) {
        s_news_ready = news_fetch(s_news_text, sizeof(s_news_text));
        ESP_LOGI(TAG, "新闻预取%s 空闲=%u 最大块=%u", s_news_ready ? "成功" : "失败",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    // 编解码器(~19/25KB 连续堆)改为按需创建:OTA 的 TLS 峰值已结束,
    // 半双工时聆听只用编码器、回复只用解码器,避免两者同时占用堆。

    // WebSocket 连接
    {
        char headers[288];
        snprintf(headers, sizeof(headers),
                 "Authorization: Bearer %s\r\nDevice-Id: %s\r\nClient-Id: %s\r\n"
                 "Protocol-Version: 1\r\n", ws_token, s_mac_str, s_client_id);
        esp_websocket_client_config_t wcfg = {
            .uri = ws_url,
            .headers = headers,
            .cert_pem = xz_ca_pem_start,
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
            case EV_ABORT:
                // 仅打断当前回复(语音录入待办:拿到 STT 后不需要聊天回复)
                purge_audio_events();
                ws_send_text(ws, "{\"type\":\"abort\"}");
                if (s_state == XZ_ST_LISTENING) xz_end_uplink(&enc);
                if (s_state == XZ_ST_SPEAKING) bsp_audio_set_format(XZ_RATE, 16, 1);
                if (s_state == XZ_ST_THINKING || s_state == XZ_ST_SPEAKING) {
                    set_state(XZ_ST_READY);
                }
                break;
            case EV_TALK_TOGGLE:
                if (s_state == XZ_ST_READY && ws_ready) {
                    if (!xz_prepare_uplink(&dec, &out_pcm, &enc)) {
                        set_state(XZ_ST_ERROR);
                        break;
                    }
                    ws_send_text(ws, "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}");
                    set_state(XZ_ST_LISTENING);
                } else if (s_state == XZ_ST_LISTENING) {
                    ws_send_text(ws, "{\"type\":\"listen\",\"state\":\"stop\"}");
                    xz_end_uplink(&enc); // 停止上行,让出堆给下行解码
                    set_state(XZ_ST_THINKING);
                    think_ticks = 0;
                } else if ((s_state == XZ_ST_THINKING || s_state == XZ_ST_SPEAKING) && ws_ready) {
                    // 打断:先让服务器中止当前回复,再直接开一轮新的聆听。
                    // 之后到达的旧 TTS 帧/文本因状态已是 LISTENING 而被丢弃。
                    ESP_LOGI(TAG, "按键打断,开始新一轮聆听");
                    purge_audio_events(); // 先丢掉已排队的旧回复音频
                    ws_send_text(ws, "{\"type\":\"abort\"}");
                    if (!xz_prepare_uplink(&dec, &out_pcm, &enc)) {
                        set_state(XZ_ST_ERROR);
                        break;
                    }
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
                ws_send_text(ws, "{\"type\":\"hello\",\"version\":1,"
                                 "\"features\":{\"mcp\":true},\"transport\":\"websocket\","
                                 "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                                 "\"channels\":1,\"frame_duration\":20}}");
                break;
            case EV_WS_DISCONNECTED:
                // 客户端会自动重连(含服务器空闲 CLOSE);这里只做状态回退
                ws_ready = false;
                // 释放编解码器,给 TLS 重连让出连续堆
                xz_end_uplink(&enc);
                if (dec) {
                    esp_opus_dec_close(dec);
                    dec = NULL;
                    free(out_pcm);
                    out_pcm = NULL;
                }
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
                        cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
                        if (cJSON_IsString(sid)) {
                            strlcpy(s_session_id, sid->valuestring, sizeof(s_session_id));
                        }
                        ws_ready = true;
                        set_state(XZ_ST_READY);
                    } else if (strcmp(t, "mcp") == 0) {
                        // 工具调用:同步处理(此时无音频流),结果直接回给服务端
                        mcp_handle(ws, root);
                    } else if (strcmp(t, "stt") == 0 || strcmp(t, "tts") == 0) {
                        save_last_text(txt);
                        if (strcmp(t, "tts") == 0) {
                            cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
                            const char *s = cJSON_IsString(st) ? st->valuestring : "";
                            if (strcmp(s, "start") == 0 && s_state != XZ_ST_LISTENING) {
                                // 下行固定 opus/24k,播放前切到 24k(半双工,不会与录制冲突)
                                if (!dec) {
                                    xz_end_uplink(&enc); // 停止上行后编码器不再需要
                                    dec = xz_dec_open(&out_pcm);
                                }
                                if (dec) {
                                    bsp_audio_set_format(XZ_OUT_RATE, 16, 1);
                                    set_state(XZ_ST_SPEAKING);
                                }
                            } else if (strcmp(s, "stop") == 0 && s_state == XZ_ST_SPEAKING) {
                                bsp_audio_set_format(XZ_RATE, 16, 1);
                                xz_end_uplink(&enc);
                                if (dec) { // 回复结束即释放解码器,留言给下一轮聆听
                                    esp_opus_dec_close(dec);
                                    dec = NULL;
                                    free(out_pcm);
                                    out_pcm = NULL;
                                }
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
                        // 口型同步:按本帧峰值映射 0..100,快攻慢放让口型自然
                        int16_t peak = 0;
                        int n = (int)(fr.decoded_size / 2);
                        for (int i = 0; i < n; i += 8) { // 抽 1/8 样点足够估计包络
                            int16_t v = out_pcm[i];
                            if (v < 0) v = (int16_t)-v;
                            if (v > peak) peak = v;
                        }
                        int lvl = (int)peak * 100 / 32768;
                        lvl = lvl * 3 / 2; // 语音峰值普遍不高,稍加增益
                        if (lvl > 100) lvl = 100;
                        int cur = s_talk_level;
                        s_talk_level = (uint8_t)(lvl > cur ? lvl : cur * 4 / 5);
                        bsp_audio_write(out_pcm, fr.decoded_size);
                    }
                }
                break;
            }
            free(ev.data);
            continue;
        }

        // 队列空:LISTENING 时采集一帧并编码成 opus 上行
        if (s_state == XZ_ST_LISTENING && ws_ready && enc) {
            if (bsp_audio_read(pcm, XZ_PCM_BYTES) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            esp_audio_enc_in_frame_t in = {
                .buffer = (uint8_t *)pcm, .len = XZ_PCM_BYTES,
            };
            esp_audio_enc_out_frame_t of = {
                .buffer = s_enc_out, .len = sizeof(s_enc_out),
            };
            if (esp_opus_enc_process(enc, &in, &of) == ESP_AUDIO_ERR_OK && of.encoded_bytes > 0) {
                // 编码后一帧约 200~300 字节,单次 WS 写不再被拆成 512 字节碎片
                esp_websocket_client_send_bin(ws, (const char *)s_enc_out,
                                              (int)of.encoded_bytes, pdMS_TO_TICKS(500));
            } else {
                ESP_LOGW(TAG, "opus 编码失败");
            }
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
            xz_end_uplink(&enc);
            if (dec) {
                esp_opus_dec_close(dec);
                dec = NULL;
                free(out_pcm);
                out_pcm = NULL;
            }
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
    // 重建:释放 WS 与编解码器,回到 connect 重新 OTA + 连接
    if (ws) {
        esp_websocket_client_stop(ws);
        esp_websocket_client_destroy(ws);
        ws = NULL;
    }
    ws_ready = false;
    xz_end_uplink(&enc);
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
    xz_end_uplink(&enc);
    if (dec) esp_opus_dec_close(dec);
    free(pcm);
    free(out_pcm);
    app_net_release("xiaozhi");
done:
    set_state(XZ_ST_IDLE);
    s_running = false;
    xz_free_pre_enc();
    if (s_portal_paused) { // 恢复会话期间暂停的配网门户
        s_portal_paused = false;
        app_portal_start();
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---- 预连接任务:OTA + 新闻预取(6KB 栈),完成后拉起 24KB 会话任务 ----
static void xz_prep_task(void *arg) {
    (void)arg;
    s_prep.ready = false;
    set_state(XZ_ST_CONNECTING);
    // 网络锁可能被天气刷新/门户占用(天气最长 12 秒),这里重试等待而不是
    // 一次失败就报错退出——否则"网络忙"会让小智直接连不上。
    bool locked = false;
    for (int i = 0; i < 60 && !locked; i++) {
        xz_evt_t ev;
        if (xQueueReceive(s_q, &ev, 0) == pdTRUE) {
            if (ev.type == EV_CLOSE) goto finish;
            free(ev.data);
        }
        if (app_net_try_acquire("xiaozhi")) {
            locked = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (!locked) {
        ESP_LOGW(TAG, "网络忙(owner=%s),放弃本次会话",
                 app_net_lock_owner() ? app_net_lock_owner() : "?");
        set_state(XZ_ST_ERROR);
        goto finish;
    }
    for (;;) {
        xz_evt_t ev;
        if (xQueueReceive(s_q, &ev, 0) == pdTRUE) {
            if (ev.type == EV_CLOSE) { app_net_release("xiaozhi"); goto finish; }
            free(ev.data);
        }
        esp_err_t e = xz_ota(s_prep.url, sizeof(s_prep.url), s_prep.token, sizeof(s_prep.token));
        if (e == ESP_OK) break;
        if (e == ESP_ERR_NOT_FOUND) {
            set_state(XZ_ST_ACTIVATING);
            ESP_LOGI(TAG, "设备待激活,激活码 %s,5 秒后重试", s_act_code);
            if (wait_or_close(pdMS_TO_TICKS(5000))) { app_net_release("xiaozhi"); goto finish; }
        } else {
            ESP_LOGW(TAG, "OTA 请求失败,3 秒后重试");
            if (wait_or_close(pdMS_TO_TICKS(3000))) { app_net_release("xiaozhi"); goto finish; }
        }
    }
    ESP_LOGI(TAG, "WS 地址: %s (空闲=%u 最大块=%u)", s_prep.url,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // 新闻预取:OTA 的 TLS 峰值已结束、WS 还没连,此刻堆最充裕
    if (!s_news_ready) {
        s_news_ready = news_fetch(s_news_text, sizeof(s_news_text));
        ESP_LOGI(TAG, "新闻预取%s 空闲=%u 最大块=%u", s_news_ready ? "成功" : "失败",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }

    app_net_release("xiaozhi"); // 让出网络锁,交给会话任务重新获取
    s_prep.ready = true;
    if (xTaskCreate(xz_task, "badge_xz", 24576, NULL, 5, NULL) == pdPASS) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGE(TAG, "会话任务创建失败");
    s_prep.ready = false;
    set_state(XZ_ST_ERROR);
finish:
    set_state(XZ_ST_IDLE);
    s_running = false;
    xz_free_pre_enc();
    if (s_portal_paused) {
        s_portal_paused = false;
        app_portal_start();
    }
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
    // 编码器改到进页面时分配(见 app_xiaozhi_open):开机不再常驻占 21KB,
    // 不玩小智时这段连续堆留给电台解码、天气/邮箱的 TLS 握手。
    return ESP_OK;
}

esp_err_t app_xiaozhi_open(void) {
    if (!s_q) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_OK;
    if (!app_net_sta_connected()) {
        ESP_LOGW(TAG, "未连接 Wi-Fi,无法开始会话");
        return ESP_ERR_INVALID_STATE;
    }
    // 会话期间暂停配网门户:httpd 及其缓冲占掉的就是编码器最缺的连续堆
    s_portal_paused = app_portal_running();
    if (s_portal_paused) app_portal_stop();

    // 编码器在这里预置:门户刚停、TLS 未开,是进页面后堆最连续的时刻。
    // 退出页面时由 xz_free_pre_enc() 释放,不做常驻占用。
    if (!s_enc_pre) {
        s_enc_pre = xz_enc_open();
    }

    ESP_LOGI(TAG, "开始会话(空闲=%u 最大块=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    xSemaphoreTake(s_done, 0);
    s_act_code[0] = '\0';
    s_last_text[0] = '\0';
    s_news_ready = false; // 每次进页面重新预取,保证新闻是当天的
    s_running = true;
    esp_wifi_set_ps(WIFI_PS_NONE); // 会话期间关省电,降低语音往返延迟
    // 先起 6KB 的预连接任务做 OTA/新闻,它再拉起 24KB 会话任务。
    // 建任务要连续堆,刚退出上一轮会话时可能暂时不够,短暂重试几次。
    bool ok = false;
    for (int i = 0; i < 4 && !ok; i++) {
        if (xTaskCreate(xz_prep_task, "badge_xz_prep", 6144, NULL, 5, NULL) == pdPASS) {
            ok = true;
        } else {
            ESP_LOGW(TAG, "会话任务创建失败(空闲=%u 最大块=%u),重试 %d",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), i + 1);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if (!ok) {
        s_running = false;
        if (s_portal_paused) {
            s_portal_paused = false;
            app_portal_start();
        }
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

void app_xiaozhi_last_stt(char *buf, size_t len) {
    if (!buf || !len) return;
    strlcpy(buf, s_last_stt, len);
}

void app_xiaozhi_clear_stt(void) {
    s_last_stt[0] = '\0';
}

void app_xiaozhi_abort(void) {
    if (!s_running) return;
    post_evt_wait(EV_ABORT, NULL, 0, pdMS_TO_TICKS(200));
}

uint8_t app_xiaozhi_talk_level(void) {
    return s_state == XZ_ST_SPEAKING ? s_talk_level : 0;
}
