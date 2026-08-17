/**
 * test_unicode_path.c - Chinese/special character path tests
 * Verifies log file creation with non-ASCII paths
 */

#include "test_harness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <dirent.h>
#endif

static void test_chinese_path(void) {
    const char *path = "/tmp/测试日志_ログ_test.log";
    remove(path);

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen with Chinese path");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add handler with Chinese path");

    log_ctx_info(ctx, "Chinese path test message");

    log_destroy(ctx);
    fclose(fp);

    /* Verify file exists and has content */
    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "read back Chinese path file");

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Chinese path test message")) {
            found = 1;
            break;
        }
    }
    fclose(fp);

    TEST_ASSERT(found, "message written to Chinese path file");
    remove(path);
    TEST_PASS("Chinese path");
}

static void test_special_chars_path(void) {
    const char *path = "/tmp/test file (with) [special] {chars} & spaces.log";
    remove(path);

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen with special chars path");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add handler with special chars path");

    log_ctx_info(ctx, "Special chars path test");

    log_destroy(ctx);
    fclose(fp);

    /* Verify */
    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "read back special chars file");

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Special chars path test")) {
            found = 1;
            break;
        }
    }
    fclose(fp);

    TEST_ASSERT(found, "message written to special chars path file");
    remove(path);
    TEST_PASS("Special characters path");
}

static void test_unicode_json_content(void) {
    const char *path = "/tmp/test_unicode_content.log";
    remove(path);

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    log_enable_json_format(ctx);
    TEST_ASSERT(idx >= 0, "add handler for JSON");

    /* Log messages with Unicode content */
    log_ctx_info(ctx, "中文消息 test");
    log_ctx_info(ctx, "日本語テスト test");
    log_ctx_info(ctx, "Emoji: 🚀🎉 test");
    log_ctx_info(ctx, "Mixed: Hello 世界 ワールド test");

    log_destroy(ctx);
    fclose(fp);

    /* Verify file has JSON content */
    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "read back Unicode JSON file");

    char line[512];
    int json_lines = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Check basic JSON structure */
        if (strstr(line, "\"level\"") && strstr(line, "\"message\"")) {
            json_lines++;
        }
    }
    fclose(fp);

    printf("    JSON lines with Unicode: %d", json_lines);
    /* Library may output both JSON and text formats, so we expect at least 4 JSON lines */
    TEST_ASSERT(json_lines >= 4, "all 4 JSON messages written correctly");

    /* Also verify Unicode content is preserved */
    fp = fopen(path, "r");
    if (fp) {
        char bigbuf[4096];
        size_t n = fread(bigbuf, 1, sizeof(bigbuf)-1, fp);
        bigbuf[n] = 0;
        fclose(fp);
        TEST_ASSERT(strstr(bigbuf, "中文消息") != NULL, "Chinese content preserved");
        TEST_ASSERT(strstr(bigbuf, "日本語テスト") != NULL, "Japanese content preserved");
    }

    remove(path);
    TEST_PASS("Unicode JSON content");
}

static void test_long_path(void) {
    /* Create a deep directory path */
    char path[512];
#if defined(_WIN32) || defined(_WIN64)
    snprintf(path, sizeof(path), "test_very\\long\\nested\\path\\to\\log\\files\\application.log");
    system("mkdir test_very\\long\\nested\\path\\to\\log\\files 2>nul");
#else
    snprintf(path, sizeof(path), "/tmp/very/long/nested/path/to/log/files/application.log");
    system("mkdir -p /tmp/very/long/nested/path/to/log/files");
#endif
    remove(path);

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen with long nested path");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add handler with long path");

    log_ctx_info(ctx, "Long path test");

    log_destroy(ctx);
    fclose(fp);

    /* Verify */
    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "read back long path file");

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Long path test")) {
            found = 1;
            break;
        }
    }
    fclose(fp);

    TEST_ASSERT(found, "message written to long path file");

    /* Cleanup */
    remove(path);
#if defined(_WIN32) || defined(_WIN64)
    system("rmdir /s /q test_very 2>nul");
#else
    system("rm -rf /tmp/very");
#endif
    TEST_PASS("Long nested path");
}

void test_unicode_path_register(void) {
    test_add(test_chinese_path, "unicode_chinese_path");
    test_add(test_special_chars_path, "unicode_special_chars_path");
    test_add(test_unicode_json_content, "unicode_json_content");
    test_add(test_long_path, "unicode_long_path");
}
