// main/imap_parse.c
#include "imap_parse.h"

#include <stdbool.h>
#include <string.h>

static char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool starts_with_ci(const char *s, size_t len, const char *prefix) {
    size_t n = strlen(prefix);
    if (len < n) return false;
    for (size_t i = 0; i < n; i++) {
        if (lower_ascii(s[i]) != lower_ascii(prefix[i])) return false;
    }
    return true;
}

int imap_parse_exists(const char *data, size_t len) {
    if (!data) return 0;
    int best = 0;
    size_t i = 0;
    while (i < len) {
        size_t line_end = i;
        while (line_end < len && data[line_end] != '\n') line_end++;
        size_t line_len = line_end - i;
        if (line_len > 8 && data[i] == '*' && data[i + 1] == ' ') {
            size_t p = i + 2;
            int value = 0;
            bool digits = false;
            while (p < line_end && data[p] >= '0' && data[p] <= '9') {
                value = value * 10 + (data[p] - '0');
                digits = true;
                p++;
            }
            if (digits) {
                while (p < line_end && (data[p] == ' ' || data[p] == '\r')) p++;
                if (starts_with_ci(data + p, line_end - p, "exists") && value > best) {
                    best = value;
                }
            }
        }
        i = line_end + 1;
    }
    return best;
}

// 状态码是否 OK(状态码只有 OK/NO/BAD,后接空格、行尾或 "[")。
static bool status_is_ok(const char *s, size_t len) {
    if (len < 2) return false;
    if (lower_ascii(s[0]) != 'o' || lower_ascii(s[1]) != 'k') return false;
    if (len == 2) return true;
    return s[2] == ' ' || s[2] == '\t' || s[2] == '\r' || s[2] == '\n' || s[2] == '[';
}

bool imap_find_tag_status(const char *data, size_t len, const char *tag, bool *ok) {
    if (!data || !tag || !tag[0]) return false;
    const size_t tag_len = strlen(tag);
    size_t i = 0;
    while (i < len) {
        if (i + tag_len < len && memcmp(data + i, tag, tag_len) == 0 &&
            data[i + tag_len] == ' ') {
            size_t p = i + tag_len + 1;
            while (p < len && (data[p] == ' ' || data[p] == '\t')) p++;
            size_t start = p;
            while (p < len && data[p] != '\r' && data[p] != '\n') p++;
            if (ok) *ok = status_is_ok(data + start, p - start);
            return true;
        }
        const char *nl = memchr(data + i, '\n', len - i);
        if (!nl) break;
        i = (size_t)(nl - data) + 1;
    }
    return false;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t decode_b64(const char *in, size_t len, char *out, size_t out_size) {
    uint32_t acc = 0;
    int bits = 0;
    size_t written = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=') break;
        int v = b64_value(in[i]);
        if (v < 0) continue; // 空白/换行
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written + 1 < out_size) out[written++] = (char)((acc >> bits) & 0xFF);
        }
    }
    return written;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t decode_q(const char *in, size_t len, char *out, size_t out_size) {
    size_t written = 0;
    for (size_t i = 0; i < len && written + 1 < out_size; i++) {
        if (in[i] == '_') {
            out[written++] = ' ';
        } else if (in[i] == '=' && i + 2 < len) {
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[written++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                out[written++] = in[i];
            }
        } else {
            out[written++] = in[i];
        }
    }
    return written;
}

size_t imap_decode_mime_words(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size == 0) return 0;
    size_t written = 0;
    size_t i = 0;
    while (in[i] && written + 1 < out_size) {
        if (in[i] != '=' || in[i + 1] != '?') {
            out[written++] = in[i++];
            continue;
        }
        // =?charset?enc?text?=
        const char *q1 = strchr(in + i + 2, '?');
        if (!q1) {
            out[written++] = in[i++];
            continue;
        }
        // q1 指向 charset 后的 '?',其后一个字符是编码 B/Q,再后是正文起点。
        const char *text = q1 + 3;
        const char *q2 = strchr(text, '?');
        if (!q2 || q2[1] != '=') {
            out[written++] = in[i++];
            continue;
        }
        char enc = lower_ascii(q1[1]);
        size_t text_len = (size_t)(q2 - text);
        size_t n = 0;
        if (enc == 'b') {
            n = decode_b64(text, text_len, out + written, out_size - written);
        } else if (enc == 'q') {
            n = decode_q(text, text_len, out + written, out_size - written);
        } else {
            out[written++] = in[i++];
            continue;
        }
        written += n;
        i = (size_t)(q2 + 2 - in); // 跳过结尾的 "?="
        // 相邻 encoded-word 之间的空白按 RFC 2047 丢弃
        size_t j = i;
        while (in[j] == ' ' || in[j] == '\t') j++;
        if (in[j] == '=' && in[j + 1] == '?') i = j;
    }
    out[written] = '\0';
    return written;
}

