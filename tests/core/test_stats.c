/**
 * test_stats.c - Performance statistics accuracy tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_stats_total_count(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "msg %d", i);
    }

    log_stats stats;
    int ret = log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(ret, 0, "get_stats succeeds");
    TEST_ASSERT_EQ(stats.total_count, 100, "total count is 100");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("stats total count");
}

static void test_stats_level_counts(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    for (int i = 0; i < 5; i++) log_ctx_trace(ctx, "t");
    for (int i = 0; i < 10; i++) log_ctx_debug(ctx, "d");
    for (int i = 0; i < 15; i++) log_ctx_info(ctx, "i");
    for (int i = 0; i < 20; i++) log_ctx_warn(ctx, "w");
    for (int i = 0; i < 25; i++) log_ctx_error(ctx, "e");

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.total_count, 75, "total 75");
    TEST_ASSERT_EQ(stats.level_counts[LOG_TRACE], 5, "trace 5");
    TEST_ASSERT_EQ(stats.level_counts[LOG_DEBUG], 10, "debug 10");
    TEST_ASSERT_EQ(stats.level_counts[LOG_INFO], 15, "info 15");
    TEST_ASSERT_EQ(stats.level_counts[LOG_WARN], 20, "warn 20");
    TEST_ASSERT_EQ(stats.level_counts[LOG_ERROR], 25, "error 25");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("stats level counts");
}

static void test_stats_filtered_count(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_level(ctx, LOG_WARN);

    for (int i = 0; i < 10; i++) log_ctx_trace(ctx, "t");
    for (int i = 0; i < 10; i++) log_ctx_info(ctx, "i");
    for (int i = 0; i < 10; i++) log_ctx_warn(ctx, "w");
    for (int i = 0; i < 10; i++) log_ctx_error(ctx, "e");

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.total_count, 20, "only warn/error counted");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("stats filtered count");
}

static void test_stats_async_counts(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_async(ctx, true);

    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "async %d", i);
    }

    log_set_async(ctx, false);

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT(stats.total_count >= 45, "async writes counted");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("stats async counts");
}

void test_stats_register(void) {
    test_add(test_stats_total_count, "stats_total_count");
    test_add(test_stats_level_counts, "stats_level_counts");
    test_add(test_stats_filtered_count, "stats_filtered_count");
    test_add(test_stats_async_counts, "stats_async_counts");
}
