// main/app_radio.c —— 国内内置精选电台(MP3 直链,蜻蜓 CDN)+ Helix 流式解码播放。
// CNR 音乐之声固定置顶;无需联网探测,进入页面即显示列表。
#include "app_radio.h"

#include "app_config.h"
#include "app_net.h"
#include "app_portal.h"
#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mp3dec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_radio";
// 压缩流缓冲 8KB(静态 BSS):128kbps 码流约 0.5 秒音频。
// 抗网络抖动主要靠 PCM 环形缓冲,此处只为解码供料,不宜过大(静态 RAM 宝贵)。
#define IN_BUF_SIZE (8 * 1024)
#define PCM_SAMPLES (MAX_NCHAN * MAX_NSAMP * 2)
#define HTTP_TIMEOUT_MS 15000
// 单次 HTTP 读请求上限。esp_http_client_read 会尽量填满请求长度才返回;
// 直播流按实时速率送达(64kbps≈8KB/s)。小步(1KB)读虽返回快,但每次读固定
// 付出一次 TCP 往返/唤醒开销,实测 1KB≈200~300ms,只带回 ~4 帧(≈120ms 音频),
// 供给恒小于实时消费,ring 被持续抽干(帧计数实测仅 17~31 帧/秒,实时需 41.7)。
// 一次读 4KB(≈16 帧/0.5 秒音频)把往返开销摊薄,平均供给速率回到实时以上。
#define READ_CHUNK 4096

// 内置国内电台。首项固定置顶。
// 其后为此前被误判为 AAC 而剔除的 10 台
// (逐台 112KB 采样实测 MP3 帧头 540~780 个,全部为真 MP3 64k 流)。
static const app_radio_station_t BUILTIN[] = {
    {"CNR 音乐之声", "http://lhttp.qtfm.cn/live/1260/64k.mp3"},
    {"北京交通广播", "http://lhttp.qtfm.cn/live/336/64k.mp3"},
    {"北京音乐广播", "http://lhttp.qtfm.cn/live/332/64k.mp3"},
    {"北京文艺广播", "http://lhttp.qtfm.cn/live/333/64k.mp3"},
    {"北京新闻广播", "http://lhttp.qtfm.cn/live/339/64k.mp3"},
    {"北京城市广播", "http://lhttp.qtfm.cn/live/345/64k.mp3"},
    {"京津冀之声", "http://lhttp.qtfm.cn/live/5022463/64k.mp3"},
    {"HIT FM 88.7", "http://lhttp.qtfm.cn/live/15318703/64k.mp3"},
    {"国际新闻广播", "http://lhttp.qtfm.cn/live/20500172/64k.mp3"},
    {"上海新闻广播", "http://lhttp.qtfm.cn/live/270/64k.mp3"},
    {"上海动感101", "http://lhttp.qtfm.cn/live/274/64k.mp3"},
    {"欢乐调频", "http://lhttp.qtfm.cn/live/15318317/64k.mp3"},
    {"江苏经典音乐", "http://lhttp.qtfm.cn/live/4938/64k.mp3"},
    {"清晨音乐台", "http://lhttp.qingting.fm/live/4915/64k.mp3"},
    {"AsiaFM 亚洲台", "http://lhttp.qtfm.cn/live/15318569/64k.mp3"},
    {"温州音乐之声", "http://lhttp.qtfm.cn/live/1149/64k.mp3"},
    {"华语经典500", "http://lhttp.qtfm.cn/live/5022308/64k.mp3"},
    {"云梦音乐广播", "http://lhttp.qtfm.cn/live/20500187/64k.mp3"},
    {"星空音乐广播", "http://lhttp.qtfm.cn/live/5022379/64k.mp3"},
    {"内蒙古音乐之声", "http://lhttp-hw.qtfm.cn/live/1886/64k.mp3"},
};
#define BUILTIN_COUNT ((int)(sizeof(BUILTIN) / sizeof(BUILTIN[0])))

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_play_done;
static app_radio_station_t s_stations[APP_RADIO_MAX];
static int s_count;
static volatile bool s_playing;
static volatile bool s_stop_play;
static volatile int s_current = -1;

