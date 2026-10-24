// tests/test_wav_file.c —— WAV 头生成与解析。
#include "wav_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,        \
                    __LINE__, #cond);                                     \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void test_roundtrip(void) {
    uint8_t header[WAV_HEADER_SIZE];
    wav_build_header(header, 16000, 1, 16, 32000);
    CHECK(memcmp(header, "RIFF", 4) == 0);
    CHECK(memcmp(header + 8, "WAVE", 4) == 0);

    wav_info_t info;
    CHECK(wav_parse_header(header, sizeof(header), &info));
    CHECK(info.sample_rate == 16000);
    CHECK(info.channels == 1);
    CHECK(info.bits == 16);
    CHECK(info.data_bytes == 32000);
    CHECK(info.data_offset == 44);
}

static void test_rejects(void) {
    uint8_t header[WAV_HEADER_SIZE];
    wav_build_header(header, 44100, 2, 16, 100);
    wav_info_t info;

    CHECK(!wav_parse_header(header, 8, &info));
    uint8_t bad[WAV_HEADER_SIZE];
    memcpy(bad, header, sizeof(bad));
    bad[0] = 'X';
    CHECK(!wav_parse_header(bad, sizeof(bad), &info));
    memcpy(bad, header, sizeof(bad));
    bad[20] = 3; // 非 PCM
    CHECK(!wav_parse_header(bad, sizeof(bad), &info));
    // data 长度超出缓冲区(截断文件)
    CHECK(!wav_parse_header(header, sizeof(header) - 1, &info));
}

static void test_extra_chunk(void) {
    // 在 fmt 后插入一个 LIST 块,解析仍应找到 data。
    uint8_t buf[WAV_HEADER_SIZE + 12];
    wav_build_header(buf, 8000, 1, 8, 4);
    memcpy(buf + 44, "LIST", 4);
    memset(buf + 48, 0, 8); // size=8 + 8 字节内容
    memcpy(buf + 56, "data", 4);
    buf[60] = 4;
    buf[61] = 0;
    buf[62] = 0;
    buf[63] = 0;
    wav_info_t info;
    CHECK(wav_parse_header(buf, sizeof(buf), &info));
    CHECK(info.data_offset == 56);
    CHECK(info.data_bytes == 4);
    CHECK(info.sample_rate == 8000);
}

int main(void) {
    test_roundtrip();
    test_rejects();
    test_extra_chunk();
    puts("test_wav_file: PASS");
    return 0;
}
