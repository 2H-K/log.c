/**
 * bench_durability.c - A4 durability throughput comparison
 * Platform: All
 *
 * Measures synchronous throughput for the three durability tiers:
 *   buffered  = LOG_FLUSH_NEVER  (stdio default)
 *   flushed   = LOG_FLUSH_EVERY  (fflush per message)
 *   fsynced   = LOG_FLUSH_EVERY + fsync (durable per message)
 *
 * Numbers are machine-dependent; the runner prints them for the README table.
 * The assertions only check that every message is present after teardown.
 */

#include "test_harness.h"
#include "log.h"
#include "perf/bench_common.h"

#include <stdlib.h>

static long count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long lines = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') lines++;
    }
    fclose(f);
    return lines;
}

static void run_durability(const char *label, const char *path, int n,
                           int policy, unsigned interval_ms, bool fsync_on) {
    remove(path);
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;

    int idx = log_add_file(ctx, path, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add file handler");
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, idx, policy, interval_ms), 0, "set flush");
    if (fsync_on) {
        TEST_ASSERT_EQ(log_handler_set_fsync(ctx, idx, true), 0, "set fsync");
    }

    uint64_t t0 = test_now_ns();
    for (int i = 0; i < n; i++) {
        log_ctx_info(ctx, "durable %d", i);
    }
    uint64_t dt = test_now_ns() - t0;

    log_destroy(ctx);   /* flushes the owned file */

    long lines = count_lines(path);
    TEST_ASSERT_EQ(lines, n, "all messages persisted after teardown");

    double msg_s = n / (dt / 1e9);
    printf("    %-9s %8.0f msg/s  %7.2f us/msg  (n=%d)", label, msg_s,
           dt / (double)n / 1000.0, n);
    remove(path);
}

static void test_bench_durability(void) {
    /* Buffered: the hot path is dominated by vsnprintf/handler work. */
    run_durability("buffered", "/tmp/bench_durable_buffered.log", 200000,
                   LOG_FLUSH_NEVER, 0, false);
    /* Flushed: one write(2) per message. */
    run_durability("flushed", "/tmp/bench_durable_flushed.log", 50000,
                   LOG_FLUSH_EVERY, 0, false);
    /* Fsynced: one fsync(2) per message (slowest, most durable). */
    run_durability("fsynced", "/tmp/bench_durable_fsynced.log", 2000,
                   LOG_FLUSH_EVERY, 0, true);
    TEST_PASS("bench durability tiers");
}

void bench_durability_register(void) {
    test_add(test_bench_durability, "bench_durability");
}
