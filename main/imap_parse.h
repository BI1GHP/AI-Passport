// main/imap_parse.h —— IMAP 响应解析 + MIME encoded-word 解码(纯逻辑,可主机测试)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define IMAP_FROM_LEN 64
#define IMAP_SUBJECT_LEN 96
#define IMAP_DATE_LEN 32

typedef struct {
    char from[IMAP_FROM_LEN];
    char subject[IMAP_SUBJECT_LEN];
    char date[IMAP_DATE_LEN];
} imap_msg_t;

// 返回 "* <n> EXISTS" 中最大的 n;没有则 0。
int imap_parse_exists(const char *data, size_t len);

// 从 FETCH 响应(含 {n} literal)中提取 From/Subject/Date。
// 返回写入 out 的邮件数(最多 max 条)。
int imap_parse_fetch_headers(const char *data, size_t len, imap_msg_t *out, int max);

// 解码 =?UTF-8?B?...?= / =?UTF-8?Q?...?=;返回写入字节数(不含结尾 NUL)。
size_t imap_decode_mime_words(const char *in, char *out, size_t out_size);