// 取 header 值并解码;value 可能已折叠。
static void store_value(const char *value, size_t len, char *out, size_t out_size) {
    while (len > 0 && (*value == ' ' || *value == '\t')) {
        value++;
        len--;
    }
    while (len > 0 && (value[len - 1] == '\r' || value[len - 1] == '\n' ||
                       value[len - 1] == ' ' || value[len - 1] == '\t')) {
        len--;
    }
    char raw[256];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, value, len);
    raw[len] = '\0';
    if (out_size) imap_decode_mime_words(raw, out, out_size);
}

static void parse_literal(const char *p, size_t len, imap_msg_t *msg) {
    memset(msg, 0, sizeof(*msg));
    char accum[256];
    size_t acc_len = 0;
    int field = -1; // 0=from 1=subject 2=date

    size_t i = 0;
    while (i < len) {
        size_t line_end = i;
        while (line_end < len && p[line_end] != '\n') line_end++;
        size_t line_len = line_end - i;
        size_t content = line_len;
        if (content > 0 && p[i + content - 1] == '\r') content--;

        if (content == 0) break; // 头部结束

        bool continuation = (p[i] == ' ' || p[i] == '\t');
        if (continuation) {
            if (field >= 0 && acc_len + 1 < sizeof(accum)) {
                accum[acc_len++] = ' ';
                size_t add = content;
                const char *src = p + i;
                while (add > 0 && (*src == ' ' || *src == '\t')) {
                    src++;
                    add--;
                }
                if (acc_len + add >= sizeof(accum)) add = sizeof(accum) - 1 - acc_len;
                memcpy(accum + acc_len, src, add);
                acc_len += add;
            }
            i = line_end + 1;
            continue;
        }

        // 新的头部行:先落上一项
        if (field == 0) store_value(accum, acc_len, msg->from, sizeof(msg->from));
        else if (field == 1) store_value(accum, acc_len, msg->subject, sizeof(msg->subject));
        else if (field == 2) store_value(accum, acc_len, msg->date, sizeof(msg->date));

        field = -1;
        acc_len = 0;
        const char *colon = memchr(p + i, ':', content);
        if (!colon) {
            i = line_end + 1;
            continue;
        }
        size_t key_len = (size_t)(colon - (p + i));
        size_t value_len = content - key_len - 1;
        const char *value = colon + 1;
        if (starts_with_ci(p + i, key_len, "from")) field = 0;
        else if (starts_with_ci(p + i, key_len, "subject")) field = 1;
        else if (starts_with_ci(p + i, key_len, "date")) field = 2;
        if (field >= 0 && value_len < sizeof(accum)) {
            memcpy(accum, value, value_len);
            acc_len = value_len;
        }
        i = line_end + 1;
    }
    if (field == 0) store_value(accum, acc_len, msg->from, sizeof(msg->from));
    else if (field == 1) store_value(accum, acc_len, msg->subject, sizeof(msg->subject));
    else if (field == 2) store_value(accum, acc_len, msg->date, sizeof(msg->date));
}

int imap_parse_fetch_headers(const char *data, size_t len, imap_msg_t *out, int max) {
    if (!data || !out || max <= 0) return 0;
    int count = 0;
    size_t i = 0;
    while (i < len && count < max) {
        if (data[i] != '{') {
            i++;
            continue;
        }
        size_t p = i + 1;
        uint32_t n = 0;
        bool digits = false;
        while (p < len && data[p] >= '0' && data[p] <= '9') {
            n = n * 10 + (uint32_t)(data[p] - '0');
            digits = true;
            p++;
            if (n > 8192) break;
        }
        if (!digits || p >= len || data[p] != '}') {
            i++;
            continue;
        }
        p++;
        if (p < len && data[p] == '\r') p++;
        if (p >= len || data[p] != '\n') {
            i++;
            continue;
        }
        p++;
        if (n == 0 || n > 8192 || p + n > len) {
            i++;
            continue;
        }
        parse_literal(data + p, n, &out[count]);
        count++;
        i = p + n;
    }
    return count;
}
