/**
 * test_queue_full.c - Queue full behavior tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

/* Deterministic slow sink: busy-waits ~100us per message and counts
 * deliveries, so a burst provably overflows a small queue regardless of
 * machine speed. Only the async writer thread invokes this (DROP policy
 * has no fallback path), so a plain counter is safe. */
static int g_slow_sink_delivered = 0;

static void slow_sink_handler(log_handle *ctx, log_event *ev) {
    (void)ctx;
    (void)ev;
    uint64_t t0 = test_now_ns();
    while (test_now_ns() - t0 < 100000ULL) { /* spin ~100us */ }
    g_slow_sink_delivered++;
}

static void test_queue_drop_policy(void) {
    /* The batch-draining consumer is fast enough to keep up with a plain
     * burst, so overflow must be forced deterministically: a slow sink
     * plus a small queue guarantees drops regardless of machine speed. */
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    g_slow_sink_delivered = 0;
    log_handle *ctx = log_create();
    int idx = log_add_handler(ctx, slow_sink_handler, NULL, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add slow sink");
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_queue_size(ctx, 64);
    log_set_queue_policy(ctx, LOG_QUEUE_DROP);
    log_set_async(ctx, true);

    const int SENT = 1000;
    for (int i = 0; i < SENT; i++) {
        log_ctx_info(ctx, "drop test %d", i);
    }

    log_set_async(ctx, false);

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT(stats.queue_drops > 0, "should have drops");
    /* Accounting: every message is either delivered or dropped. */
    TEST_ASSERT_EQ((int)stats.queue_drops + g_slow_sink_delivered, SENT,
                   "delivered + dropped == sent");
    printf("    (drops: %llu, delivered: %d)",
           (unsigned long long)stats.queue_drops, g_slow_sink_delivered);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("queue drop policy");
}

static void test_queue_fallback_sync_policy(void) {
    /* Regression guard: ring-full + FALLBACK_SYNC must WRITE EVERY MESSAGE
     * synchronously. The old implementation silently dropped overflowing
     * messages on the ring path (queue_drops stayed 0, so the old assertion
     * passed while 56% of messages vanished). Verify delivery, not just
     * the drop counter. */
    const char *path = TEST_TMP_DIR "test_fallback_delivery.log";
    remove(path);

    log_handle *ctx = log_create();
    int idx = log_add_file(ctx, path, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add file handler");
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_queue_size(ctx, 64);   /* tiny queue: overflow is guaranteed */
    log_set_queue_policy(ctx, LOG_QUEUE_FALLBACK_SYNC);
    log_set_async(ctx, true);

    const int SENT = 20000;
    for (int i = 0; i < SENT; i++) {
        log_ctx_info(ctx, "fallback test %d", i);
    }

    log_set_async(ctx, false);

    log_stats stats;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(stats.queue_drops, 0, "fallback sync should not drop");

    log_destroy(ctx);   /* flushes owned file */

    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(f, "delivery file readable");
    char buf[512];
    int delivered = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "fallback test")) delivered++;
    }
    fclose(f);
    TEST_ASSERT_EQ(delivered, SENT, "every message delivered despite overflow");

    remove(path);
    TEST_PASS("queue fallback sync policy");
}

static void test_queue_block_policy(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
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
    log_handle *ctx = log_create();

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
