// components/bsp/src/bsp_audio.c
// 移植自 trae_card/components/platform/platform_esp32/src/audio_es8311.c
#include "bsp_audio.h"
#include "bsp_es8311_sleep_check.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_audio";

static esp_codec_dev_handle_t s_dev;
static i2s_chan_handle_t      s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t      *s_codec;
static const audio_codec_gpio_if_t *s_gpio;
// 记录当前已打开的格式,用于判断"要不要 close 重开"(见头文件里的坑说明)。
static uint32_t s_hz;
static uint8_t  s_bits, s_ch;
static bool     s_opened;
static bool     s_sleeping;
static bool     s_initialized;
static esp_err_t s_sleep_result;
static bool     s_codec_release_failed;
// 软件音量(Q15,32768 = 满幅)。实测 ES8311 的 DAC 音量寄存器(REG32)在 BOTH
// 模式下写入后听感无变化(音量 100→30 声音大小不变),故音量改由 PCM 缩放实现:
// 硬件 DAC 固定满幅,用户音量只作用于写入 I2S 的采样,任何 codec 配置下都生效。
static volatile int32_t s_sw_gain = 32768;
static int      s_io_error;
// bsp_audio 公共 API 互斥锁:set_format 会 delete/create codec 设备(s_dev 短暂
// 为 NULL),另一任务此时调 set_volume/write 即空指针 Load access fault
// (电台播放启动与按键音并发实测复现崩溃)。公共入口统一拿锁,内部调用的
// 真实实现为 *_locked 静态函数,避免递归锁。
static SemaphoreHandle_t s_api_lock;

// esp_codec_dev 1.6.2 discards some control/data interface errors. Remember the
// first one at the public interface boundary; never inspect its private state.
static int audio_track_error(int result) {
    if (result != ESP_CODEC_DEV_OK && s_io_error == ESP_CODEC_DEV_OK) {
        s_io_error = result;
    }
    return result;
}

static bool audio_ctrl_is_open(const audio_codec_ctrl_if_t *ctrl) {
    (void)ctrl;
    return s_ctrl && s_ctrl->is_open(s_ctrl);
}

static int audio_ctrl_read(const audio_codec_ctrl_if_t *ctrl, int reg,
                           int reg_len, void *data, int data_len) {
    (void)ctrl;
    return audio_track_error(s_ctrl->read_reg(s_ctrl, reg, reg_len, data, data_len));
}

static int audio_ctrl_write(const audio_codec_ctrl_if_t *ctrl, int reg,
                            int reg_len, void *data, int data_len) {
    (void)ctrl;
    return audio_track_error(s_ctrl->write_reg(s_ctrl, reg, reg_len, data, data_len));
}

static int audio_ctrl_info(const audio_codec_ctrl_if_t *ctrl,
                           audio_codec_ctrl_info_t *info) {
    (void)ctrl;
    return audio_track_error(s_ctrl->get_info(s_ctrl, info));
}

static const audio_codec_ctrl_if_t s_checked_ctrl = {
    .is_open = audio_ctrl_is_open, .read_reg = audio_ctrl_read,
    .write_reg = audio_ctrl_write, .get_info = audio_ctrl_info,
};

static bool audio_data_is_open(const audio_codec_data_if_t *data) {
    (void)data;
    return s_data && s_data->is_open(s_data);
}

static int audio_data_enable(const audio_codec_data_if_t *data,
                             esp_codec_dev_type_t type, bool enable) {
    (void)data;
    // IN_OUT overwrites a failed TX result with the RX result in 1.6.2.
    // Enable TX first (master clock), disable RX first (no deferred TX stop).
    esp_codec_dev_type_t first = enable ? ESP_CODEC_DEV_TYPE_OUT : ESP_CODEC_DEV_TYPE_IN;
    esp_codec_dev_type_t last = enable ? ESP_CODEC_DEV_TYPE_IN : ESP_CODEC_DEV_TYPE_OUT;
    int result = ESP_CODEC_DEV_OK;
    if (type & first) result = audio_track_error(s_data->enable(s_data, first, enable));
    if (type & last) {
        int next = audio_track_error(s_data->enable(s_data, last, enable));
        if (result == ESP_CODEC_DEV_OK) result = next;
    }
    return result;
}

