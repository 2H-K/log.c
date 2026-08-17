/**
 * test_handlers.c - Handler management tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_handler_add_fp(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_DEBUG);
    TEST_ASSERT(idx >= 0, "log_add_fp returns valid index");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("handler add fp");
}

static void test_handler_add_file(void) {
    log *ctx = log_create();
    int idx = log_add_file(ctx, TEST_TMP_FILE, LOG_INFO);
    TEST_ASSERT(idx >= 0, "log_add_file succeeds");

    log_destroy(ctx);
    remove(TEST_TMP_FILE);
    TEST_PASS("handler add file");
}

static void test_handler_add_null_file(void) {
    log *ctx = log_create();
    int idx = log_add_file(ctx, NULL, LOG_INFO);
    TEST_ASSERT_EQ(idx, -1, "NULL filename returns -1");

    log_destroy(ctx);
    TEST_PASS("handler add null file");
}

static void test_handler_remove(void) {
    FILE *fp1 = fopen(TEST_DEV_NULL, "w");
    FILE *fp2 = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp1 && fp2, "fopen");

    log *ctx = log_create();
    /* Disable default stderr handler */
    ctx->handlers[0].active = false;
    int idx1 = log_add_fp(ctx, fp1, LOG_DEBUG);
    int idx2 = log_add_fp(ctx, fp2, LOG_INFO);
    TEST_ASSERT(idx1 >= 0 && idx2 >= 0, "handlers added");
    TEST_ASSERT_EQ(ctx->handler_count, 3, "three handlers total (default + 2)");

    log_remove_handler(ctx, idx1);
    /* handler_count stays the same, but handler is inactive */
    TEST_ASSERT_EQ(ctx->handlers[idx1].active, 0, "handler should be inactive");

    log_destroy(ctx);
    fclose(fp1);
    fclose(fp2);
    TEST_PASS("handler remove");
}

static void test_handler_set_level(void) {
    FILE *fp = fopen("test_h1.txt", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    /* Disable default stderr handler to isolate test */
    ctx->handlers[0].active = false;

    log_handler_set_level(ctx, idx, LOG_ERROR);
    log_ctx_info(ctx, "filtered");
    log_ctx_error(ctx, "passes");

    log_destroy(ctx);
    fclose(fp);

    /* Read file and verify only error was written */
    fp = fopen("test_h1.txt", "r");
    TEST_ASSERT_NOT_NULL(fp, "reopen");
    char buf[1024];
    int lines = 0;
    int has_error = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        lines++;
        if (strstr(buf, "passes")) has_error = 1;
    }
    fclose(fp);
    TEST_ASSERT_EQ(lines, 1, "only one line written");
    TEST_ASSERT(has_error, "error message present");
    remove("test_h1.txt");

    TEST_PASS("handler set level");
}

static void test_handler_max_capacity(void) {
    log *ctx = log_create();
    /* Disable default handler to maximize available slots */
    ctx->handlers[0].active = false;
    int added = 0;

    for (int i = 0; i < 40; i++) {
        FILE *fp = fopen(TEST_DEV_NULL, "w");
        int idx = log_add_fp(ctx, fp, LOG_INFO);
        if (idx < 0) {
            fclose(fp);
            break;
        }
        added++;
    }

    TEST_ASSERT(added >= 31, "at least 31 handlers (32 max - 1 default)");
    printf("    (added %d handlers)", added);

    log_destroy(ctx);
    TEST_PASS("handler max capacity");
}

static void test_handler_add_null_fp(void) {
    log *ctx = log_create();
    int idx = log_add_fp(ctx, NULL, LOG_INFO);
    TEST_ASSERT_EQ(idx, -1, "NULL fp returns -1");

    log_destroy(ctx);
    TEST_PASS("handler add null fp");
}

void test_handlers_register(void) {
    test_add(test_handler_add_fp, "handler_add_fp");
    test_add(test_handler_add_file, "handler_add_file");
    test_add(test_handler_add_null_file, "handler_add_null_file");
    test_add(test_handler_remove, "handler_remove");
    test_add(test_handler_set_level, "handler_set_level");
    test_add(test_handler_max_capacity, "handler_max_capacity");
    test_add(test_handler_add_null_fp, "handler_add_null_fp");
}
