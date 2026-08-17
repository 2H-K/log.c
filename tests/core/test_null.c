/**
 * test_null.c - NULL safety tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_null_log_context(void) {
    log_log(NULL, LOG_INFO, __FILE__, __LINE__, "test");
    log_set_level(NULL, LOG_DEBUG);
    log_set_quiet(NULL, true);
    log_destroy(NULL);
    log_set_format(NULL, NULL);
    log_set_async(NULL, false);
    log_set_max_file_size(NULL, 0);
    log_set_file_prefix(NULL, NULL);
    log_remove_handler(NULL, 0);
    log_enable_thread_id(NULL, 0, false);
    log_handler_set_level(NULL, 0, LOG_INFO);
    log_get_stats(NULL, NULL);
    TEST_PASS("NULL context handling");
}

static void test_null_format_string(void) {
    log *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_ctx_info(ctx, NULL);

    log_destroy(ctx);
    TEST_PASS("NULL format string");
}

static void test_null_filename(void) {
    log *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    int idx = log_add_file(ctx, NULL, LOG_INFO);
    TEST_ASSERT_EQ(idx, -1, "NULL filename returns -1");

    log_destroy(ctx);
    TEST_PASS("NULL filename");
}

static void test_null_file_pointer(void) {
    log *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    int idx = log_add_fp(ctx, NULL, LOG_INFO);
    TEST_ASSERT_EQ(idx, -1, "NULL fp returns -1");

    log_destroy(ctx);
    TEST_PASS("NULL file pointer");
}

static void test_null_handler_functions(void) {
    log *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    int idx = log_add_handler(ctx, NULL, NULL, LOG_INFO);
    TEST_ASSERT_EQ(idx, -1, "NULL handler returns -1");

    log_destroy(ctx);
    TEST_PASS("NULL handler function");
}

static void test_null_stats_pointer(void) {
    log *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    int ret = log_get_stats(ctx, NULL);
    TEST_ASSERT_EQ(ret, -1, "NULL stats returns -1");

    log_destroy(ctx);
    TEST_PASS("NULL stats pointer");
}

static void test_null_prefix(void) {
    log *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_file_prefix(ctx, NULL);

    log_destroy(ctx);
    TEST_PASS("NULL prefix");
}

void test_null_register(void) {
    test_add(test_null_log_context, "null_log_context");
    test_add(test_null_format_string, "null_format_string");
    test_add(test_null_filename, "null_filename");
    test_add(test_null_file_pointer, "null_file_pointer");
    test_add(test_null_handler_functions, "null_handler_functions");
    test_add(test_null_stats_pointer, "null_stats_pointer");
    test_add(test_null_prefix, "null_prefix");
}
