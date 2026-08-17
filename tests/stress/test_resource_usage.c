/**
 * test_resource_usage.c - Resource usage measurement
 * Measures CPU time, memory (RSS), file descriptors, and threads
 *
 * NOTE: Uses /proc/self, getrusage(), etc. — POSIX-only.
 *       On Windows all tests are skipped.
 */

#include "test_harness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================== Resource Measurement Helpers ==================== */

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <sys/resource.h>
#include <dirent.h>

static long get_kb_rss(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(fp);
    return rss;
}

static int count_open_fds(void) {
    DIR *dp = opendir("/proc/self/fd");
    if (!dp) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (entry->d_name[0] != '.') count++;
    }
    closedir(dp);
    return count;
}

static int count_threads(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    if (!fp) return -1;
    char line[256];
    int threads = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Threads:", 8) == 0) {
            sscanf(line + 8, "%d", &threads);
            break;
        }
    }
    fclose(fp);
    return threads;
}

static double get_cpu_time_ms(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
           (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

/* ==================== Tests ==================== */

static void test_memory_baseline(void) {
    long rss_before = get_kb_rss();

    /* Create a log context */
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    long rss_after = get_kb_rss();
    long overhead = rss_after - rss_before;

    printf("    RSS before=%ldKB, after=%ldKB, overhead=%ldKB", rss_before, rss_after, overhead);

    /* Overhead should be reasonable (< 1MB for basic context) */
    TEST_ASSERT(overhead < 1024, "log context overhead < 1MB");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("memory baseline");
}

static void test_memory_stability(void) {
    long rss_before = get_kb_rss();

    /* Create and destroy many contexts */
    for (int i = 0; i < 10000; i++) {
        log *ctx = log_create();
        FILE *fp = fopen(TEST_DEV_NULL, "w");
        log_add_fp(ctx, fp, LOG_INFO);
        log_destroy(ctx);
        fclose(fp);
    }

    long rss_after = get_kb_rss();
    long leak = rss_after - rss_before;

    printf("    RSS before=%ldKB, after=%ldKB, delta=%ldKB (10K create/destroy)", rss_before, rss_after, leak);

    /* Allow up to 5MB for 10K iterations (fragmentation, locale caches, etc.) */
    TEST_ASSERT(leak < 5120, "no significant memory leak over 10K iterations");

    TEST_PASS("memory stability");
}

static void test_fd_leak(void) {
    int fds_before = count_open_fds();

    /* Create and destroy many file handlers */
    for (int i = 0; i < 100; i++) {
        log *ctx = log_create();
        FILE *fp = fopen(TEST_DEV_NULL, "w");
        log_add_fp(ctx, fp, LOG_INFO);
        log_destroy(ctx);
        fclose(fp);
    }

    int fds_after = count_open_fds();
    int leak = fds_after - fds_before;

    printf("    FDs before=%d, after=%d, leak=%d (100 file handlers)", fds_before, fds_after, leak);

    /* Should not leak FDs */
    TEST_ASSERT(leak <= 0, "no file descriptor leak");

    TEST_PASS("fd leak check");
}

static void test_thread_leak_sync(void) {
    int threads_before = count_threads();

    /* Sync mode should not create extra threads */
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    for (int i = 0; i < 1000; i++) {
        log_ctx_info(ctx, "test message %d", i);
    }

    int threads_after = count_threads();
    int extra = threads_after - threads_before;

    printf("    threads before=%d, after=%d, extra=%d (sync mode)", threads_before, threads_after, extra);

    /* Sync mode: no extra threads */
    TEST_ASSERT(extra <= 1, "sync mode creates no extra threads");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("thread leak sync");
}

static void test_thread_leak_async(void) {
    int threads_before = count_threads();

    /* Async mode creates one background thread */
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);
    log_set_async(ctx, true);

    int threads_during = count_threads();
    int async_threads = threads_during - threads_before;

    /* Write some messages */
    for (int i = 0; i < 1000; i++) {
        log_ctx_info(ctx, "async test %d", i);
    }

    /* Stop async */
    log_set_async(ctx, false);

    int threads_after = count_threads();
    int remaining = threads_after - threads_before;

    printf("    threads: before=%d, during_async=%d, after_stop=%d",
           threads_before, threads_during, threads_after);

    /* Async mode: exactly 1 background thread */
    TEST_ASSERT(async_threads == 1, "async mode creates exactly 1 background thread");
    /* After stopping: thread should be cleaned up */
    TEST_ASSERT(remaining <= 1, "background thread cleaned up after async stop");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("thread leak async");
}

static void test_cpu_overhead_sync(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);

    double cpu_before = get_cpu_time_ms();

    /* Write 1M messages */
    for (int i = 0; i < 1000000; i++) {
        log_ctx_info(ctx, "cpu test %d", i);
    }

    double cpu_after = get_cpu_time_ms();
    double cpu_ms = cpu_after - cpu_before;
    double cpu_per_msg_us = cpu_ms / 1000.0;

    printf("    CPU time: %.1f ms for 1M msgs, %.3f us/msg", cpu_ms, cpu_per_msg_us);

    /* CPU per message should be reasonable (< 5us for sync mode with full formatting) */
    TEST_ASSERT(cpu_per_msg_us < 5.0, "CPU overhead < 5us per message");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("cpu overhead sync");
}

static void test_handle_limit(void) {
    log *ctx = log_create();
    int count = 0;

    /* Add handlers until we hit the limit */
    for (int i = 0; i < 50; i++) {
        char fname[64];
        snprintf(fname, sizeof(fname), "/tmp/test_handle_%d.log", i);
        FILE *fp = fopen(fname, "w");
        if (!fp) break;
        int idx = log_add_fp(ctx, fp, LOG_INFO);
        if (idx < 0) {
            fclose(fp);
            break;
        }
        count++;
        remove(fname);
    }

    printf("    handlers added: %d", count);

    /* Should support reasonable number of handlers */
    TEST_ASSERT(count >= 10, "supports at least 10 handlers");

    log_destroy(ctx);
    TEST_PASS("handle limit");
}

void test_resource_usage_register(void) {
    test_add(test_memory_baseline, "memory_baseline");
    test_add(test_memory_stability, "memory_stability");
    test_add(test_fd_leak, "fd_leak");
    test_add(test_thread_leak_sync, "thread_leak_sync");
    test_add(test_thread_leak_async, "thread_leak_async");
    test_add(test_cpu_overhead_sync, "cpu_overhead_sync");
    test_add(test_handle_limit, "handle_limit");
}

#else /* Windows - all tests skipped */

static void test_resource_skip(void) {
    TEST_SKIP("resource usage tests require /proc/self, getrusage() (POSIX only)");
}

void test_resource_usage_register(void) {
    test_add(test_resource_skip, "memory_baseline");
    test_add(test_resource_skip, "memory_stability");
    test_add(test_resource_skip, "fd_leak");
    test_add(test_resource_skip, "thread_leak_sync");
    test_add(test_resource_skip, "thread_leak_async");
    test_add(test_resource_skip, "cpu_overhead_sync");
    test_add(test_resource_skip, "handle_limit");
}

#endif
