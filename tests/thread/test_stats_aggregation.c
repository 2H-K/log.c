/**
 * test_stats_aggregation.c - Cross-thread statistics aggregation + latency
 * Platform: All
 *
 * Regression: log_get_stats() used to read only the calling thread's TLS
 * counters, so a main-thread snapshot after worker-thread logging reported
 * total_count == 0 (and async_writes/queue_drops, which live on other
 * threads, were never visible). The stats registry now aggregates every
 * registered thread slot.
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

#define AGG_THREADS 8
#define AGG_MSGS 20000

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI agg_writer(LPVOID arg) {
#else
static void* agg_writer(void *arg) {
#endif
    thread_arg *a = (thread_arg*)arg;
    for (int i = 0; i < a->count; i++) {
        log_ctx_info(a->ctx, "agg %d %d", a->thread_id, i);
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static void test_stats_multithread_aggregation(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    THREAD_T threads[AGG_THREADS];
    thread_arg args[AGG_THREADS];
    for (int i = 0; i < AGG_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = AGG_MSGS;
        THREAD_CREATE(threads[i], agg_writer, &args[i]);
    }
    for (int i = 0; i < AGG_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_stats stats;
    log_get_stats(ctx, &stats);
    printf("    total=%llu", (unsigned long long)stats.total_count);

    TEST_ASSERT_EQ(stats.total_count, (uint64_t)AGG_THREADS * AGG_MSGS,
                   "all threads' counts aggregated");
    TEST_ASSERT_EQ(stats.level_counts[LOG_INFO], (uint64_t)AGG_THREADS * AGG_MSGS,
                   "per-level counts aggregated");
    /* No async: every message took the sync path. */
    TEST_ASSERT_EQ(stats.sync_writes, (uint64_t)AGG_THREADS * AGG_MSGS,
                   "sync writes aggregated");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("stats multithread aggregation");
}

/* Deterministic slow sink so messages visibly dwell in the async queue. */
static void latency_slow_sink(log_handle *ctx, log_event *ev) {
    (void)ctx; (void)ev;
    uint64_t t0 = test_now_ns();
    while (test_now_ns() - t0 < 20000ULL) { /* spin ~20us */ }
}

static void test_stats_queue_latency_measured(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
    int idx = log_add_handler(ctx, latency_slow_sink, NULL, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add slow sink");
    if (idx >= 0) ctx->handlers[0].active = false;

    log_set_async(ctx, true);
    for (int i = 0; i < 5000; i++) {
        log_ctx_info(ctx, "latency %d", i);
    }
    log_set_async(ctx, false);

    log_stats stats;
    log_get_stats(ctx, &stats);
    printf("    avg_queue_latency_ms=%.4f", stats.avg_queue_latency_ms);
    TEST_ASSERT(stats.avg_queue_latency_ms > 0.0,
                "queue latency is measured (not the old hardcoded 0)");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("stats queue latency measured");
}

void test_stats_aggregation_register(void) {
    test_add(test_stats_multithread_aggregation, "stats_multithread_aggregation");
    test_add(test_stats_queue_latency_measured, "stats_queue_latency_measured");
}
