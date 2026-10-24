// main/wav_file.c
#include "wav_file.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void wav_build_header(uint8_t out[WAV_HEADER_SIZE], uint32_t sample_rate,
                      uint16_t channels, uint16_t bits, uint32_t data_bytes) {
    memset(out, 0, WAV_HEADER_SIZE);
    memcpy(out, "RIFF", 4);
    put_u32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVE", 4);
    memcpy(out + 12, "fmt ", 4);
    put_u32(out + 16, 16);
    put_u16(out + 20, 1); // PCM
    put_u16(out + 22, channels);
    put_u32(out + 24, sample_rate);
    uint32_t block_align = (uint32_t)channels * (bits / 8);
    put_u32(out + 28, sample_rate * block_align);
    put_u16(out + 32, (uint16_t)block_align);
    put_u16(out + 34, bits);
    memcpy(out + 36, "data", 4);
    put_u32(out + 40, data_bytes);
}

bool wav_parse_header(const uint8_t *buf, size_t len, wav_info_t *out) {
    if (!buf || !out || len < 12) return false;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return false;

    bool have_fmt = false;
    bool have_data = false;
    wav_info_t info = { 0 };

    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t size = get_u32(buf + pos + 4);
        size_t body = pos + 8;
        const bool is_data = (memcmp(id, "data", 4) == 0);
        // data 块允许只有块头在 buf 内(播放侧只读 44 字节头,数据流式读取);
        // 其余块(含 fmt)必须完整,否则视为截断。
        if (!is_data && body + size > len) return false;
        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16) return false;
            uint16_t format = get_u16(buf + body);
            if (format != 1) return false; // 只支持未压缩 PCM
            info.channels = get_u16(buf + body + 2);
            info.sample_rate = get_u32(buf + body + 4);
            info.bits = get_u16(buf + body + 14);
            if (info.channels == 0 || info.sample_rate == 0 ||
                (info.bits != 8 && info.bits != 16 && info.bits != 24 && info.bits != 32)) {
                return false;
            }
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            info.data_offset = (uint32_t)body;
            info.data_bytes = size;
            have_data = true;
        }
        if (have_fmt && have_data) break;
        pos = body + size + (size & 1u); // RIFF 块按 2 字节对齐
    }

    if (!have_fmt || !have_data) return false;
    *out = info;
    return true;
}
