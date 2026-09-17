/**
 * test_format.c - Output format tests (text and JSON)
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_json_escape_double_quote(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
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

    log_handle *ctx = log_create();
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

    log_handle *ctx = log_create();
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

    log_handle *ctx = log_create();
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

    log_handle *ctx = log_create();
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

    log_handle *ctx = log_create();
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

    log_handle *ctx = log_create();
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

static void test_json_escapes_file_and_controls(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.raw_msg = "ctrl \x01 char";
    ev.file = "C:\\src\\log.c";
    ev.line = 7;
    ev.level = LOG_INFO;
    ev.timestamp = 0.0;

    char buf[512];
    int n = log_format_json(ctx, &ev, buf, sizeof(buf));
    TEST_ASSERT(n > 0, "log_format_json returns length");
    TEST_ASSERT(strstr(buf, "\"file\": \"C:\\\\src\\\\log.c\"") != NULL,
                "file backslashes escaped");
    TEST_ASSERT(strstr(buf, "\\u0001") != NULL, "control char escaped as \\u0001");
    TEST_ASSERT(strchr(buf, '\x01') == NULL, "no raw control byte in output");

    log_destroy(ctx);
    TEST_PASS("json file/control escaping");
}

/* Regression: log_handler_set_formatter used to set handlers[idx].fn = NULL
 * (silently disabling the handler) and only updated the global format_fn,
 * which the file handler path never consulted. A per-handler formatter must
 * actually write lines. */
static int custom_short_prefix(log_handle *ctx, log_event *ev, char *buf, size_t n) {
    (void)ctx; (void)ev;
    return snprintf(buf, n, "CUSTOM>");
}

static void test_handler_formatter_writes(void) {
    const char *path = TEST_TMP_DIR "test_handler_fmt.log";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add fp");
    ctx->handlers[0].active = false;   /* silence stderr */

    log_handler_set_formatter(ctx, idx, custom_short_prefix);
    log_ctx_info(ctx, "hello");

    log_destroy(ctx);
    fclose(fp);

    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[512];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "line was written (handler not disabled)");
    TEST_ASSERT(strstr(buf, "CUSTOM>") != NULL, "per-handler formatter used");
    TEST_ASSERT(strstr(buf, "hello") != NULL, "message body present");
    fclose(fp);
    remove(path);

    TEST_PASS("handler formatter writes");
}

/* Same guarantee through the async path: the writer thread must resolve the
 * per-handler formatter too. */
static void test_handler_formatter_async(void) {
    const char *path = TEST_TMP_DIR "test_handler_fmt_async.log";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add fp");
    ctx->handlers[0].active = false;

    log_handler_set_formatter(ctx, idx, custom_short_prefix);
    log_set_async(ctx, true);
    for (int i = 0; i < 10; i++) {
        log_ctx_info(ctx, "async %d", i);
    }
    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);

    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[512];
    int lines = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        lines++;
        TEST_ASSERT(strstr(buf, "CUSTOM>") != NULL, "async line uses formatter");
    }
    fclose(fp);
    TEST_ASSERT_EQ(lines, 10, "all async lines written");
    remove(path);

    TEST_PASS("handler formatter async");
}

#if LOG_FEATURE_KV
static void test_kv_text_suffix(void) {
    const char *path = TEST_TMP_DIR "test_kv_text.log";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add fp");
    ctx->handlers[0].active = false;

    log_ctx_info_kv(ctx, "boot", LOG_KV_STR("mod", "net"),
                    LOG_KV_INT("port", 8080), LOG_KV_BOOL("ready", true));

    log_destroy(ctx);
    fclose(fp);

    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "boot") != NULL, "message present");
    TEST_ASSERT(strstr(buf, "mod=net") != NULL, "str kv suffix");
    TEST_ASSERT(strstr(buf, "port=8080") != NULL, "int kv suffix");
    TEST_ASSERT(strstr(buf, "ready=true") != NULL, "bool kv suffix");
    fclose(fp);
    remove(path);

    TEST_PASS("kv text suffix");
}

static void test_kv_json_top_level(void) {
    const char *path = TEST_TMP_DIR "test_kv.json";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add fp");
    ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    log_ctx_info_kv(ctx, "login",
                    LOG_KV_STR("user", "alice"),
                    LOG_KV_INT("id", 42),
                    LOG_KV_DOUBLE("score", 1.5),
                    LOG_KV_BOOL("ok", true));

    log_destroy(ctx);
    fclose(fp);

    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "\"message\": \"login\"") != NULL, "message field only");
    TEST_ASSERT(strstr(buf, "\"user\": \"alice\"") != NULL, "str field quoted");
    TEST_ASSERT(strstr(buf, "\"id\": 42") != NULL, "int field unquoted");
    TEST_ASSERT(strstr(buf, "\"score\": 1.5") != NULL, "double field unquoted");
    TEST_ASSERT(strstr(buf, "\"ok\": true") != NULL, "bool field unquoted");
    fclose(fp);
    remove(path);

    TEST_PASS("kv json top level");
}

static void test_kv_json_escape(void) {
    const char *path = TEST_TMP_DIR "test_kv_escape.json";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add fp");
    ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    log_ctx_info_kv(ctx, "esc", LOG_KV_STR("s", "a\"b\nc\001d"));

    log_destroy(ctx);
    fclose(fp);

    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[1024];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read line");
    TEST_ASSERT(strstr(buf, "a\\\"b") != NULL, "quote escaped");
    TEST_ASSERT(strstr(buf, "\\n") != NULL, "newline escaped");
    TEST_ASSERT(strstr(buf, "\\u0001") != NULL, "control char escaped");
    TEST_ASSERT(strchr(buf, '\001') == NULL, "no raw control byte");
    fclose(fp);
    remove(path);

    TEST_PASS("kv json escaping");
}

static void test_kv_async(void) {
    const char *path = TEST_TMP_DIR "test_kv_async.json";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add fp");
    ctx->handlers[0].active = false;
    log_enable_json_format(ctx);
    log_set_async(ctx, true);

    for (int i = 0; i < 5; i++) {
        log_ctx_info_kv(ctx, "evt", LOG_KV_INT("seq", i), LOG_KV_STR("kind", "tick"));
    }
    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);

    fp = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[1024];
    int lines = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        lines++;
        TEST_ASSERT(strstr(buf, "\"kind\": \"tick\"") != NULL, "kv survives async");
    }
    fclose(fp);
    TEST_ASSERT_EQ(lines, 5, "all async kv lines written");
    remove(path);

    TEST_PASS("kv async transport");
}
#endif /* LOG_FEATURE_KV */

void test_format_register(void) {
    test_add(test_json_escape_double_quote, "json_escape_double_quote");
    test_add(test_json_escape_newline, "json_escape_newline");
    test_add(test_json_escape_backslash, "json_escape_backslash");
    test_add(test_json_escape_tab, "json_escape_tab");
    test_add(test_json_structure_fields, "json_structure_fields");
    test_add(test_json_thread_id_field, "json_thread_id_field");
    test_add(test_json_escapes_file_and_controls, "json_file_and_control_escaping");
    test_add(test_text_format_output, "text_format_output");
    test_add(test_handler_formatter_writes, "handler_formatter_writes");
    test_add(test_handler_formatter_async, "handler_formatter_async");
#if LOG_FEATURE_KV
    test_add(test_kv_text_suffix, "kv_text_suffix");
    test_add(test_kv_json_top_level, "kv_json_top_level");
    test_add(test_kv_json_escape, "kv_json_escaping");
    test_add(test_kv_async, "kv_async");
#endif
}
