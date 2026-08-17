/**
 * test_levels.c - Log level filtering tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_level_filter_all_pass(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_ctx_trace(ctx, "trace");
    log_ctx_debug(ctx, "debug");
    log_ctx_info(ctx, "info");
    log_ctx_warn(ctx, "warn");
    log_ctx_error(ctx, "error");

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.total_count, 5, "all 5 messages counted");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("level filter all pass");
}

static void test_level_filter_warn_and_above(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_level(ctx, LOG_WARN);

    log_ctx_trace(ctx, "trace");
    log_ctx_debug(ctx, "debug");
    log_ctx_info(ctx, "info");
    log_ctx_warn(ctx, "warn");
    log_ctx_error(ctx, "error");
    log_ctx_fatal(ctx, "fatal");

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.total_count, 3, "only warn/error/fatal pass");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("level filter WARN and above");
}

static void test_level_filter_quiet_mode(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_quiet(ctx, true);
    log_ctx_info(ctx, "suppressed");
    log_set_quiet(ctx, false);
    log_ctx_info(ctx, "passes");

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.total_count, 1, "only one message passes");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("quiet mode filtering");
}

static void test_level_counts_accuracy(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    for (int i = 0; i < 10; i++) log_ctx_trace(ctx, "t");
    for (int i = 0; i < 20; i++) log_ctx_debug(ctx, "d");
    for (int i = 0; i < 30; i++) log_ctx_info(ctx, "i");
    for (int i = 0; i < 40; i++) log_ctx_warn(ctx, "w");
    for (int i = 0; i < 50; i++) log_ctx_error(ctx, "e");

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.total_count, 150, "total 150");
    TEST_ASSERT_EQ(stats.level_counts[LOG_TRACE], 10, "trace count");
    TEST_ASSERT_EQ(stats.level_counts[LOG_DEBUG], 20, "debug count");
    TEST_ASSERT_EQ(stats.level_counts[LOG_INFO], 30, "info count");
    TEST_ASSERT_EQ(stats.level_counts[LOG_WARN], 40, "warn count");
    TEST_ASSERT_EQ(stats.level_counts[LOG_ERROR], 50, "error count");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("level counts accuracy");
}

static void test_level_string_representation(void) {
    TEST_ASSERT_STR_EQ(log_level_string(LOG_TRACE), "TRACE", "TRACE");
    TEST_ASSERT_STR_EQ(log_level_string(LOG_DEBUG), "DEBUG", "DEBUG");
    TEST_ASSERT_STR_EQ(log_level_string(LOG_INFO), "INFO", "INFO");
    TEST_ASSERT_STR_EQ(log_level_string(LOG_WARN), "WARN", "WARN");
    TEST_ASSERT_STR_EQ(log_level_string(LOG_ERROR), "ERROR", "ERROR");
    TEST_ASSERT_STR_EQ(log_level_string(LOG_FATAL), "FATAL", "FATAL");
    TEST_ASSERT_STR_EQ(log_level_string(-1), "UNKNOWN", "invalid level");
    TEST_ASSERT_STR_EQ(log_level_string(99), "UNKNOWN", "out of range");
    TEST_PASS("level string representation");
}

void test_levels_register(void) {
    test_add(test_level_filter_all_pass, "level_filter_all_pass");
    test_add(test_level_filter_warn_and_above, "level_filter_warn_and_above");
    test_add(test_level_filter_quiet_mode, "level_filter_quiet_mode");
    test_add(test_level_counts_accuracy, "level_counts_accuracy");
    test_add(test_level_string_representation, "level_string_representation");
}
