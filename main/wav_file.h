// main/wav_file.h —— 16bit PCM WAV 头生成/解析(纯逻辑,可主机测试)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WAV_HEADER_SIZE 44

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_bytes;
    uint32_t data_offset;
} wav_info_t;

// 生成 44 字节 RIFF/WAVE/PCM 头。
void wav_build_header(uint8_t out[WAV_HEADER_SIZE], uint32_t sample_rate,
                      uint16_t channels, uint16_t bits, uint32_t data_bytes);

// 解析头;只接受 PCM,要求 fmt 与 data 都在 buf 内。
bool wav_parse_header(const uint8_t *buf, size_t len, wav_info_t *out);
