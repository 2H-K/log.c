#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { setvbuf(stdout, NULL, _IONBF, 0); printf("[TEST] %s...\n", name); } while(0)
#define PASS() do { tests_passed++; printf("  PASS\n"); } while(0)
#define FAIL(msg) do { tests_failed++; printf("  FAIL: %s\n", msg); } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

static void test_null_fmt(void) {
    TEST("NULL format string");
    log *ctx = log_create();
    log_add_fp(ctx, NULL, LOG_INFO);
    log_ctx_info(ctx, NULL);
    log_destroy(ctx);
    PASS();
}

static void test_null_file(void) {
    TEST("NULL file path");
    log *ctx = log_create();
    int idx = log_add_file(ctx, NULL, LOG_INFO);
    ASSERT(idx == -1, "should return -1");
    log_destroy(ctx);
    PASS();
}

static void test_null_ctx(void) {
    TEST("NULL context");
    log_log(NULL, LOG_INFO, __FILE__, __LINE__, "test");
    log_set_level(NULL, LOG_DEBUG);
    log_set_quiet(NULL, true);
    log_destroy(NULL);
    PASS();
}

static void test_json_quote(void) {
    TEST("JSON escape quote");
    FILE *fp = fopen("test_jq.txt", "w");
    ASSERT(fp != NULL, "fopen failed");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);
    log_ctx_info(ctx, "say \"hello\"");
    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_jq.txt", "r");
    ASSERT(fp != NULL, "reopen failed");
    char buf[1024];
    int found = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "\\\"hello\\\"")) found = 1;
    }
    fclose(fp);
    ASSERT(found, "quotes not escaped");
    remove("test_jq.txt");
    PASS();
}

static void test_json_nl(void) {
    TEST("JSON escape newline");
    FILE *fp = fopen("test_jn.txt", "w");
    ASSERT(fp != NULL, "fopen failed");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);
    log_ctx_info(ctx, "line1\nline2");
    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_jn.txt", "r");
    ASSERT(fp != NULL, "reopen failed");
    char buf[1024];
    int found = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "\\n")) found = 1;
    }
    fclose(fp);
    ASSERT(found, "newline not escaped");
    remove("test_jn.txt");
    PASS();
}

static void test_json_bs(void) {
    TEST("JSON escape backslash");
    FILE *fp = fopen("test_jb.txt", "w");
    ASSERT(fp != NULL, "fopen failed");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);
    log_ctx_info(ctx, "path\\to\\file");
    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_jb.txt", "r");
    ASSERT(fp != NULL, "reopen failed");
    char buf[1024];
    int found = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "\\\\")) found = 1;
    }
    fclose(fp);
    ASSERT(found, "backslash not escaped");
    remove("test_jb.txt");
    PASS();
}

static void test_json_tab(void) {
    TEST("JSON escape tab");
    FILE *fp = fopen("test_jt.txt", "w");
    ASSERT(fp != NULL, "fopen failed");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);
    log_ctx_info(ctx, "col1\tcol2");
    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_jt.txt", "r");
    ASSERT(fp != NULL, "reopen failed");
    char buf[1024];
    int found = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "\\t")) found = 1;
    }
    fclose(fp);
    ASSERT(found, "tab not escaped");
    remove("test_jt.txt");
    PASS();
}

static void test_json_struct(void) {
    TEST("JSON structure");
    FILE *fp = fopen("test_js.txt", "w");
    ASSERT(fp != NULL, "fopen failed");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);
    log_ctx_info(ctx, "test");
    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_js.txt", "r");
    ASSERT(fp != NULL, "reopen failed");
    char buf[1024];
    int ok = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "\"time\"") && strstr(buf, "\"level\"") && strstr(buf, "\"message\"")) ok = 1;
    }
    fclose(fp);
    ASSERT(ok, "missing fields");
    remove("test_js.txt");
    PASS();
}

static void test_levels(void) {
    TEST("Level filtering");
    FILE *fp = fopen("test_lv.txt", "w");
    ASSERT(fp != NULL, "fopen failed");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_set_level(ctx, LOG_WARN);
    log_ctx_trace(ctx, "trace");
    log_ctx_info(ctx, "info");
    log_ctx_warn(ctx, "warn");
    log_ctx_error(ctx, "error");
    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_lv.txt", "r");
    char buf[1024];
    int lines = 0;
    while (fgets(buf, sizeof(buf), fp)) lines++;
    fclose(fp);
    ASSERT(lines == 2, "should have 2 lines");
    remove("test_lv.txt");
    PASS();
}

