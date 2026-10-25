// tests/test_imap_parse.c —— IMAP EXISTS / FETCH 头解析与 encoded-word 解码。
#include "imap_parse.h"

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

static void test_tag_status(void) {
    bool ok = false;
    const char *a1 = "A1 OK LOGIN completed\r\n";
    CHECK(imap_find_tag_status(a1, strlen(a1), "A1", &ok));
    CHECK(ok);

    const char *a2 = "* CAPABILITY IMAP4rev1\r\nA2 NO [AUTHENTICATIONFAILED] bad\r\n";
    ok = true;
    CHECK(imap_find_tag_status(a2, strlen(a2), "A2", &ok));
    CHECK(!ok);

    const char *a3 = "* 5 EXISTS\r\n* OK [READ-WRITE] done\r\nA3 OK FETCH done\r\n";
    ok = false;
    CHECK(imap_find_tag_status(a3, strlen(a3), "A3", &ok));
    CHECK(ok);

    const char *bad = "A9 BAD nonsense\r\n";
    ok = true;
    CHECK(imap_find_tag_status(bad, strlen(bad), "A9", &ok));
    CHECK(!ok);

    const char *missing = "* 1 FETCH (FLAGS (\\Seen))\r\nA4 OK done\r\n";
    CHECK(!imap_find_tag_status(missing, strlen(missing), "A7", NULL));
}

static void test_exists(void) {
    const char *resp = "* 12 EXISTS\r\n* 0 RECENT\r\nA2 OK SELECT done\r\n";
    CHECK(imap_parse_exists(resp, strlen(resp)) == 12);
    const char *none = "* OK no mail\r\n";
    CHECK(imap_parse_exists(none, strlen(none)) == 0);
}

static void test_decode(void) {
    char out[128];
    imap_decode_mime_words("=?UTF-8?B?5L2g5aW9?=", out, sizeof(out));
    CHECK(strcmp(out, "你好") == 0);

    imap_decode_mime_words("Hi =?UTF-8?Q?=E4=BD=A0=E5=A5=BD?=!", out, sizeof(out));
    CHECK(strcmp(out, "Hi 你好!") == 0);

    imap_decode_mime_words("A =?UTF-8?B?5L2g?= =?UTF-8?B?5aW9?= B", out, sizeof(out));
    CHECK(strcmp(out, "A 你好 B") == 0);

    imap_decode_mime_words("plain text", out, sizeof(out));
    CHECK(strcmp(out, "plain text") == 0);
}

static void test_fetch(void) {
    const char *resp =
        "* 3 FETCH (UID 7 BODY[HEADER.FIELDS (FROM SUBJECT DATE)] {106}\r\n"
        "From: =?UTF-8?B?5byg5LiJ?= <a@b.com>\r\n"
        "Subject: Quarterly report\r\n"
        "Date: Tue, 21 Oct 2026 10:00:00 +0800\r\n"
        "\r\n"
        ")\r\n"
        "* 4 FETCH (BODY[HEADER.FIELDS (FROM SUBJECT DATE)] {121}\r\n"
        "From: Li <li@example.com>\r\n"
        "Subject: =?UTF-8?Q?=E4=BC=9A=E8=AE=AE?=\r\n"
        " continued\r\n"
        "Date: Wed, 22 Oct 2026 09:30:00 +0800\r\n"
        "\r\n"
        ")\r\n"
        "A3 OK FETCH done\r\n";

    imap_msg_t msgs[4];
    int n = imap_parse_fetch_headers(resp, strlen(resp), msgs, 4);
    CHECK(n == 2);
    CHECK(strcmp(msgs[0].from, "张三 <a@b.com>") == 0);
    CHECK(strcmp(msgs[0].subject, "Quarterly report") == 0);
    CHECK(strcmp(msgs[0].date, "Tue, 21 Oct 2026 10:00:00 +0800") == 0);
    CHECK(strcmp(msgs[1].from, "Li <li@example.com>") == 0);
    CHECK(strcmp(msgs[1].subject, "会议 continued") == 0);
}

static void test_fetch_limit(void) {
    const char *resp =
        "* 1 FETCH (BODY[HEADER.FIELDS (FROM)] {17}\r\nFrom: a@b.com\r\n\r\n)\r\n"
        "* 2 FETCH (BODY[HEADER.FIELDS (FROM)] {17}\r\nFrom: c@d.com\r\n\r\n)\r\n";
    imap_msg_t msgs[1];
    CHECK(imap_parse_fetch_headers(resp, strlen(resp), msgs, 1) == 1);
    CHECK(strcmp(msgs[0].from, "a@b.com") == 0);
}

int main(void) {
    test_tag_status();
    test_exists();
    test_decode();
    test_fetch();
    test_fetch_limit();
    puts("test_imap_parse: PASS");
    return 0;
}