static int audio_data_set_fmt(const audio_codec_data_if_t *data,
                              esp_codec_dev_type_t type,
                              esp_codec_dev_sample_info_t *fs) {
    (void)data;
    if (type != ESP_CODEC_DEV_TYPE_IN_OUT) return ESP_CODEC_DEV_NOT_SUPPORT;
    // Configure directions separately so a failed TX configuration cannot be
    // hidden by a successful RX configuration. Keep TX enabled while configuring
    // RX: the dependency otherwise reconfigures/restarts TX a second time.
    int result = audio_track_error(s_data->set_fmt(s_data, ESP_CODEC_DEV_TYPE_OUT, fs));
    if (result != ESP_CODEC_DEV_OK) return result;
    result = audio_track_error(s_data->enable(s_data, ESP_CODEC_DEV_TYPE_OUT, true));
    if (result == ESP_CODEC_DEV_OK) {
        result = audio_track_error(s_data->set_fmt(s_data, ESP_CODEC_DEV_TYPE_IN, fs));
    }
    int stopped = audio_track_error(s_data->enable(s_data, ESP_CODEC_DEV_TYPE_OUT, false));
    return result == ESP_CODEC_DEV_OK ? stopped : result;
}

static int audio_data_read(const audio_codec_data_if_t *data, uint8_t *pcm, int size) {
    (void)data;
    return s_data->read(s_data, pcm, size);
}

static int audio_data_write(const audio_codec_data_if_t *data, uint8_t *pcm, int size) {
    (void)data;
    return s_data->write(s_data, pcm, size);
}

static const audio_codec_data_if_t s_checked_data = {
    .is_open = audio_data_is_open, .enable = audio_data_enable,
    .set_fmt = audio_data_set_fmt, .read = audio_data_read, .write = audio_data_write,
};

#define AUDIO_DEFAULT_HZ   16000
#define AUDIO_DEFAULT_BITS 16
#define AUDIO_DEFAULT_CH   1
#define ES8311_SLEEP_ATTEMPTS 2
#define ES8311_SLEEP_RETRY_MS 5

typedef struct {
    uint8_t reg;
    uint8_t value;
} es8311_reg_value_t;

// 寄存器序列不依赖 esp_codec_dev 的 opened 标志。REG45=0x01 额外关闭
// BCLK/LRCK 内部上拉，比当前 esp_codec_dev 1.6.2 的默认 suspend 更彻底。
static const es8311_reg_value_t s_es8311_sleep_sequence[] = {
    {0x32, 0x00}, {0x17, 0x00}, {0x0E, 0xFF}, {0x12, 0x02},
    {0x14, 0x00}, {0x0D, 0xFA}, {0x15, 0x00}, {0x02, 0x10},
    {0x00, 0x00}, {0x00, 0x1F}, {0x01, 0x30}, {0x01, 0x00},
    {0x45, 0x01}, {0x0D, 0xFC}, {0x02, 0x00},
};

static esp_err_t audio_disable_i2s_channels(void) {
    esp_err_t first_error = ESP_OK;
    const struct {
        i2s_chan_handle_t channel;
        const char *name;
    } channels[] = {
        {s_tx, "TX"},
        {s_rx, "RX"},
    };

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        if (!channels[i].channel) continue;
        esp_err_t e = i2s_channel_disable(channels[i].channel);
        if (e == ESP_ERR_INVALID_STATE) e = ESP_OK; // READY 即已停止。
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "I2S %s 停止失败: %s", channels[i].name,
                     esp_err_to_name(e));
            if (first_error == ESP_OK) first_error = e;
        }
    }
    return first_error;
}

static esp_err_t es8311_force_sleep_once(unsigned attempt) {
    bool valid = true;

    for (size_t i = 0; i < sizeof(s_es8311_sleep_sequence) /
                           sizeof(s_es8311_sleep_sequence[0]); i++) {
        const es8311_reg_value_t *item = &s_es8311_sleep_sequence[i];
        uint8_t value = item->value;
        int write_result = s_ctrl->write_reg(s_ctrl, item->reg, 1, &value, 1);
        if (write_result == ESP_CODEC_DEV_OK) continue;

        uint8_t actual = 0;
        int read_result = s_ctrl->read_reg(s_ctrl, item->reg, 1, &actual, 1);
        if (read_result == ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 休眠写入失败 attempt=%u REG%02X "
                          "expected=0x%02X actual=0x%02X error=%d",
                     attempt, item->reg, item->value, actual, write_result);
        } else {
            ESP_LOGE(TAG, "ES8311 休眠写入失败 attempt=%u REG%02X "
                          "expected=0x%02X actual=unavailable error=%d read_error=%d",
                     attempt, item->reg, item->value, write_result, read_result);
        }
        valid = false;
    }

    for (size_t i = 0; i < bsp_es8311_sleep_check_count; i++) {
        const bsp_es8311_reg_check_t *item = &bsp_es8311_sleep_checks[i];
        uint8_t actual = 0;
        int read_result = s_ctrl->read_reg(s_ctrl, item->reg, 1, &actual, 1);
        if (read_result == ESP_CODEC_DEV_OK &&
            bsp_es8311_sleep_check_matches(item, actual)) continue;

        ESP_LOGE(TAG, "ES8311 休眠校验失败 attempt=%u REG%02X "
                      "expected=0x%02X mask=0x%02X actual=0x%02X error=%d",
                 attempt, item->reg, item->value, item->mask, actual, read_result);
        valid = false;
    }
    return valid ? ESP_OK : ESP_FAIL;
}

