/*
 * stress_test_v2.c - Corrected comprehensive stress tests for logc
 * Fixes: uses file handlers instead of quiet mode, proper async testing
 */
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>

#define ITERATIONS_1M 1000000
#define ITERATIONS_100K 100000
#define ITERATIONS_10K 10000

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static size_t get_rss_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (size_t)ru.ru_maxrss;
}

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int lines = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) lines++;
    fclose(f);
    return lines;
}

/* ========== Test 1: Single-threaded sync throughput ========== */
static void test_sync_throughput(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_1M; i++) {
        log_ctx_info(ctx, "sync throughput test message number %d with some data %f", i, (double)i * 1.5);
    }
    double elapsed = now_ms() - start;

    printf("[TEST 1] Sync 1M messages: %.1f ms (%.0f msg/ms, %.1f ns/msg)\n",
           elapsed, ITERATIONS_1M / elapsed, (elapsed * 1e6) / ITERATIONS_1M);
    log_destroy(ctx);
}

/* ========== Test 2: Multi-threaded contention (sync) ========== */
#define THREADS 8
#define MSGS_PER_THREAD 50000

static void *sync_worker(void *arg) {
    log *ctx = (log *)arg;
    for (int i = 0; i < MSGS_PER_THREAD; i++) {
        log_ctx_debug(ctx, "thread %lu writing message %d data=%f",
                      (unsigned long)pthread_self(), i, (double)i * 3.14);
    }
    return NULL;
}

static void test_contention_sync(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);

    pthread_t threads[THREADS];
    double start = now_ms();
    for (int i = 0; i < THREADS; i++) {
        pthread_create(&threads[i], NULL, sync_worker, ctx);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    double elapsed = now_ms() - start;
    double total = (double)(THREADS * MSGS_PER_THREAD);
    printf("[TEST 2] Sync %d threads x %d msgs: %.1f ms (%.0f msg/ms)\n",
           THREADS, MSGS_PER_THREAD, elapsed, total / elapsed);
    log_destroy(ctx);
}

/* ========== Test 3: Async throughput with file output ========== */
static void test_async_throughput(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/async_test.log", "w");
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_enable_mpool(ctx, true);

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_1M; i++) {
        log_ctx_info(ctx, "async throughput message %d data=%f", i, (double)i * 2.718);
    }
    /* Allow async thread to flush */
    usleep(2000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 3] Async 1M messages: %.1f ms (%.0f msg/ms), drops=%lu, async_writes=%lu\n",
           elapsed, ITERATIONS_1M / elapsed, stats.queue_drops, stats.async_writes);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 3b] Lines written: %d\n", count_lines("/tmp/async_test.log"));
    unlink("/tmp/async_test.log");
}

/* ========== Test 4: Multi-threaded async with file output ========== */
static void *async_worker(void *arg) {
    log *ctx = (log *)arg;
    for (int i = 0; i < 100000; i++) {
        log_ctx_trace(ctx, "async thread %lu msg %d value=%f extra_data_here",
                      (unsigned long)pthread_self(), i, (double)i);
    }
    return NULL;
}

static void test_contention_async(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/async_contention.log", "w");
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_enable_mpool(ctx, true);

    pthread_t threads[THREADS];
    double start = now_ms();
    for (int i = 0; i < THREADS; i++) {
        pthread_create(&threads[i], NULL, async_worker, ctx);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    usleep(2000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 4] Async %d threads x 100K msgs: %.1f ms (%.0f msg/ms), drops=%lu\n",
           THREADS, elapsed, (double)(THREADS * 100000) / elapsed, stats.queue_drops);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 4b] Lines written: %d\n", count_lines("/tmp/async_contention.log"));
    unlink("/tmp/async_contention.log");
}