// PCM 环形缓冲(静态 StreamBuffer,16KB ≈ 44.1kHz 立体声 93ms 音频)。
// I2S DMA 仅 ~33ms,单任务"读网络→解码→写 I2S"串行链路会在每次 HTTP 读
// 窗口抽空 DMA,表现为周期性卡顿(实测复现)。独立 I2S 写入任务与网络读解耦,
// 抖动由本缓冲吸收,叠加 DMA 共约 126ms 抗抖动窗口。
// 全静态分配:C3 堆碎片化,>16KB 连续分配运行时经常失败,加大 I2S DMA
// (desc 连续分配)已实测在格式重配时分配失败并 panic,不可行。
// 尺寸权衡:32KB 版本实测把启动堆从 92KB 压到 57KB,Helix 解码器(需 ~14KB
// 连续堆)随之初始化失败;16KB 版本播放期空闲堆仅 ~7KB,TCP 停顿仍频发。
// 12KB(≈70ms 抖动窗口,叠加 DMA ~33ms)配合 LVGL 显存减半,播放期堆回升到
// ~19KB,供流稳定性优先于缓冲深度。
#define PCM_RING_BYTES (12 * 1024)
static uint8_t s_ring_storage[PCM_RING_BYTES];
static StaticStreamBuffer_t s_ring_struct;
static StreamBufferHandle_t s_ring;
static SemaphoreHandle_t s_writer_done;
static volatile bool s_feed_done;

esp_err_t app_radio_init(void) {
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_play_done = xSemaphoreCreateBinary();
    s_writer_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_play_done || !s_writer_done) return ESP_ERR_NO_MEM;
    app_radio_load(); // 启动时装载内置电台,进入页面即可播放
    return ESP_OK;
}

bool app_radio_loading(void) {
    return false; // 内置列表同步就绪,无异步加载态
}

esp_err_t app_radio_load_error(void) {
    return ESP_OK;
}

int app_radio_count(void) {
    return s_count;
}

bool app_radio_station(int index, app_radio_station_t *out) {
    if (!out || index < 0 || index >= s_count) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    *out = s_stations[index];
    xSemaphoreGive(s_lock);
    return true;
}

bool app_radio_playing(void) {
    return s_playing;
}

int app_radio_current(void) {
    return s_current;
}

