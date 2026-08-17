/**
 * bench_async.c - Asynchronous mode throughput benchmarks
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"
#include "perf/bench_common.h"

static void test_bench_async_single(void) {
    FILE *fp = fopen("/dev/null", "w");
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    bench_thread_arg arg = { .ctx = ctx, .count = BENCH_N };
    bench_writer(&arg);

    log_set_async(ctx, false);

    double msg_s = BENCH_N / (arg.elapsed_ns / 1e9);
    double latency_us = arg.elapsed_ns / (double)BENCH_N / 1000.0;
    printf("    %.0f msg/s, %.2f us/msg", msg_s, latency_us);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("bench async single");
}

static void test_bench_async_multi(void) {
    FILE *fp = fopen("/dev/null", "w");
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    #define BENCH_THREADS 8
    THREAD_T threads[BENCH_THREADS];
    bench_thread_arg args[BENCH_THREADS];
    int per_thread = BENCH_N / BENCH_THREADS;

    uint64_t t0 = test_now_ns();
    for (int i = 0; i < BENCH_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = per_thread;
        THREAD_CREATE(threads[i], bench_writer, &args[i]);
    }
    for (int i = 0; i < BENCH_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }
    uint64_t total_ns = test_now_ns() - t0;

    log_set_async(ctx, false);

    double msg_s = BENCH_N / (total_ns / 1e9);
    printf("    %.0f msg/s", msg_s);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("bench async multi");
}

static void test_bench_async_latency(void) {
    FILE *fp = fopen("/dev/null", "w");
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    uint64_t total_ns = 0;
    int samples = 10000;
    for (int i = 0; i < samples; i++) {
        uint64_t t0 = test_now_ns();
        log_ctx_info(ctx, "latency test");
        uint64_t t1 = test_now_ns();
        total_ns += (t1 - t0);
    }

    log_set_async(ctx, false);

    double avg_us = total_ns / (double)samples / 1000.0;
    printf("    %.2f us/msg", avg_us);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("bench async latency");
}

static void test_bench_async_ring_queue(void) {
    FILE *fp = fopen("/dev/null", "w");
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_enable_ring_queue(ctx, true);
    log_set_async(ctx, true);

    bench_thread_arg arg = { .ctx = ctx, .count = BENCH_N };
    bench_writer(&arg);

    log_set_async(ctx, false);

    double msg_s = BENCH_N / (arg.elapsed_ns / 1e9);
    double latency_us = arg.elapsed_ns / (double)BENCH_N / 1000.0;
    printf("    %.0f msg/s, %.2f us/msg", msg_s, latency_us);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("bench async ring queue");
}

void bench_async_register(void) {
    test_add(test_bench_async_single, "bench_async_single");
    test_add(test_bench_async_multi, "bench_async_multi");
    test_add(test_bench_async_latency, "bench_async_latency");
    test_add(test_bench_async_ring_queue, "bench_async_ring_queue");
}