/* ========== Test 5: Queue overflow behavior (DROP policy) ========== */
static void test_queue_overflow_drop(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/overflow_drop.log", "w");
    /* Make file writes slow to simulate backend pressure */
    setvbuf(fp, NULL, _IOFBF, 1024);
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_set_queue_size(ctx, 512);
    log_set_queue_policy(ctx, LOG_QUEUE_DROP);

    int sent = 0;
    double start = now_ms();
    /* Flood faster than async can drain */
    for (int i = 0; i < 500000; i++) {
        log_ctx_info(ctx, "overflow drop test message %d with payload data to fill queue quickly iteration", i);
        sent++;
    }
    usleep(3000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 5] Queue overflow (DROP): sent=%d, async_writes=%lu, dropped=%lu, time=%.0f ms\n",
           sent, stats.async_writes, stats.queue_drops, elapsed);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 5b] Lines written: %d\n", count_lines("/tmp/overflow_drop.log"));
    unlink("/tmp/overflow_drop.log");
}

/* ========== Test 6: Queue overflow behavior (BLOCK policy) ========== */
static void test_queue_overflow_block(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/overflow_block.log", "w");
    setvbuf(fp, NULL, _IOFBF, 1024);
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_set_queue_size(ctx, 512);
    log_set_queue_policy(ctx, LOG_QUEUE_BLOCK);

    int sent = 0;
    double start = now_ms();
    for (int i = 0; i < 100000; i++) {
        log_ctx_info(ctx, "block policy message %d ensuring zero loss under pressure with data", i);
        sent++;
    }
    usleep(2000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 6] Queue overflow (BLOCK): sent=%d, async_writes=%lu, dropped=%lu, blocked=%lu, time=%.0f ms\n",
           sent, stats.async_writes, stats.queue_drops, stats.queue_blocked, elapsed);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 6b] Lines written: %d\n", count_lines("/tmp/overflow_block.log"));
    unlink("/tmp/overflow_block.log");
}

/* ========== Test 7: Log rotation stress ========== */
static void test_rotation_stress(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);
    log_set_max_file_size(ctx, 1024 * 32); /* 32KB rotation */
    log_set_file_prefix(ctx, "/tmp/stress_rotate");
    log_add_file(ctx, "/tmp/stress_rotate", LOG_TRACE);

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_100K; i++) {
        log_ctx_info(ctx, "rotation stress test message number %d with enough data to fill files quickly payload=XXXXXXXXXXXXXXXXXXXXXXXXXX", i);
    }
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 7] Rotation stress: 100K msgs, rotations=%lu, time=%.0f ms\n",
           stats.rotation_count, elapsed);

    /* Count total lines across all rotation files (only last N files are kept) */
    int total_lines = 0;
    for (int i = 0; i < 10; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/tmp/stress_rotate.%d", i);
        total_lines += count_lines(path);
        unlink(path);
    }
    total_lines += count_lines("/tmp/stress_rotate");
    unlink("/tmp/stress_rotate");
    printf("[TEST 7b] Total lines across rotation files: %d (rotation keeps last %d files)\n",
           total_lines, LOG_MAX_ROTATION_FILES - 1);
    log_destroy(ctx);
}

/* ========== Test 8: Large message handling ========== */
static void test_large_messages(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);

    /* 4KB message */
    char *big = (char *)malloc(4096);
    memset(big, 'A', 4095);
    big[4095] = '\0';

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_10K; i++) {
        log_ctx_warn(ctx, "large msg %d: %s", i, big);
    }
    double elapsed = now_ms() - start;
    printf("[TEST 8] Large messages (4KB): 10K msgs in %.0f ms (%.0f msg/ms)\n",
           elapsed, ITERATIONS_10K / elapsed);

    /* 16KB message */
    char *huge = (char *)malloc(16384);
    memset(huge, 'B', 16383);
    huge[16383] = '\0';

    start = now_ms();
    for (int i = 0; i < 1000; i++) {
        log_ctx_error(ctx, "huge msg %d: %s", i, huge);
    }
    elapsed = now_ms() - start;
    printf("[TEST 8b] Huge messages (16KB): 1K msgs in %.0f ms\n", elapsed);

    /* 64KB message */
    char *massive = (char *)malloc(65536);
    memset(massive, 'C', 65535);
    massive[65535] = '\0';

    start = now_ms();
    for (int i = 0; i < 100; i++) {
        log_ctx_fatal(ctx, "massive msg %d: %s", i, massive);
    }
    elapsed = now_ms() - start;
    printf("[TEST 8c] Massive messages (64KB): 100 msgs in %.0f ms\n", elapsed);

    free(big);
    free(huge);
    free(massive);
    log_destroy(ctx);
}

