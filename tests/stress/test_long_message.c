/**
 * test_long_message.c - Long message handling tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_long_message_1k(void) {
    FILE *fp = fopen("test_tmp.log", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    char msg[1024];
    memset(msg, 'A', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';

    log_ctx_info(ctx, "%s", msg);

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.log", "r");
    char buf[2048];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read long line");
    TEST_ASSERT(strlen(buf) >= 1024, "message at least 1024 chars");
    fclose(fp);
    remove("test_tmp.log");

    TEST_PASS("long message 1k");
}

static void test_long_message_4k(void) {
    FILE *fp = fopen("test_tmp.log", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    char msg[4096];
    memset(msg, 'B', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';

    log_ctx_info(ctx, "%s", msg);

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.log", "r");
    char buf[8192];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read 4k line");
    TEST_ASSERT(strlen(buf) >= 4096, "message at least 4096 chars");
    fclose(fp);
    remove("test_tmp.log");

    TEST_PASS("long message 4k");
}

static void test_long_message_json(void) {
    FILE *fp = fopen("test_tmp.json", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_json_format(ctx);

    char msg[2048];
    memset(msg, 'C', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';

    log_ctx_info(ctx, "%s", msg);

    log_destroy(ctx);
    fclose(fp);

    fp = fopen("test_tmp.json", "r");
    char buf[4096];
    TEST_ASSERT(fgets(buf, sizeof(buf), fp) != NULL, "read JSON with long msg");
    TEST_ASSERT(buf[0] == '{', "valid JSON start");
    fclose(fp);
    remove("test_tmp.json");

    TEST_PASS("long message JSON");
}

static void test_long_message_async(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    for (int i = 0; i < 100; i++) {
        char msg[1024];
        memset(msg, 'A' + (i % 26), sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = '\0';
        log_ctx_info(ctx, "%s", msg);
    }

    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("long message async");
}

void test_long_message_register(void) {
    test_add(test_long_message_1k, "long_message_1k");
    test_add(test_long_message_4k, "long_message_4k");
    test_add(test_long_message_json, "long_message_json");
    test_add(test_long_message_async, "long_message_async");
}