static esp_err_t es8311_force_sleep(void) {
    if (!s_ctrl || !s_ctrl->read_reg || !s_ctrl->write_reg) {
        return ESP_ERR_INVALID_STATE;
    }

    for (unsigned attempt = 1; attempt <= ES8311_SLEEP_ATTEMPTS; attempt++) {
        esp_err_t e = es8311_force_sleep_once(attempt);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "ES8311 已进入低功耗状态并通过寄存器校验");
            return ESP_OK;
        }
        if (attempt < ES8311_SLEEP_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(ES8311_SLEEP_RETRY_MS));
        }
    }

    ESP_LOGE(TAG, "ES8311 强制休眠失败");
    return ESP_FAIL;
}

// esp_codec_dev_open() 会先 disable 再重配 I2S；close 后通道处于 READY，
// 先 enable 一次可让下一次 open 的内部 disable 合法。
static esp_err_t audio_prepare_i2s_reopen(void) {
    esp_err_t e = s_tx ? i2s_channel_enable(s_tx) : ESP_ERR_INVALID_STATE;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX 恢复失败: %s", esp_err_to_name(e));
        return e;
    }
    e = s_rx ? i2s_channel_enable(s_rx) : ESP_ERR_INVALID_STATE;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX 恢复失败: %s", esp_err_to_name(e));
        i2s_channel_disable(s_tx);
    }
    return e;
}