/* ========== Test 9: Many handlers performance ========== */
static void test_many_handlers(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);

    /* Add file handlers */
    FILE *fps[10];
    char names[128];
    for (int i = 0; i < 10; i++) {
        snprintf(names, sizeof(names), "/tmp/handler_test_%d.log", i);
        fps[i] = fopen(names, "w");
        log_add_fp(ctx, fps[i], LOG_TRACE);
    }

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_100K; i++) {
        log_ctx_info(ctx, "many handlers test message %d data=%f", i, (double)i);
    }
    double elapsed = now_ms() - start;
    printf("[TEST 9] 10 file handlers: 100K msgs in %.0f ms (%.0f msg/ms)\n",
           elapsed, ITERATIONS_100K / elapsed);

    for (int i = 0; i < 10; i++) {
        fclose(fps[i]);
        snprintf(names, sizeof(names), "/tmp/handler_test_%d.log", i);
        unlink(names);
    }
    log_destroy(ctx);
}

/* ========== Test 10: Timestamp cache effectiveness ========== */
static void test_timestamp_cache(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);
    log_enable_ts_cache(ctx, true);

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_1M; i++) {
        log_ctx_trace(ctx, "ts cache test message %d", i);
    }
    double elapsed = now_ms() - start;
    printf("[TEST 10] Timestamp cache enabled: 1M msgs in %.0f ms (%.0f msg/ms)\n",
           elapsed, ITERATIONS_1M / elapsed);

    /* Without cache */
    log_enable_ts_cache(ctx, false);
    start = now_ms();
    for (int i = 0; i < ITERATIONS_1M; i++) {
        log_ctx_trace(ctx, "no cache test message %d", i);
    }
    double elapsed2 = now_ms() - start;
    printf("[TEST 10b] Timestamp cache disabled: 1M msgs in %.0f ms (%.0f msg/ms)\n",
           elapsed2, ITERATIONS_1M / elapsed2);
    printf("[TEST 10c] Cache speedup: %.2fx\n", elapsed2 / elapsed);

    log_destroy(ctx);
}

/* ========== Test 11: Memory stability under sustained load ========== */
static void test_memory_stability(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);

    size_t rss_before = get_rss_kb();

    /* 10 rounds of 100K messages */
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 100000; i++) {
            log_ctx_info(ctx, "memory stability round %d message %d payload data here", round, i);
        }
        size_t rss_now = get_rss_kb();
        printf("[TEST 11] Round %d: RSS = %zu KB (delta=%+zd KB)\n",
               round, rss_now, (ssize_t)rss_now - (ssize_t)rss_before);
    }
    log_destroy(ctx);
}

/* ========== Test 12: Ring queue mode with file output ========== */
static void test_ring_queue(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/ring_queue.log", "w");
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_enable_ring_queue(ctx, true);

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_1M; i++) {
        log_ctx_info(ctx, "ring queue message %d data=%f", i, (double)i);
    }
    usleep(2000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 12] Ring queue 1M msgs: %.0f ms (%.0f msg/ms), drops=%lu\n",
           elapsed, ITERATIONS_1M / elapsed, stats.queue_drops);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 12b] Lines written: %d\n", count_lines("/tmp/ring_queue.log"));
    unlink("/tmp/ring_queue.log");
}

/* ========== Test 13: Rapid create/destroy cycles ========== */
static void test_create_destroy(void) {
    double start = now_ms();
    for (int i = 0; i < 10000; i++) {
        log *ctx = log_create();
        log_set_quiet(ctx, true);
        log_ctx_info(ctx, "quick create destroy %d", i);
        log_destroy(ctx);
    }
    double elapsed = now_ms() - start;
    printf("[TEST 13] 10K create/destroy cycles: %.0f ms (%.2f ms/cycle)\n",
           elapsed, elapsed / 10000);
}

