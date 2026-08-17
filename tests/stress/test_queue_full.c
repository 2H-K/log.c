/**
 * test_queue_full.c - Queue full behavior tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_queue_drop_policy(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_queue_policy(ctx, LOG_QUEUE_DROP);
    log_set_async(ctx, true);

    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "drop test %d", i);
    }

    log_set_async(ctx, false);

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT(stats.queue_drops > 0, "should have drops");
    printf("    (drops: %lu)", stats.queue_drops);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("queue drop policy");
}

static void test_queue_fallback_sync_policy(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_queue_policy(ctx, LOG_QUEUE_FALLBACK_SYNC);
    log_set_async(ctx, true);

    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "fallback test %d", i);
    }

    log_set_async(ctx, false);

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.queue_drops, 0, "fallback sync should not drop");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("queue fallback sync policy");
}

static void test_queue_block_policy(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_queue_policy(ctx, LOG_QUEUE_BLOCK);
    log_set_async(ctx, true);

    for (int i = 0; i < 5000; i++) {
        log_ctx_info(ctx, "block test %d", i);
    }

    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("queue block policy");
}

static void test_queue_policy_bounds_check(void) {
    log *ctx = log_create();

    log_set_queue_policy(ctx, -1);
    log_set_queue_policy(ctx, 99);

    log_destroy(ctx);
    TEST_PASS("queue policy bounds check");
}

void test_queue_full_register(void) {
    test_add(test_queue_drop_policy, "queue_drop_policy");
    test_add(test_queue_fallback_sync_policy, "queue_fallback_sync_policy");
    test_add(test_queue_block_policy, "queue_block_policy");
    test_add(test_queue_policy_bounds_check, "queue_policy_bounds_check");
}