// esp_codec_dev 不拥有传入的接口和 I2S channel；失败回滚必须按依赖逆序逐一释放。
static void audio_delete_codec(void) {
    if (s_dev) {
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    if (s_codec) {
        // The dependency frees the interface even when close() fails (for
        // example, a reference-manager lock timeout). Its shared reference may
        // then survive: rebuilding could silently skip hardware initialization.
        // Such an unrecoverable release error needs a reboot, never a false OK.
        int result = audio_codec_delete_codec_if(s_codec);
        if (result != ESP_CODEC_DEV_OK) {
            s_codec_release_failed = true;
            audio_track_error(result);
            ESP_LOGE(TAG, "ES8311 codec 释放失败，需重启恢复: %d", result);
        }
        s_codec = NULL;
    }
    s_opened = false;
}

static void audio_cleanup(void) {
    audio_delete_codec();
    if (s_gpio) {
        audio_codec_delete_gpio_if(s_gpio);
        s_gpio = NULL;
    }
    if (s_data) {
        audio_codec_delete_data_if(s_data);
        s_data = NULL;
    }
    if (s_rx) {
        i2s_channel_disable(s_rx);
        esp_err_t e = i2s_del_channel(s_rx);
        if (e == ESP_OK) s_rx = NULL;
        else ESP_LOGE(TAG, "I2S RX 回滚失败: %s", esp_err_to_name(e));
    }
    if (s_tx) {
        i2s_channel_disable(s_tx);
        esp_err_t e = i2s_del_channel(s_tx);
        if (e == ESP_OK) s_tx = NULL;
        else ESP_LOGE(TAG, "I2S TX 回滚失败: %s", esp_err_to_name(e));
    }
    if (s_ctrl) {
        audio_codec_delete_ctrl_if(s_ctrl);
        s_ctrl = NULL;
    }
    s_opened = false;
    s_sleeping = false;
    s_initialized = false;
    s_sleep_result = ESP_OK;
    s_hz = 0;
    s_bits = 0;
    s_ch = 0;
}

static esp_err_t i2s_full_duplex_init(void) {
    i2s_chan_config_t chan = {
        .id = BSP_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        // DMA 缓冲保持 6x240。不要调大:reconfig_std_slot 在单/立体声切换时按
        // desc_num*frame_size 一次性连续分配 DMA 内存,调大后(实测 12x480 需
        // 连续 23KB)在堆碎片化时分配失败并引发 I2S 半配置 panic。网络电台的
        // 抗抖动由 app_radio 的静态 PCM 环形缓冲承担,不在这里解决。
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t e = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(e)); return e; }

    // 这里的采样率只用于建通道;实际速率由 esp_codec_dev_open() 按需重配。
    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_BCLK, .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if ((e = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx 初始化失败: %s", esp_err_to_name(e)); return e;
    }
    if ((e = i2s_channel_init_std_mode(s_rx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx 初始化失败: %s", esp_err_to_name(e)); return e;
    }
    // esp_codec_dev_open 内部重配前会先 i2s_channel_disable,而 disable 要求通道处于
    // RUNNING;刚 init 的通道是 READY,会打一条 "channel has not been enabled yet" 错误日志。
    // 这里先 enable 一次让那次 disable 合法(此时 codec 未配,不出声)。
    e = i2s_channel_enable(s_tx);
    if (e == ESP_OK) e = i2s_channel_enable(s_rx);
    if (e != ESP_OK) ESP_LOGE(TAG, "i2s channel enable 失败: %s", esp_err_to_name(e));
    return e;
}

static esp_err_t audio_create_codec(void) {
    if (s_codec_release_failed) return ESP_ERR_INVALID_STATE;
    s_io_error = ESP_CODEC_DEV_OK;
    s_codec = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if = &s_checked_ctrl,
        .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_I2S_PA_CTRL,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
        .no_dac_ref = true, // Mono capture must use ADC, not the DAC reference.
    });
    if (!s_codec || s_io_error != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "es8311_codec_new 失败: %d", s_io_error);
        audio_delete_codec();
        return ESP_FAIL;
    }
    s_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec,
        .data_if = &s_checked_data,
    });
    if (!s_dev) {
        audio_delete_codec();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_init(void) {
    if (!s_api_lock) s_api_lock = xSemaphoreCreateMutex();
    if (!s_api_lock) return ESP_ERR_NO_MEM;
    if (s_codec_release_failed) return ESP_ERR_INVALID_STATE;
    if (s_initialized) return ESP_OK;
    if (s_tx || s_rx || s_ctrl || s_data || s_codec || s_gpio) {
        ESP_LOGE(TAG, "上次音频初始化回滚不完整，拒绝覆盖仍存活的资源句柄");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    s_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = BSP_I2C_PORT,
        .addr = BSP_I2C_ES8311_ADDR << 1,   // 该接口要 8 位地址形式
        .bus_handle = bsp_i2c_bus(),
    });
    if (!s_ctrl) {
        ESP_LOGE(TAG, "ES8311 控制口创建失败 —— 用 bsp_i2c_scan() 确认 0x%02X 是否应答;"
                      "检查 SDA=GPIO%d / SCL=GPIO%d 接线与 codec 供电",
                 BSP_I2C_ES8311_ADDR, BSP_I2C_SDA, BSP_I2C_SCL);
        return ESP_FAIL;
    }

    if ((e = i2s_full_duplex_init()) != ESP_OK) goto fail;

    s_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = BSP_I2S_PORT, .tx_handle = s_tx, .rx_handle = s_rx,
    });
    if (!s_data) { ESP_LOGE(TAG, "I2S 数据口创建失败"); e = ESP_ERR_NO_MEM; goto fail; }

    s_gpio = audio_codec_new_gpio();
    if (!s_gpio) { ESP_LOGE(TAG, "codec GPIO 接口创建失败"); e = ESP_ERR_NO_MEM; goto fail; }

    if ((e = audio_create_codec()) != ESP_OK) goto fail;

    s_initialized = true;
    ESP_LOGI(TAG, "ES8311 就绪");
    return ESP_OK;

fail:
    audio_delete_codec();
    if (s_ctrl) (void)es8311_force_sleep();
    audio_cleanup();
    return e;
}