/* ========== Test 14: Format string edge cases ========== */
static void test_format_edge_cases(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);

    /* NULL format */
    log_ctx_info(ctx, NULL);
    /* Empty string */
    log_ctx_info(ctx, "");
    /* Format with no args */
    log_ctx_info(ctx, "no format specifiers here");
    /* Many format specifiers */
    log_ctx_info(ctx, "%d %f %s %x %o %c %p %e %g %i",
                 42, 3.14, "test", 255, 64, 'Z', (void*)ctx, 1e10, 2.718, -99);
    /* Percent escapes */
    log_ctx_info(ctx, "100%% complete, %%s %%d %%f");
    /* Very long format string */
    char long_fmt[2048];
    memset(long_fmt, 'x', 2047);
    long_fmt[2047] = '\0';
    log_ctx_warn(ctx, "long: %s", long_fmt);
    /* Special chars */
    log_ctx_info(ctx, "special: \t\n\r\"\\");
    /* Unicode */
    log_ctx_info(ctx, "unicode: 你好世界 日本語 한국어");

    printf("[TEST 14] Format edge cases: completed without crash\n");
    log_destroy(ctx);
}

/* ========== Test 15: Concurrent create/destroy with shared default logger ========== */
static void *default_logger_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 50000; i++) {
        log_info("default logger thread %lu msg %d data=%f",
                 (unsigned long)pthread_self(), i, (double)i);
    }
    return NULL;
}

static void test_default_logger_threads(void) {
    /* Redirect stdout to /dev/null for this test */
    FILE *saved = stdout;
    stdout = fopen("/dev/null", "w");

    pthread_t threads[THREADS];
    double start = now_ms();
    for (int i = 0; i < THREADS; i++) {
        pthread_create(&threads[i], NULL, default_logger_worker, NULL);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    double elapsed = now_ms() - start;

    fclose(stdout);
    stdout = saved;

    printf("[TEST 15] Default logger %d threads x 50K: %.0f ms (%.0f msg/ms)\n",
           THREADS, elapsed, (double)(THREADS * 50000) / elapsed);
}

/* ========== Test 16: Async with FALLBACK policy ========== */
static void test_queue_overflow_fallback(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/overflow_fallback.log", "w");
    setvbuf(fp, NULL, _IOFBF, 1024);
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_set_queue_size(ctx, 256);
    log_set_queue_policy(ctx, LOG_QUEUE_FALLBACK_SYNC);

    int sent = 0;
    double start = now_ms();
    for (int i = 0; i < 200000; i++) {
        log_ctx_info(ctx, "fallback test message %d with payload data to fill queue quickly iteration", i);
        sent++;
    }
    usleep(2000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 16] Queue overflow (FALLBACK): sent=%d, async_writes=%lu, sync_writes=%lu, dropped=%lu\n",
           sent, stats.async_writes, stats.sync_writes, stats.queue_drops);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 16b] Lines written: %d\n", count_lines("/tmp/overflow_fallback.log"));
    unlink("/tmp/overflow_fallback.log");
}

/* ========== Test 17: JSON format performance ========== */
static void test_json_format(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_TRACE);
    log_enable_json_format(ctx);

    double start = now_ms();
    for (int i = 0; i < ITERATIONS_100K; i++) {
        log_ctx_info(ctx, "json format test message %d data=%f string=%s", i, (double)i, "test_data");
    }
    double elapsed = now_ms() - start;
    printf("[TEST 17] JSON format 100K msgs: %.0f ms (%.0f msg/ms)\n",
           elapsed, ITERATIONS_100K / elapsed);
    log_destroy(ctx);
}

