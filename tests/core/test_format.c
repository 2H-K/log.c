/**
 * test_format.c - Output format tests (text and JSON)
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_json_escape_double_quote(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    log_ctx_info(ctx, "say \"hello\" world");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.json", "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "\\\"hello\\\"") != NULL, "quotes escaped");
    fclose(fp);
    remove("test_tmp.json");

    TEST_PASS("JSON escape double quote");
}

static void test_json_escape_newline(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    log_ctx_info(ctx, "line1\nline2");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.json", "r");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "\\n") != NULL, "newline escaped");
    fclose(fp);
    remove("test_tmp.json");

    TEST_PASS("JSON escape newline");
}

static void test_json_escape_backslash(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    log_ctx_info(ctx, "path\\to\\file");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.json", "r");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "\\\\") != NULL, "backslash escaped");
    fclose(fp);
    remove("test_tmp.json");

    TEST_PASS("JSON escape backslash");
}

static void test_json_escape_tab(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    log_ctx_info(ctx, "col1\tcol2");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.json", "r");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "\\t") != NULL, "tab escaped");
    fclose(fp);
    remove("test_tmp.json");

    TEST_PASS("JSON escape tab");
}

static void test_json_structure_fields(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    log_ctx_info(ctx, "test message");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.json", "r");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "\"time\":") != NULL, "has time field");
    TEST_ASSERT(strstr(buf, "\"level\":") != NULL, "has level field");
    TEST_ASSERT(strstr(buf, "\"file\":") != NULL, "has file field");
    TEST_ASSERT(strstr(buf, "\"line\":") != NULL, "has line field");
    TEST_ASSERT(strstr(buf, "\"message\":") != NULL, "has message field");
    fclose(fp);
    remove("test_tmp.json");

    TEST_PASS("JSON structure fields");
}

static void test_json_thread_id_field(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) {
        ctx->handlers[0].active = false;
        log_enable_thread_id(ctx, idx, true);
    }
    log_enable_json_format(ctx);

    log_ctx_info(ctx, "with thread");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.json", "r");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "\"thread_id\":") != NULL, "has thread_id field");
    fclose(fp);
    remove("test_tmp.json");

    TEST_PASS("JSON thread_id field");
}

static void test_text_format_output(void) {
    FILE *fp = fopen("test_tmp.log", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_ctx_info(ctx, "text message");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.log", "r");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "INFO") != NULL, "contains INFO");
    TEST_ASSERT(strstr(buf, "text message") != NULL, "contains message");
    fclose(fp);
    remove("test_tmp.log");

    TEST_PASS("text format output");
}

void test_format_register(void) {
    test_add(test_json_escape_double_quote, "json_escape_double_quote");
    test_add(test_json_escape_newline, "json_escape_newline");
    test_add(test_json_escape_backslash, "json_escape_backslash");
    test_add(test_json_escape_tab, "json_escape_tab");
    test_add(test_json_structure_fields, "json_structure_fields");
    test_add(test_json_thread_id_field, "json_thread_id_field");
    test_add(test_text_format_output, "text_format_output");
}
