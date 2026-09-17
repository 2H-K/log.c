/**
 * bench_latency.c - A1 tail-latency and overload-behaviour benchmarks
 * Platform: All
 *
 * A1-1: with a full queue and a slow sink, the *producer's* per-call latency
 *       must stay bounded (async's value is worst-case pause, not throughput).
 * A1-2: the three queue policies (DROP / FALLBACK_SYNC / BLOCK) must show the
 *       documented observable behaviour and statistics.
 */

#include "test_harness.h"
#include "log.h"
#include "perf/bench_common.h"

#include <stdlib.h>

/* ---- slow sink -------------------------------------------------------- */

typedef struct {
    int count;
#if defined(_WIN32) || defined(_WIN64)
    DWORD us;
#else
    long us;
#endif
} slow_sink;

static void slow_sleep(const slow_sink *s) {
#if defined(_WIN32) || defined(_WIN64)
    Sleep((DWORD)(s->us / 1000 > 0 ? s->us / 1000 : 1));
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = s->us * 1000L;
    nanosleep(&ts, NULL);
#endif
}

static void slow_sink_fn(log_handle *ctx, log_event *ev) {
    (void)ctx;
    slow_sink *s = (slow_sink*)ev->udata;
    s->count++;
    slow_sleep(s);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint64_t pct(const uint64_t *sorted, int n, double p) {
    int idx = (int)(p * (double)(n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return sorted[idx];
}

/* ---- A1-1: producer tail latency under overload ----------------------- */

static void test_bench_tail_latency(void) {
    slow_sink sink = { 0, 50 };   /* 50 us per sink call */
    const int n = 20000;

    log_handle *ctx = log_create();
    ctx->handlers[0].active = false;
    log_add_handler(ctx, slow_sink_fn, &sink, LOG_TRACE);
    log_set_queue_size(ctx, 256);          /* small queue -> guaranteed overload */
    log_set_queue_policy(ctx, LOG_QUEUE_DROP);
    log_set_async(ctx, true);

    uint64_t *lat = malloc((size_t)n * sizeof(uint64_t));
    TEST_ASSERT_NOT_NULL(lat, "malloc");

    /* Warm up the producer path (first call registers stats, etc.). */
    log_ctx_info(ctx, "warmup");

    for (int i = 0; i < n; i++) {
        uint64_t t0 = test_now_ns();
        log_ctx_info(ctx, "tail %d", i);
        lat[i] = test_now_ns() - t0;
    }

    log_set_async(ctx, false);
    qsort(lat, (size_t)n, sizeof(uint64_t), cmp_u64);

    uint64_t p50 = pct(lat, n, 0.50);
    uint64_t p99 = pct(lat, n, 0.99);
    uint64_t pmax = lat[n - 1];

    log_stats st;
    log_get_stats(ctx, &st);

    printf("    p50=%.2f us  p99=%.2f us  max=%.2f us  drops=%llu",
           p50 / 1000.0, p99 / 1000.0, pmax / 1000.0,
           (unsigned long long)st.queue_drops);

    /* The producer never touches the slow sink, so its pauses stay far below
     * the sink's per-message cost. 20 ms is a generous machine-independent
     * sanity bound; a synchronous path would be hundreds of ms here. */
    TEST_ASSERT(pmax < 20000000ULL, "producer max pause bounded under overload");
    TEST_ASSERT(st.queue_drops > 0, "overload actually dropped messages");

    free(lat);
    log_destroy(ctx);
    TEST_PASS("async producer tail latency");
}

/* ---- A1-2: overload behaviour per queue policy ------------------------ */

typedef struct {
    const char *name;
    int policy;
    uint64_t drops;
    uint64_t blocked;
    uint64_t sync_writes;
    uint64_t async_writes;
} overload_result;

static void run_overload(overload_result *r, int policy, const char *name) {
    slow_sink sink = { 0, 50 };
    const int n = 2000;

    log_handle *ctx = log_create();
    ctx->handlers[0].active = false;
    log_add_handler(ctx, slow_sink_fn, &sink, LOG_TRACE);
    log_set_queue_size(ctx, 16);
    log_set_queue_policy(ctx, policy);
    log_set_async(ctx, true);

    for (int i = 0; i < n; i++) {
        log_ctx_info(ctx, "overload %d", i);
    }
    log_set_async(ctx, false);

    log_stats st;
    log_get_stats(ctx, &st);
    r->name = name;
    r->policy = policy;
    r->drops = st.queue_drops;
    r->blocked = st.queue_blocked;
    r->sync_writes = st.sync_writes;
    r->async_writes = st.async_writes;

    printf("    %-13s drops=%llu blocked=%llu sync=%llu async=%llu",
           name, (unsigned long long)st.queue_drops,
           (unsigned long long)st.queue_blocked,
           (unsigned long long)st.sync_writes,
           (unsigned long long)st.async_writes);

    log_destroy(ctx);
}

static void test_bench_overload_matrix(void) {
    overload_result drop, fallback, block;

    run_overload(&drop, LOG_QUEUE_DROP, "DROP");
    run_overload(&fallback, LOG_QUEUE_FALLBACK_SYNC, "FALLBACK_SYNC");
    run_overload(&block, LOG_QUEUE_BLOCK, "BLOCK");

    /* DROP: excess messages are dropped, never written synchronously. */
    TEST_ASSERT(drop.drops > 0, "DROP reports drops");
    TEST_ASSERT_EQ(drop.sync_writes, 0, "DROP never falls back to sync");

    /* FALLBACK_SYNC: nothing dropped, overflow written synchronously. */
    TEST_ASSERT_EQ(fallback.drops, 0, "FALLBACK_SYNC drops nothing");
    TEST_ASSERT(fallback.sync_writes > 0, "FALLBACK_SYNC writes synchronously");

    /* BLOCK: nothing dropped, producers wait for space. */
    TEST_ASSERT_EQ(block.drops, 0, "BLOCK drops nothing");
    TEST_ASSERT(block.blocked > 0, "BLOCK reports waiting producers");

    TEST_PASS("queue policy overload matrix");
}

void bench_latency_register(void) {
    test_add(test_bench_tail_latency, "bench_tail_latency");
    test_add(test_bench_overload_matrix, "bench_overload_matrix");
}
