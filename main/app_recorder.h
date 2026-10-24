// main/app_recorder.h —— 录音(FAT WAV)与播放,后台任务。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define APP_REC_NAME_LEN 24

esp_err_t app_recorder_init(void);

bool app_recorder_recording(void);
bool app_recorder_playing(void);

esp_err_t app_recorder_start(void);        // 开始录音(自动命名 recNNN.wav)
esp_err_t app_recorder_stop(void);         // 停止录音(补写头)或停止播放,等待任务退出
esp_err_t app_recorder_play(const char *path);

// 删除一个录音文件(只接受目录内的文件名,不含路径)。录音/播放进行中返回 INVALID_STATE。
esp_err_t app_recorder_delete(const char *name);

// 已录时长(秒),仅录音时有意义。
uint32_t app_recorder_seconds(void);

// 列出录音文件名(不含目录),新的在前。返回条数。
int app_recorder_list(char (*names)[APP_REC_NAME_LEN], int max);