static void test_handler(void) {
    TEST("Handler add/remove");
    log *ctx = log_create();
    FILE *fp1 = fopen("test_h1.txt", "w");
    FILE *fp2 = fopen("test_h2.txt", "w");
    ASSERT(fp1 != NULL && fp2 != NULL, "fopen failed");
    int idx1 = log_add_fp(ctx, fp1, LOG_DEBUG);
    int idx2 = log_add_fp(ctx, fp2, LOG_INFO);
    log_remove_handler(ctx, idx1);
    ctx->handlers[0].active = false;
    log_ctx_info(ctx, "test");
    log_destroy(ctx);
    fclose(fp1);
    fclose(fp2);

    int h1 = 0, h2 = 0;
    fp1 = fopen("test_h1.txt", "r");
    char buf[256];
    if (fgets(buf, sizeof(buf), fp1)) h1 = 1;
    fclose(fp1);
    fp2 = fopen("test_h2.txt", "r");
    if (fgets(buf, sizeof(buf), fp2)) h2 = 1;
    fclose(fp2);
    ASSERT(!h1 && h2, "handler 1 should be removed");
    remove("test_h1.txt");
    remove("test_h2.txt");
    PASS();
}

static void test_stats(void) {
    TEST("Stats accuracy");
    FILE *fp = fopen("/dev/null", "w");
    if (!fp) fp = fopen("NUL", "w");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;
    for (int i = 0; i < 100; i++) log_ctx_trace(ctx, "t");
    for (int i = 0; i < 50; i++) log_ctx_debug(ctx, "d");
    for (int i = 0; i < 200; i++) log_ctx_info(ctx, "i");
    log_stats s;
    log_get_stats(ctx, &s);
    fclose(fp);
    log_destroy(ctx);
    ASSERT(s.total_count == 350, "total should be 350");
    ASSERT(s.level_counts[LOG_TRACE] == 100, "trace should be 100");
    ASSERT(s.level_counts[LOG_DEBUG] == 50, "debug should be 50");
    ASSERT(s.level_counts[LOG_INFO] == 200, "info should be 200");
    PASS();
}

static void test_rotation(void) {
    TEST("Rotation");
    log *ctx = log_create();
    log_set_file_prefix(ctx, "test_rot");
    log_set_max_file_size(ctx, 1024);
    int idx = log_add_file(ctx, "test_rot", LOG_INFO);
    ASSERT(idx >= 0, "add_file failed");
    if (idx >= 0) ctx->handlers[0].active = false;
    for (int i = 0; i < 100; i++)
        log_ctx_info(ctx, "rotation test message %d with padding data", i);
    log_destroy(ctx);
    FILE *f = fopen("test_rot.1", "r");
    ASSERT(f != NULL, "rotation file should exist");
    if (f) fclose(f);
    remove("test_rot");
    remove("test_rot.1");
    remove("test_rot.2");
    remove("test_rot.3");
    remove("test_rot.4");
    remove("test_rot.5");
    PASS();
}

static void test_quiet(void) {
    TEST("Quiet mode");
    FILE *fp = fopen("test_q.txt", "w");
    ASSERT(fp != NULL, "fopen failed");
    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_set_quiet(ctx, true);
    log_ctx_info(ctx, "hidden");
    log_set_quiet(ctx, false);
    log_ctx_info(ctx, "visible");
    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_q.txt", "r");
    char buf[1024];
    int lines = 0;
    while (fgets(buf, sizeof(buf), fp)) lines++;
    fclose(fp);
    ASSERT(lines == 1, "should have 1 line");
    remove("test_q.txt");
    PASS();
}

int main(void) {
    printf("=== Feature Safety Tests ===\n\n");

    test_null_fmt();
    test_null_file();
    test_null_ctx();

    test_json_quote();
    test_json_nl();
    test_json_bs();
    test_json_tab();
    test_json_struct();

    test_levels();
    test_handler();
    test_stats();
    test_rotation();
    test_quiet();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed;
}