static esp_err_t audio_set_format_locked(uint32_t hz, uint8_t bits, uint8_t ch) {
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_sleeping) return ESP_ERR_INVALID_STATE;
    if (s_opened && s_hz == hz && s_bits == bits && s_ch == ch) return ESP_OK;   // 同格式复用

    esp_err_t e = ESP_FAIL;
    s_io_error = ESP_CODEC_DEV_OK;
    if (s_opened) audio_delete_codec();
    if (s_io_error != ESP_CODEC_DEV_OK) goto fail;
    // Always normalize the channels, including after a failed partial open.
    e = audio_disable_i2s_channels();
    if (e != ESP_OK) goto fail;
    e = audio_prepare_i2s_reopen();
    if (e != ESP_OK) goto fail;
    if (!s_dev && (e = audio_create_codec()) != ESP_OK) goto fail;
    s_io_error = ESP_CODEC_DEV_OK;

    // channel_mask 必须与实际声道数匹配:esp_codec_dev 的 I2S 接口按掩码位数
    // 决定 slot_mode(1 位=MONO)。立体声只给声道 0 会把 I2S 配成单声道,
    // 交织的 LRLR 数据被逐样本消费,表现为 2 倍速变调(电台实测复现)。
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = ch,
        .channel_mask = (ch >= 2)
            ? (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
            : ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = hz,
        .mclk_multiple = 0,          // 0 → 驱动按默认 256xfs 取 MCLK
    };
    int r = esp_codec_dev_open(s_dev, &fs);
    if (r != ESP_CODEC_DEV_OK || s_io_error != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %d interface=%d", r, s_io_error);
        e = ESP_FAIL;
        goto fail;
    }

    // ⚠ open 之后【不要】手动覆写 ES8311 的时钟分频寄存器(REG01~06):
    //   驱动已按采样率与 MCLK 精确算好,覆写会导致 ADC/DAC 时序错乱、录音回放全是杂音。
    //   这里只设麦克风模拟 PGA 增益。
    r = esp_codec_dev_set_in_gain(s_dev, 30.0f);
    // 硬件 DAC 固定满幅:用户音量由 bsp_audio_write 的软件缩放控制(见 s_sw_gain)。
    if (r == ESP_CODEC_DEV_OK) r = esp_codec_dev_set_out_vol(s_dev, 100);
    if (r != ESP_CODEC_DEV_OK || s_io_error != ESP_CODEC_DEV_OK) {
        e = ESP_FAIL;
        goto fail;
    }

    s_opened = true; s_hz = hz; s_bits = bits; s_ch = ch;
    ESP_LOGI(TAG, "codec 打开 %luHz/%ubit/%uch", (unsigned long)hz, bits, ch);
    return ESP_OK;

fail:
    // A failed open can already have set codec-dev's opened flags and the
    // ES8311 enabled/reference state. Recreate through public APIs next time.
    audio_delete_codec();
    (void)es8311_force_sleep();
    (void)audio_disable_i2s_channels();
    return e == ESP_OK ? ESP_FAIL : e;
}

esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch) {
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    esp_err_t r = audio_set_format_locked(hz, bits, ch);
    xSemaphoreGive(s_api_lock);
    return r;
}

static esp_err_t audio_sleep_locked(void) {
    if (!s_initialized) return ESP_OK;
    if (s_sleeping) return s_sleep_result;

    esp_err_t first_error = ESP_OK;
    // Drop stale enabled/opened state, even if suspend fails. Deletion may write
    // the dependency's weaker suspend sequence, so force-sleep MUST run last.
    // Only codec objects are released: I2C bus and I2S channels remain owned.
    s_io_error = ESP_CODEC_DEV_OK;
    audio_delete_codec();
    if (s_io_error != ESP_CODEC_DEV_OK) first_error = ESP_FAIL;

    esp_err_t e = es8311_force_sleep();
    if (e != ESP_OK && first_error == ESP_OK) first_error = e;

    e = audio_disable_i2s_channels();
    if (e != ESP_OK && first_error == ESP_OK) first_error = e;
    s_sleeping = true;
    s_sleep_result = first_error;
    return first_error;
}

esp_err_t bsp_audio_sleep(void) {
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    esp_err_t r = audio_sleep_locked();
    xSemaphoreGive(s_api_lock);
    return r;
}

static esp_err_t audio_prepare_deep_sleep_locked(void) {
    esp_err_t first_error = audio_disable_i2s_channels();
    const int pins[] = {
        BSP_I2S_MCLK, BSP_I2S_BCLK, BSP_I2S_WS, BSP_I2S_DOUT, BSP_I2S_DIN,
    };
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] >= 0) mask |= 1ULL << (unsigned)pins[i];
    }

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 引脚高阻配置失败: %s", esp_err_to_name(e));
        if (first_error == ESP_OK) first_error = e;
    } else {
        ESP_LOGI(TAG, "I2S MCLK/BCLK/WS/DOUT/DIN 已切换为高阻");
    }
    return first_error;
}