/* ========== Test 18: Level filtering performance ========== */
static void test_level_filtering(void) {
    log *ctx = log_create();
    log_set_quiet(ctx, true);
    log_set_level(ctx, LOG_WARN); /* Only WARN and above */

    double start = now_ms();
    int filtered = 0;
    for (int i = 0; i < ITERATIONS_1M; i++) {
        log_ctx_trace(ctx, "should be filtered %d", i);
        log_ctx_debug(ctx, "should be filtered %d", i);
        log_ctx_info(ctx, "should be filtered %d", i);
        log_ctx_warn(ctx, "should pass %d", i);
        log_ctx_error(ctx, "should pass %d", i);
        filtered += 3;
    }
    double elapsed = now_ms() - start;
    printf("[TEST 18] Level filtering: 5M msgs (3M filtered) in %.0f ms (%.0f msg/ms)\n",
           elapsed, ITERATIONS_1M * 5 / elapsed);
    printf("[TEST 18b] Filtered messages: %d\n", filtered);
    log_destroy(ctx);
}

/* ========== Test 19: Burst mode - simulate traffic spike ========== */
static void test_burst_mode(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/burst_test.log", "w");
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_set_queue_size(ctx, 4096);

    double start = now_ms();
    /* 10 bursts of 50K messages with 100ms gaps */
    for (int burst = 0; burst < 10; burst++) {
        for (int i = 0; i < 50000; i++) {
            log_ctx_info(ctx, "burst %d message %d data=%f", burst, i, (double)i);
        }
        usleep(100000);
    }
    usleep(2000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 19] Burst mode 10x50K: %.0f ms, drops=%lu, async_writes=%lu\n",
           elapsed, stats.queue_drops, stats.async_writes);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 19b] Lines written: %d\n", count_lines("/tmp/burst_test.log"));
    unlink("/tmp/burst_test.log");
}

/* ========== Test 20: Long-running stability (30 seconds) ========== */
static void *long_running_worker(void *arg) {
    log *ctx = (log *)arg;
    int count = 0;
    while (count < 500000) {
        log_ctx_info(ctx, "long running thread %lu count %d data=%f",
                     (unsigned long)pthread_self(), count, (double)count);
        count++;
        /* Small delay to simulate real workload */
        if (count % 100 == 0) usleep(1);
    }
    return NULL;
}

static void test_long_running(void) {
    log *ctx = log_create();
    log_set_level(ctx, LOG_TRACE);
    FILE *fp = fopen("/tmp/long_running.log", "w");
    log_add_fp(ctx, fp, LOG_TRACE);
    log_set_async(ctx, true);
    log_enable_mpool(ctx, true);

    pthread_t threads[THREADS];
    double start = now_ms();
    for (int i = 0; i < THREADS; i++) {
        pthread_create(&threads[i], NULL, long_running_worker, ctx);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    usleep(2000000);
    double elapsed = now_ms() - start;

    log_stats stats;
    log_get_perf_stats(ctx, &stats);
    printf("[TEST 20] Long running %d threads x 500K: %.0f ms, drops=%lu\n",
           THREADS, elapsed, stats.queue_drops);
    log_destroy(ctx);
    fclose(fp);
    printf("[TEST 20b] Lines written: %d\n", count_lines("/tmp/long_running.log"));
    unlink("/tmp/long_running.log");
}

int main(void) {
    printf("=== logc Stress Test Suite v2 ===\n");
    printf("Environment: %d cores, GCC %d.%d\n\n", sysconf(_SC_NPROCESSORS_ONLN),
           __GNUC__, __GNUC_MINOR__);

    test_sync_throughput();
    test_contention_sync();
    test_async_throughput();
    test_contention_async();
    test_queue_overflow_drop();
    test_queue_overflow_block();
    test_queue_overflow_fallback();
    test_rotation_stress();
    test_large_messages();
    test_many_handlers();
    test_timestamp_cache();
    test_memory_stability();
    test_ring_queue();
    test_create_destroy();
    test_format_edge_cases();
    test_default_logger_threads();
    test_json_format();
    test_level_filtering();
    test_burst_mode();
    test_long_running();

    printf("\n=== All stress tests completed ===\n");
    return 0;
}
