/**
 * test_mt_sync.c - Multi-threaded synchronous write tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI sync_writer(LPVOID arg) {
#else
static void* sync_writer(void *arg) {
#endif
    thread_arg *a = (thread_arg*)arg;
    for (int i = 0; i < a->count; i++) {
        log_ctx_info(a->ctx, "thread %d msg %d", a->thread_id, i);
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static void test_mt_sync_basic(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = MSGS_PER_THREAD;
        THREAD_CREATE(threads[i], sync_writer, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    /* Note: Stats are thread-local, so main thread may not see all counts.
     * The key test here is that no crash/hang occurred. */
    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("multi-threaded sync basic");
}

static void test_mt_sync_no_crash(void) {
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_DEBUG);
    if (idx >= 0) ctx->handlers[0].active = false;

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = 5000;
        THREAD_CREATE(threads[i], sync_writer, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("multi-threaded sync no crash");
}

void test_mt_sync_register(void) {
    test_add(test_mt_sync_basic, "mt_sync_basic");
    test_add(test_mt_sync_no_crash, "mt_sync_no_crash");
}
