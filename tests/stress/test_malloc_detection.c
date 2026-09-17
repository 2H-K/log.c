/**
 * test_malloc_detection.c - Hot-path malloc detection
 * Verifies that the logging hot path does not call malloc.
 *
 * Technique: the test binary is linked with -Wl,--wrap=malloc (GNU ld /
 * LLVM lld, Linux only) so every malloc call in the process routes through
 * __wrap_malloc and the counters are real. The old dlsym/__malloc_hook
 * approach was vacuous: the hook was never installed, so the count was
 * always 0. Windows and non-GNU linkers skip these tests.
 */

#include "test_harness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(LOG_TEST_WRAP_MALLOC) && defined(__linux__)

/* Counter for malloc calls inside measurement windows. */
static size_t g_malloc_count = 0;
static int g_tracking_enabled = 0;

extern void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size) {
    if (g_tracking_enabled) {
        g_malloc_count++;
    }
    return __real_malloc(size);
}

static void start_tracking(void) {
    g_malloc_count = 0;
    g_tracking_enabled = 1;
}

static void stop_tracking(void) {
    g_tracking_enabled = 0;
}

static size_t get_malloc_count(void) {
    return g_malloc_count;
}

/* ==================== Tests ==================== */

static void test_malloc_sync_short_message(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);

    /* Warm up - let any initialization allocations happen */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "warmup");
    }

    /* Now track allocations */
    start_tracking();

    /* Write many short messages */
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "short msg");
    }

    stop_tracking();

    size_t count = get_malloc_count();
    printf("    malloc calls for 10K short messages: %zu", count);

    /* Short messages should use ring buffer (zero malloc) */
    TEST_ASSERT(count == 0, "no malloc for short sync messages");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("malloc sync short message");
}

static void test_malloc_sync_long_message(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);

    /* Warm up */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "warmup");
    }

    start_tracking();

    /* Write long messages (may need allocation for formatting) */
    char long_msg[2048];
    memset(long_msg, 'X', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    for (int i = 0; i < 1000; i++) {
        log_ctx_info(ctx, "%s", long_msg);
    }

    stop_tracking();

    size_t count = get_malloc_count();
    printf("    malloc calls for 1K long messages: %zu", count);

    /* Long messages may require some allocation, but should be minimal */
    /* Allow up to 100 mallocs (1 per 10 messages) */
    TEST_ASSERT(count <= 100, "minimal malloc for long sync messages");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("malloc sync long message");
}

static void test_malloc_async_short_message(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    log_set_async(ctx, true);

    /* Warm up */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "warmup");
    }

    start_tracking();

    /* Write short messages in async mode */
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "async short");
    }

    stop_tracking();

    size_t count = get_malloc_count();
    printf("    malloc calls for 10K async short messages: %zu", count);

    /* Async mode with ring buffer should be zero malloc */
    TEST_ASSERT(count == 0, "no malloc for short async messages");

    log_set_async(ctx, false);
    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("malloc async short message");
}

static void test_malloc_formatter_direct(void) {
    /* Test the formatting function directly */
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);

    /* Warm up */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "warmup %d", i);
    }

    start_tracking();

    /* Write messages with format strings */
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "msg %d %s %f", i, "test", 3.14);
    }

    stop_tracking();

    size_t count = get_malloc_count();
    printf("    malloc calls for 10K formatted messages: %zu", count);

    /* Formatted messages should use stack buffer or ring buffer */
    TEST_ASSERT(count == 0, "no malloc for formatted sync messages");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("malloc formatter direct");
}

typedef struct {
    log_handle *ctx;
    int thread_id;
    int count;
} mt_malloc_arg;

static void *mt_malloc_writer(void *arg) {
    mt_malloc_arg *a = (mt_malloc_arg*)arg;
    for (int j = 0; j < a->count; j++) {
        log_ctx_info(a->ctx, "thread %d msg %d", a->thread_id, j);
    }
    return NULL;
}

static void test_malloc_multithread(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log_handle *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);

    /* Warm up */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "warmup");
    }

    /* Create threads that all log concurrently */
    #define MT_THREADS 4
    #define MT_MSGS 2500

    pthread_t threads[MT_THREADS];
    mt_malloc_arg args[MT_THREADS];

    start_tracking();

    for (int i = 0; i < MT_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = MT_MSGS;
        pthread_create(&threads[i], NULL, mt_malloc_writer, &args[i]);
    }

    for (int i = 0; i < MT_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    stop_tracking();

    size_t count = get_malloc_count();
    printf("    malloc calls for %d threads x %d messages: %zu", MT_THREADS, MT_MSGS, count);

    /* Multi-threaded logging should still be zero malloc on hot path */
    TEST_ASSERT(count <= 10, "minimal malloc for multithreaded logging");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("malloc multithread");
}

void test_malloc_detection_register(void) {
    test_add(test_malloc_sync_short_message, "malloc_sync_short");
    test_add(test_malloc_sync_long_message, "malloc_sync_long");
    test_add(test_malloc_async_short_message, "malloc_async_short");
    test_add(test_malloc_formatter_direct, "malloc_formatter");
    test_add(test_malloc_multithread, "malloc_multithread");
}

#else /* Windows or non-GNU linker - tests skipped */

static void test_malloc_skip(void) {
    TEST_SKIP("malloc detection requires -Wl,--wrap=malloc (Linux + GNU ld)");
}

void test_malloc_detection_register(void) {
    test_add(test_malloc_skip, "malloc_sync_short");
    test_add(test_malloc_skip, "malloc_sync_long");
    test_add(test_malloc_skip, "malloc_async_short");
    test_add(test_malloc_skip, "malloc_formatter");
    test_add(test_malloc_skip, "malloc_multithread");
}

#endif
