/**
 * test_mt_async.c - Multi-threaded asynchronous write tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI async_writer(LPVOID arg) {
#else
static void* async_writer(void *arg) {
#endif
    thread_arg *a = (thread_arg*)arg;
    for (int i = 0; i < a->count; i++) {
        log_ctx_info(a->ctx, "async thread %d msg %d", a->thread_id, i);
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static void test_mt_async_basic(void) {
    FILE *fp = fopen("/dev/null", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = MSGS_PER_THREAD;
        THREAD_CREATE(threads[i], async_writer, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    /* Note: Stats are thread-local, so we can't verify exact counts here.
     * The key test is no crash/hang. */
    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("multi-threaded async basic");
}

static void test_mt_async_ring_queue(void) {
    FILE *fp = fopen("/dev/null", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_enable_ring_queue(ctx, true);
    log_set_async(ctx, true);

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = MSGS_PER_THREAD;
        THREAD_CREATE(threads[i], async_writer, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("multi-threaded async ring queue");
}

static void test_mt_async_no_crash_stress(void) {
    FILE *fp = fopen("/dev/null", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_DEBUG);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = 5000;
        THREAD_CREATE(threads[i], async_writer, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("multi-threaded async stress");
}

void test_mt_async_register(void) {
    test_add(test_mt_async_basic, "mt_async_basic");
    test_add(test_mt_async_ring_queue, "mt_async_ring_queue");
    test_add(test_mt_async_no_crash_stress, "mt_async_no_crash_stress");
}
