// main/app_recorder.c
#include "app_recorder.h"

#include "app_config.h"
#include "app_store.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wav_file.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_recorder";
#define REC_RATE 16000
#define REC_SECONDS_MAX 60
#define REC_DIR APP_STORE_BASE "/rec"
#define CHUNK_SAMPLES 320 // 20ms

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;
static volatile bool s_recording;
static volatile bool s_playing;
static volatile bool s_stop;
static volatile uint32_t s_frames;

esp_err_t app_recorder_init(void) {
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_done) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool app_recorder_recording(void) {
    return s_recording;
}

bool app_recorder_playing(void) {
    return s_playing;
}

uint32_t app_recorder_seconds(void) {
    return s_frames / REC_RATE;
}

static void next_path(char *out, size_t size) {
    for (int i = 0; i < 1000; i++) {
        snprintf(out, size, REC_DIR "/rec%03d.wav", i);
        FILE *f = fopen(out, "rb");
        if (!f) return;
        fclose(f);
    }
    snprintf(out, size, REC_DIR "/rec999.wav");
}

static void record_task(void *arg) {
    (void)arg;
    char path[64];
    next_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "无法创建 %s", path);
        s_recording = false;
        xSemaphoreGive(s_done);
        vTaskDelete(NULL);
        return;
    }

    uint8_t zero[WAV_HEADER_SIZE];
    memset(zero, 0, sizeof(zero));
    fwrite(zero, 1, sizeof(zero), f);

    s_frames = 0;
    if (bsp_audio_set_format(REC_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "录音格式设置失败");
        fclose(f);
        remove(path);
        s_recording = false;
        xSemaphoreGive(s_done);
        vTaskDelete(NULL);
        return;
    }

    int16_t *buf = malloc(CHUNK_SAMPLES * sizeof(int16_t));
    if (buf) {
        while (!s_stop && s_frames < (uint32_t)REC_RATE * REC_SECONDS_MAX) {
            if (bsp_audio_read(buf, CHUNK_SAMPLES * sizeof(int16_t)) != ESP_OK) break;
            if (fwrite(buf, sizeof(int16_t), CHUNK_SAMPLES, f) != CHUNK_SAMPLES) break;
            s_frames += CHUNK_SAMPLES;
        }
        free(buf);
    }

    uint32_t data_bytes = s_frames * (uint32_t)sizeof(int16_t);
    uint8_t header[WAV_HEADER_SIZE];
    wav_build_header(header, REC_RATE, 1, 16, data_bytes);
    fseek(f, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), f);
    fclose(f);
    ESP_LOGI(TAG, "录音完成 %s (%u 字节)", path, (unsigned)data_bytes);

    s_recording = false;
    s_frames = 0;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static void play_task(void *arg) {
    char *path = (char *)arg;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "无法打开 %s", path);
        goto done;
    }
    uint8_t header[WAV_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) goto close_file;
    wav_info_t info;
    if (!wav_parse_header(header, sizeof(header), &info)) {
        ESP_LOGE(TAG, "WAV 头无效");
        goto close_file;
    }
    if (info.bits != 16) {
        ESP_LOGE(TAG, "仅支持 16bit WAV");
        goto close_file;
    }
    if (bsp_audio_set_format(info.sample_rate, 16, (uint8_t)info.channels) != ESP_OK) {
        ESP_LOGE(TAG, "播放格式设置失败");
        goto close_file;
    }
    bsp_audio_set_volume(app_config()->volume);
    fseek(f, (long)info.data_offset, SEEK_SET);

    uint8_t buf[1024];
    uint32_t left = info.data_bytes;
    while (!s_stop && left > 0) {
        size_t want = left < sizeof(buf) ? left : sizeof(buf);
        size_t n = fread(buf, 1, want, f);
        if (n == 0) break;
        if (bsp_audio_write(buf, n) != ESP_OK) break;
        left -= (uint32_t)n;
    }

close_file:
    fclose(f);
done:
    free(path);
    s_playing = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t app_recorder_start(void) {
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (!app_store_ready()) return ESP_ERR_INVALID_STATE;
    if (s_recording || s_playing) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_done, 0);
    s_stop = false;
    s_frames = 0;
    s_recording = true;
    if (xTaskCreate(record_task, "badge_rec", 4096, NULL, 5, NULL) != pdPASS) {
        s_recording = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_recorder_play(const char *path) {
    if (!s_lock || !path) return ESP_ERR_INVALID_ARG;
    if (s_recording || s_playing) return ESP_ERR_INVALID_STATE;
    char *copy = strdup(path);
    if (!copy) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_done, 0);
    s_stop = false;
    s_playing = true;
    if (xTaskCreate(play_task, "badge_play", 4096, copy, 5, NULL) != pdPASS) {
        free(copy);
        s_playing = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_recorder_stop(void) {
    if (s_recording || s_playing) {
        s_stop = true;
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(4000)) != pdTRUE) {
            ESP_LOGW(TAG, "音频任务未按时退出");
            s_recording = false;
            s_playing = false;
        }
    }
    return ESP_OK;
}

esp_err_t app_recorder_delete(const char *name) {
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (s_recording || s_playing) return ESP_ERR_INVALID_STATE;
    // 只接受本目录内的纯文件名,防止拼出目录穿越路径。
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(name);
    if (len < 8 || len >= APP_REC_NAME_LEN) return ESP_ERR_INVALID_ARG;
    if (strncmp(name, "rec", 3) != 0 || strcmp(name + len - 4, ".wav") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/%s", name);
    if (remove(path) != 0) {
        ESP_LOGW(TAG, "删除失败 %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "已删除 %s", path);
    return ESP_OK;
}

int app_recorder_list(char (*names)[APP_REC_NAME_LEN], int max) {
    if (!names || max <= 0) return 0;
    DIR *dir = opendir(REC_DIR);
    if (!dir) return 0;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        size_t len = strlen(ent->d_name);
        if (len < 8 || len >= APP_REC_NAME_LEN) continue;
        if (strncmp(ent->d_name, "rec", 3) != 0) continue;
        if (strcmp(ent->d_name + len - 4, ".wav") != 0) continue;
        memcpy(names[count], ent->d_name, len + 1);
        count++;
    }
    closedir(dir);
    // 文件名按序号递增,倒序即最新在前。
    for (int i = 1; i < count; i++) {
        char key[APP_REC_NAME_LEN];
        memcpy(key, names[i], APP_REC_NAME_LEN);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) < 0) {
            memcpy(names[j + 1], names[j], APP_REC_NAME_LEN);
            j--;
        }
        memcpy(names[j + 1], key, APP_REC_NAME_LEN);
    }
    return count;
}