esp_err_t app_radio_load(void) {
    if (!s_lock) {
        ESP_LOGW(TAG, "未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    int n = BUILTIN_COUNT < APP_RADIO_MAX ? BUILTIN_COUNT : APP_RADIO_MAX;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        memcpy(s_stations, BUILTIN, (size_t)n * sizeof(app_radio_station_t));
        s_count = n;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "内置国内电台 %d 台就绪", n);
    return ESP_OK;
}

// ---- 频谱(8 频段 Goertzel,播放页可视化用;计算在播放任务内,CPU 开销可忽略) ----
static float s_spec[APP_RADIO_SPEC_BANDS];   // 0..100 显示值(快攻慢放)
static float s_coeff[APP_RADIO_SPEC_BANDS];
static int s_spec_sr;

static void spec_setup(int samprate) {
    // 频段中心频率(Hz)对数分布,钳到奈奎斯特以下
    static const float f[APP_RADIO_SPEC_BANDS] = { 120, 300, 700, 1500, 3000, 6000, 10000, 15000 };
    s_spec_sr = samprate;
    for (int b = 0; b < APP_RADIO_SPEC_BANDS; b++) {
        float fc = f[b];
        if (fc > samprate * 0.45f) fc = samprate * 0.45f;
        s_coeff[b] = 2.0f * cosf(2.0f * 3.14159265f * fc / (float)samprate);
    }
    memset(s_spec, 0, sizeof(s_spec));
}

static void spec_feed(const short *pcm, int frames, int chans) {
    if (!s_spec_sr || frames <= 0) return;
    for (int b = 0; b < APP_RADIO_SPEC_BANDS; b++) {
        float c = s_coeff[b], s1 = 0, s2 = 0;
        for (int i = 0; i < frames; i++) {
            float x = (float)pcm[i * chans]; // 左声道能量足够可视化
            float s0 = x + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float mag = sqrtf(s1 * s1 + s2 * s2 - c * s1 * s2) / (float)frames / 32768.0f;
        float v = mag * 600.0f;
        if (v > 1.0f) v = 1.0f + log10f(v); // 大信号对数压缩
        v *= 55.0f;
        if (v > 100.0f) v = 100.0f;
        s_spec[b] = v > s_spec[b] ? v : s_spec[b] * 0.82f; // 快攻慢放
    }
}

void app_radio_spectrum(uint8_t *out) {
    for (int b = 0; b < APP_RADIO_SPEC_BANDS; b++) {
        float v = s_playing ? s_spec[b] : 0.0f;
        out[b] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    }
}

// 播放缓冲放静态区(BSS):ESP32-C3 堆紧张,运行时 malloc 8KB+4.6KB 常与
// HTTPS mbedtls 争抢导致分配失败。静态分配牺牲常驻 RAM 换取确定性。
static unsigned char s_in_buf[IN_BUF_SIZE];
static short s_pcm_buf[PCM_SAMPLES];

static volatile uint32_t s_underruns; // ring 抽空次数(爆音诊断)

// I2S 写入任务:从环形缓冲取 PCM 阻塞写入 codec,随硬件速率排空。
static void pcm_writer_task(void *arg) {
    (void)arg;
    uint8_t buf[1536];
    // ring 抽空(网络抖动期)必须向 I2S 补静音:抽空不管时 DMA 会循环播放
    // 最后一段陈旧数据,听感为周期性 buzz/爆音;补静音则退化为短暂无声间隙。
    static const uint8_t silence[1536]; // BSS 零初始化
    bool ramp_in = false;
    while (!s_stop_play) {
        size_t n = xStreamBufferReceive(s_ring, buf, sizeof(buf), pdMS_TO_TICKS(200));
        if (n == 0) {
            if (s_feed_done) break; // 流已结束且缓冲排空
            s_underruns++;
            ramp_in = true;
            bsp_audio_write(silence, sizeof(silence));
            continue;
        }
        // 静音↔音频硬切换本身就是 click:边界各做 ~1.3ms 线性斜坡消除。
        int16_t *s = (int16_t *)buf;
        int samples = (int)(n / 2);
        int ramp = samples < 128 ? samples : 128;
        if (ramp_in) {
            for (int i = 0; i < ramp; i++) s[i] = (int16_t)((int32_t)s[i] * i / ramp);
            ramp_in = false;
        } else if (xStreamBufferBytesAvailable(s_ring) == 0 && !s_feed_done) {
            // ring 即将抽空:本笔是最后真实数据,末尾淡出避免截断 click
            for (int i = 0; i < ramp; i++) {
                int idx = samples - ramp + i;
                s[idx] = (int16_t)((int32_t)s[idx] * (ramp - i) / ramp);
            }
        }
        // 写失败只可能是格式未就绪/超时(限时取锁);限频记录,I2S 若有异常可从这里看出来
        esp_err_t we = bsp_audio_write(buf, n);
        static int wfail;
        if (we != ESP_OK && (wfail++ % 50) == 0) {
            ESP_LOGW(TAG, "I2S 写入失败: %s", esp_err_to_name(we));
        }
    }
    xSemaphoreGive(s_writer_done);
    vTaskDelete(NULL);
}

// ---- 去"沙沙"低通(仅电台链路) ----
// 蜻蜓全部为 64kbps MP3(实测 128k/aac 变体均 404,码率无法再高),64k 编码器把
// 10kHz 以上大量谱线量化成噪声,在安静段落尤其明显,高音小喇叭会放大成"沙——"。
// 两级一阶低通(fc≈10kHz,约 -2dB@10k、-7dB@15k),定点 Q15,每样点 2 次乘法。
static int32_t s_lp1[2], s_lp2[2];
static int32_t s_lp_a = 23921; // Q15,48000Hz 对应值

static void lp_setup(int samprate) {
    switch (samprate) {
        case 48000: s_lp_a = 23921; break;
        case 44100: s_lp_a = 24881; break;
        case 32000: s_lp_a = 28164; break;
        case 24000: s_lp_a = 30373; break;
        default:    s_lp_a = 23921; break;
    }
    s_lp1[0] = s_lp1[1] = s_lp2[0] = s_lp2[1] = 0;
}

static inline int16_t lp_proc(int ch, int32_t x) {
    s_lp1[ch] += ((x - s_lp1[ch]) * s_lp_a) >> 15;
    s_lp2[ch] += ((s_lp1[ch] - s_lp2[ch]) * s_lp_a) >> 15;
    return (int16_t)s_lp2[ch];
}

// MPEG Layer III 帧头校验(自实现,替代失效的 useSize 参数)。
// 返回值:1=合法且整帧已在缓冲内;0=不是合法帧头(跳过 1 字节重找);-1=数据不足一帧。
static int mp3_hdr_check(const unsigned char *h, int avail, int *flen_out, int *sr_out, int *ch_out) {
    if (avail < 4) return -1;
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    const int ver = (h[1] >> 3) & 3;   // 0=MPEG2.5, 2=MPEG2, 3=MPEG1
    const int layer = (h[1] >> 1) & 3; // 1=Layer III
    if (ver == 1 || layer != 1) return 0;
    const int bri = (h[2] >> 4) & 0xF;
    const int sri = (h[2] >> 2) & 3;
    if (bri == 0 || bri == 15 || sri == 3) return 0;
    static const int br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const int br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const int sr1[3] = { 44100, 48000, 32000 };
    static const int sr2[3] = { 22050, 24000, 16000 };
    static const int sr25[3] = { 11025, 12000, 8000 };
    const int sr = ver == 3 ? sr1[sri] : (ver == 2 ? sr2[sri] : sr25[sri]);
    const int kbps = ver == 3 ? br1[bri] : br2[bri];
    const int pad = (h[2] >> 1) & 1;
    const int flen = (ver == 3 ? 144000 : 72000) * kbps / sr + pad;
    if (flen < 4) return 0;
    if (flen > avail) return -1;
    *flen_out = flen;
    *sr_out = sr;
    *ch_out = ((h[3] >> 6) & 3) == 3 ? 1 : 2;
    return 1;
}

static void play_task(void *arg) {
    (void)arg;
    app_radio_station_t station;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        s_playing = false;
        xSemaphoreGive(s_play_done);
        vTaskDelete(NULL);
        return;
    }
    station = s_stations[s_current];
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "开始播放: %s", station.name);

    unsigned char *in = s_in_buf;
    short *pcm = s_pcm_buf;
    esp_http_client_handle_t client = NULL;
    HMP3Decoder dec = NULL;
    int used = 0;
    bool format_set = false;
    bool writer_started = false;
    bool portal_paused = false; // 为解码器腾堆而暂停门户,退出时恢复
    // 网络锁可能被天气等短时占用,重试几次
    int tries = 0;
    while (!app_net_try_acquire("radio-play") && tries < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
        tries++;
    }
    if (tries >= 20) {
        ESP_LOGW(TAG, "网络忙,无法播放(owner=%s)", app_net_lock_owner() ? app_net_lock_owner() : "?");
        goto done;
    }
    // 播放期间关闭 Wi-Fi modem sleep:省电模式下 AP 按 DTIM(~102ms)批量缓存
    // 投递报文,实时流读会周期性停顿数百毫秒甚至数秒(诊断实测单次读 4831ms),
    // ring+DMA 的 ~126ms 缓冲根本扛不住,表现为周期性卡顿。停播后恢复。
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_http_client_config_t cfg = {
        .url = station.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .user_agent = "BadgeFirmware/1.0",
        .buffer_size = 2048,
        .buffer_size_tx = 512,
    };
    client = esp_http_client_init(&cfg);
    if (!client) goto release_net;
    if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(TAG, "电台连接失败,空闲堆=%u", (unsigned)esp_get_free_heap_size());
        goto release_net;
    }
    // Helix 解码器约需 24KB 连续堆,空闲块不够时先停门户
    // (httpd 栈+socket ~10KB),播放结束后自动恢复。
    {
        uint32_t need = 24 * 1024;
        if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < need && app_portal_running()) {
            app_portal_stop();
            portal_paused = true;
            ESP_LOGI(TAG, "已暂停门户以释放堆,空闲=%u 最大块=%u",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }
    dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGE(TAG, "MP3 解码器初始化失败,空闲堆=%u 最大块=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        goto release_net; // portal 在 release_net 统一恢复
    }
    s_ring = xStreamBufferCreateStatic(PCM_RING_BYTES, 1, s_ring_storage, &s_ring_struct);
    if (!s_ring) {
        ESP_LOGE(TAG, "PCM 环形缓冲创建失败");
        goto release_net;
    }
    s_feed_done = false;
    xSemaphoreTake(s_writer_done, 0); // 清掉上次可能残留的信号

    bsp_audio_set_volume(app_config()->volume);
    s_underruns = 0;
    uint32_t last_log = 0;
    int frames_total = 0;     // 累计解出帧数
    used = esp_http_client_read(client, (char *)in, IN_BUF_SIZE);
        if (used < 0) used = 0;

        int bytes_total = used;   // 累计读入(格式探测)
        // 格式判决需要连续 3 帧一致:流头/ID3 里的随机 0xFF 会产生"假同步帧",
        // Helix 对它返回 rc=NONE 但 FrameInfo 可给出看似合法却错误的参数
        // (实测同一 48kHz 的北京交通曾被首帧误判成 44100 → 整体变调/发闷)。
        int sr_vote = 0, ch_vote = 0, votes = 0;
        while (!s_stop_play) {
            // 每 10 秒诊断日志:爆音定位证据(ring 水位/堆/抽空/累计帧数/缓冲余量)
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now - last_log >= 10000) {
                last_log = now;
                // mf=3k/6k(64k 码流量化噪声主要落在这两段)、hf=10k/15k。
                // 对比录音回放(16k 单声道,WAV 无量化噪声)可区分"码流噪声/模拟通路"。
                ESP_LOGI(TAG, "播放中: ring=%u/%d heap=%u 抽空=%u 帧=%d used=%d mf=%u/%u hf=%u/%u",
                         (unsigned)xStreamBufferBytesAvailable(s_ring), PCM_RING_BYTES,
                         (unsigned)esp_get_free_heap_size(), (unsigned)s_underruns,
                         frames_total, used,
                         (unsigned)s_spec[4], (unsigned)s_spec[5],
                         (unsigned)s_spec[6], (unsigned)s_spec[7]);
            }
            unsigned char *p = in;
            int left = used;
            int round_frames = 0;
            while (left > 0 && !s_stop_play) {
                int off = MP3FindSyncWord(p, left);
                if (off < 0) {
                    // 同步字可能跨读块(0xFF 在块尾):保留末尾候选字节,其余丢弃
                    if (p[left - 1] == 0xFF) { p += left - 1; left = 1; }
                    else { p += left; left = 0; }
                    break;
                }
                p += off;
                left -= off;
                // 自行校验 MPEG1/2/2.5 Layer III 帧头并确认整帧已在缓冲内,
                // 再交给 Helix。这正是 useSize=1 应该做的边界校验,但本版
                // libhelix 的 useSize=1 路径会连正常流也判成 underflow
                // (实测 4 台全部"无有效 MP3 帧"),故改为外部校验 + useSize=0。
                int flen = 0, hsr = 0, hch = 0;
                int hv = mp3_hdr_check(p, left, &flen, &hsr, &hch);
                if (hv < 0) break; // 帧不完整:保留到下次读,避免按垃圾长度解析越界
                if (hv == 0) {
                    p++;
                    left--;
                    continue;
                }
                int rc = MP3Decode(dec, &p, &left, pcm, 0);
                if (rc == ERR_MP3_INDATA_UNDERFLOW) break;
                if (rc != ERR_MP3_NONE) {
                    p++;
                    left--;
                    continue;
                }
                MP3FrameInfo info;
                MP3GetLastFrameInfo(dec, &info);
                if (!format_set) {
                    // 帧头已由 mp3_hdr_check 校验过,这里用帧头自身的采样率/声道
                    // 投票(比 Helix FrameInfo 可靠);连续 3 帧一致才配 I2S。
                    if (hsr == sr_vote && hch == ch_vote) {
                        votes++;
                    } else {
                        sr_vote = hsr;
                        ch_vote = hch;
                        votes = 1;
                    }
                    if (votes < 3) continue; // 连续 3 帧一致才认格式
                    if (bsp_audio_set_format((uint32_t)sr_vote, 16, (uint8_t)ch_vote) != ESP_OK) {
                        ESP_LOGE(TAG, "采样格式设置失败 %d Hz/%d ch", sr_vote, ch_vote);
                        goto release_net;
                    }
                    spec_setup(sr_vote);
                    lp_setup(sr_vote);
                    format_set = true;
                    ESP_LOGI(TAG, "MP3 格式确认: %d Hz/%d ch", sr_vote, ch_vote);
                }
                if (info.outputSamps > 0 && info.outputSamps <= PCM_SAMPLES) {
                    // 去量化噪声:解码后、入 ring 前做两级低通(仅电台链路)。
                    // 按声道交错步进,避免每样点取模除法(C3 无硬件除法器)。
                    for (int i = 0; i < info.outputSamps; i += hch) {
                        pcm[i] = lp_proc(0, pcm[i]);
                        if (hch > 1) pcm[i + 1] = lp_proc(1, pcm[i + 1]);
                    }
                    round_frames++;
                    frames_total++;
                    // 频谱每 4 帧算 1 次:C3 无 FPU,Goertzel 浮点 MAC 全软件模拟,
                    // 每帧全算约占 18% CPU,叠加 Helix 解码会把帧速率压到 19~29 帧/秒
                    // (实时需 41.7),ring 被持续抽干——炸音真凶(帧计数日志实证)。
                    // 显示刷新仅 120ms,降频后视觉无感。
                    if ((frames_total & 3) == 0) {
                        spec_feed(pcm, info.outputSamps / info.nChans, info.nChans);
                    }
                    // 无损发送:ring 满时阻塞等写入任务排空,绝不丢弃已解码 PCM。
                    // 带超时的部分发送曾把剩余样本静默丢掉,表现为语音跳段(语速快)
                    // 与 ring 水位下跌(卡顿),实测复现。停播时退出循环即可。
                    size_t pcm_bytes = (size_t)info.outputSamps * sizeof(short);
                    size_t sent = 0;
                    while (sent < pcm_bytes && !s_stop_play) {
                        sent += xStreamBufferSend(s_ring, (const uint8_t *)pcm + sent,
                                                  pcm_bytes - sent, pdMS_TO_TICKS(500));
                    }
                    if (s_stop_play) break;
                    // 预缓冲:ring 填到一半(≈35ms 抖动余量,叠加 DMA ≈68ms)再启动
                    // 写入任务;格式已就绪才出声,且启动后水位有缓冲,抖动期不易抽空。
                    if (!writer_started && xStreamBufferBytesAvailable(s_ring) >= PCM_RING_BYTES / 2) {
                        if (xTaskCreate(pcm_writer_task, "radio_pcm", 4096, NULL, 6, NULL) != pdPASS) {
                            ESP_LOGE(TAG, "写入任务创建失败");
                            goto release_net;
                        }
                        writer_started = true;
                    }
                }
            }

            int remain = left;
            if (remain > 0) memmove(in, p, (size_t)remain);
            used = remain;
            if (used > IN_BUF_SIZE - 2048) {
                // 缓冲区里没有可同步的帧:丢掉前半段,避免死锁
                int drop = used / 2;
                memmove(in, in + drop, (size_t)(used - drop));
                used -= drop;
            }
            if (frames_total == 0 && bytes_total > 32 * 1024) {
                // 读入 32KB 仍解不出一帧:流不是 MP3(qtfm 部分台已切 AAC,
                // Helix 无法解码),友好停播而不是无声死等/触发看门狗。
                ESP_LOGW(TAG, "读入 %d 字节无有效 MP3 帧,流格式不支持(可能 AAC),停播", bytes_total);
                break;
            }
            if (round_frames == 0) {
                // 本轮未解出帧(格式不符/垃圾数据):让出 CPU,防忙等饿死 IDLE
                // 触发任务看门狗(实测复现:CPU 0: badge_radio 持续被点名)。
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            int space = IN_BUF_SIZE - used;
            int want = space < READ_CHUNK ? space : READ_CHUNK;
            uint32_t t0 = xTaskGetTickCount() * portTICK_PERIOD_MS;
            int n = esp_http_client_read(client, (char *)in + used, want);
            uint32_t dt = xTaskGetTickCount() * portTICK_PERIOD_MS - t0;
            if (dt > 200 && n > 0) {
                // 慢读证据:64kbps 下 1KB 应 ~128ms 返回;超 200ms 即网络/服务器停顿,
                // ring(62ms 容错)必被抽空。记录停顿模式定位周期性根因。
                ESP_LOGI(TAG, "慢读 %ums/%dB ring=%u", (unsigned)dt, n,
                         (unsigned)xStreamBufferBytesAvailable(s_ring));
            }
            if (n <= 0) {
                // 服务器瞬断/CDN 停顿(qtfm 实测偶发 1.5~2.7 秒):重试窗口放宽到
                // ~4 秒,扛过停顿即无缝续播,不会误判断流。
                for (int retry = 0; n <= 0 && retry < 5 && !s_stop_play; retry++) {
                    vTaskDelay(pdMS_TO_TICKS(800));
                    n = esp_http_client_read(client, (char *)in + used, want);
                }
                if (n <= 0) {
                    ESP_LOGI(TAG, "电台流结束");
                    break;
                }
            }
            used += n;
            bytes_total += n;
        }

    // 通知写入任务排空并等其退出,之后才能关 codec/释放解码器
    s_feed_done = true;
    if (writer_started) {
        xSemaphoreTake(s_writer_done, pdMS_TO_TICKS(5000));
    }

release_net:
    if (dec) MP3FreeDecoder(dec);
    if (client) esp_http_client_close(client);
    if (client) esp_http_client_cleanup(client);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // 恢复默认省电
    app_net_release("radio-play");
    if (portal_paused) app_portal_start();
done:
    // 栈余量诊断:低于 512 字节说明解码链路已贴近溢出(曾出现 Store access fault)
    ESP_LOGI(TAG, "播放任务退出,栈最小余量=%u 字节",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_playing = false;
    s_spec_sr = 0;
    memset(s_spec, 0, sizeof(s_spec));
    xSemaphoreGive(s_play_done);
    vTaskDelete(NULL);
}

esp_err_t app_radio_play(int index) {
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= s_count) return ESP_ERR_INVALID_ARG;
    if (s_playing) return ESP_ERR_INVALID_STATE;
    if (!app_net_sta_connected()) {
        ESP_LOGW(TAG, "Wi-Fi 未连接,无法播放");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_play_done, 0);
    s_stop_play = false;
    s_current = index;
    s_playing = true;
    // 栈 8KB:4KB 实测在"HTTP 收包 + Helix 解码"叠加时贴近溢出,
    // 起播阶段触发过 Store access fault(崩在 xmp3_UnpackSideInfo)。
    // 多出的 4KB 只在播放期间占用,播放期空闲堆仍有 23~37KB。
    if (xTaskCreate(play_task, "badge_radio", 8192, NULL, 6, NULL) != pdPASS) {
        s_playing = false;
        ESP_LOGE(TAG, "播放任务创建失败(堆不足)");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void app_radio_stop(void) {
    if (!s_playing) return;
    s_stop_play = true;
    // 本函数由输入任务(按键回调)与页面 stop 同步调用,等待必须限时:
    // 播放任务可能正阻塞在 HTTP 读(超时 15s),无上限等待会把输入/LVGL
    // 一起卡住,表现为"OK 无法暂停""双击调音量没反应"。任务退出时会自行
    // 清零 s_playing 并释放 s_play_done;超时就让它在后台收尾。
    if (xSemaphoreTake(s_play_done, pdMS_TO_TICKS(1200)) != pdTRUE) {
        ESP_LOGW(TAG, "停播超时:播放任务仍在收尾(1.2s)");
    }
}