esp_err_t bsp_audio_prepare_deep_sleep(void) {
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    esp_err_t r = audio_prepare_deep_sleep_locked();
    xSemaphoreGive(s_api_lock);
    return r;
}

static esp_err_t audio_wake_locked(void) {
    if (!s_initialized || !s_sleeping) return ESP_OK;

    // 允许内部格式设置重新 open codec；失败时再次 close，避免留下半唤醒状态。
    s_sleeping = false;
    esp_err_t e = audio_set_format_locked(s_hz ? s_hz : AUDIO_DEFAULT_HZ,
                             s_bits ? s_bits : AUDIO_DEFAULT_BITS,
                             s_ch ? s_ch : AUDIO_DEFAULT_CH);
    if (e != ESP_OK) {
        s_opened = false;
        s_sleeping = true;
        s_sleep_result = e;
        ESP_LOGE(TAG, "ES8311 唤醒失败: %s", esp_err_to_name(e));
        return e;
    }

    s_sleep_result = ESP_OK;
    ESP_LOGI(TAG, "ES8311 已从低功耗状态恢复");
    return ESP_OK;
}

esp_err_t bsp_audio_wake(void) {
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    esp_err_t r = audio_wake_locked();
    xSemaphoreGive(s_api_lock);
    return r;
}

// 软件音量分块长度(字节)。播放调用方多为 1~2KB,分块后静态缓冲只需 1KB。
// 16bit 采样逐点乘 Q15 增益右移 15 位;C3 无 FPU,整数乘加足够(48kHz 立体声
// 约 9.6 万次/秒,占用可忽略)。
#define SW_VOL_CHUNK_BYTES 1024
static int16_t s_sw_buf[SW_VOL_CHUNK_BYTES / 2];

static esp_err_t audio_write_scaled(const void *pcm, size_t bytes) {
    const int32_t g = s_sw_gain;
    if (g >= 32768) { // 满音量:零拷贝
        return esp_codec_dev_write(s_dev, (void *)pcm, (int)bytes) == 0 ? ESP_OK : ESP_FAIL;
    }
    const uint8_t *src = (const uint8_t *)pcm;
    while (bytes >= 2) {
        size_t n = bytes < SW_VOL_CHUNK_BYTES ? bytes : SW_VOL_CHUNK_BYTES;
        n &= ~(size_t)1; // 保证 16bit 对齐,避免拆半个采样
        if (n == 0) break;
        const int16_t *s = (const int16_t *)src;
        const int cnt = (int)(n / 2);
        for (int i = 0; i < cnt; i++) {
            s_sw_buf[i] = (int16_t)(((int32_t)s[i] * g) >> 15);
        }
        if (esp_codec_dev_write(s_dev, s_sw_buf, (int)n) != 0) return ESP_FAIL;
        src += n;
        bytes -= n;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_write(const void *pcm, size_t bytes) {
    // 限时取锁:I2S 若因 DMA 停摆而阻塞在 esp_codec_dev_write 里,持锁任务会
    // 永久占锁,任何 portMAX_DELAY 的调用者(音量设置/按键路径)会被一起卡死,
    // 表现为"音量条不显示、OK 无法暂停"。宁可丢这一笔 PCM,也不锁死全系统。
    if (xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r;
    if (!s_dev || !s_opened || s_sleeping) {
        r = ESP_ERR_INVALID_STATE;
    } else {
        r = audio_write_scaled(pcm, bytes);
    }
    xSemaphoreGive(s_api_lock);
    return r;
}

esp_err_t bsp_audio_read(void *pcm, size_t bytes) {
    if (xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(500)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t r;
    if (!s_dev || !s_opened || s_sleeping) {
        r = ESP_ERR_INVALID_STATE;
    } else {
        r = esp_codec_dev_read(s_dev, pcm, bytes) == 0 ? ESP_OK : ESP_FAIL;
    }
    xSemaphoreGive(s_api_lock);
    return r;
}

void bsp_audio_set_volume(uint8_t percent) {
    if (percent > 100) percent = 100;
    // 平方律 taper:50% → -12dB,30% → -21dB;100% 满幅(增益 1.0)。
    int32_t g = ((int32_t)percent * percent * 32768 + 5000) / 10000;
    if (g > 32768) g = 32768;
    s_sw_gain = g;
    ESP_LOGI(TAG, "音量 %u%% (软件增益 %ld/32768)", (unsigned)percent, (long)g);
}
