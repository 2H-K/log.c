/**
 * test_filter_mt.c - Concurrent rate limit / dedupe tests (B3)
 * Platform: All
 *
 * The filter mutex serialises every producer, so suppression counts must be
 * exact under contention (total = emitted + suppressed).
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

#if LOG_FEATURE_FILTER

typedef struct {
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION mtx;
#else
    pthread_mutex_t mtx;
#endif
    int count;
} mt_capture;

static void mt_capture_fn(log_handle *ctx, log_event *ev) {
    (void)ctx;
    mt_capture *c = (mt_capture*)ev->udata;
#if defined(_WIN32) || defined(_WIN64)
    EnterCriticalSection(&c->mtx);
#else
    pthread_mutex_lock(&c->mtx);
#endif
    c->count++;
#if defined(_WIN32) || defined(_WIN64)
    LeaveCriticalSection(&c->mtx);
#else
    pthread_mutex_unlock(&c->mtx);
#endif
}

typedef struct {
    log_handle *ctx;
    int iters;
} filter_mt_arg;

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI filter_writer(LPVOID arg) {
#else
static void* filter_writer(void *arg) {
#endif
    filter_mt_arg *a = (filter_mt_arg*)arg;
    for (int i = 0; i < a->iters; i++) {
        log_ctx_info(a->ctx, "storm");
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI filter_err_writer(LPVOID arg) {
#else
static void* filter_err_writer(void *arg) {
#endif
    filter_mt_arg *a = (filter_mt_arg*)arg;
    for (int i = 0; i < a->iters; i++) {
        log_ctx_error(a->ctx, "storm-%d", i);
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static log_handle* mt_make_ctx(mt_capture *cap) {
    log_handle *ctx = log_create();
    if (!ctx) return NULL;
    ctx->handlers[0].active = false;
#if defined(_WIN32) || defined(_WIN64)
    InitializeCriticalSection(&cap->mtx);
#else
    pthread_mutex_init(&cap->mtx, NULL);
#endif
    cap->count = 0;
    if (log_add_handler(ctx, mt_capture_fn, cap, LOG_TRACE) < 0) {
        log_destroy(ctx);
        return NULL;
    }
    return ctx;
}

static void mt_destroy_ctx(log_handle *ctx, mt_capture *cap) {
    log_destroy(ctx);
#if defined(_WIN32) || defined(_WIN64)
    DeleteCriticalSection(&cap->mtx);
#else
    pthread_mutex_destroy(&cap->mtx);
#endif
}

static void test_filter_dedupe_mt(void) {
    mt_capture cap;
    log_handle *ctx = mt_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    const int per_thread = 500;
    log_set_dedupe(ctx, LOG_INFO, 60000);

    filter_mt_arg arg = { ctx, per_thread };
    THREAD_T threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_CREATE(threads[i], filter_writer, &arg);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    int total = per_thread * NUM_THREADS;
    TEST_ASSERT_EQ(cap.count, 1, "exactly one of N identical messages emitted");
    log_stats st;
    log_get_stats(ctx, &st);
    TEST_ASSERT_EQ((long)st.suppressed_count, (long)(total - 1),
                   "all other messages suppressed");

    mt_destroy_ctx(ctx, &cap);
    TEST_PASS("dedupe under 8 producers");
}

static void test_filter_rate_limit_mt(void) {
    mt_capture cap;
    log_handle *ctx = mt_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    const int per_thread = 250;
    const unsigned limit = 100;
    log_set_rate_limit(ctx, LOG_ERROR, limit);

    filter_mt_arg arg = { ctx, per_thread };
    THREAD_T threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_CREATE(threads[i], filter_err_writer, &arg);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    int total = per_thread * NUM_THREADS;
    TEST_ASSERT_EQ((long)cap.count, (long)limit,
                   "no more than one window's budget emitted");
    log_stats st;
    log_get_stats(ctx, &st);
    TEST_ASSERT_EQ((long)st.suppressed_count, (long)(total - (int)limit),
                   "remaining messages suppressed");
    TEST_ASSERT_EQ(cap.count + (int)st.suppressed_count, total,
                   "emitted + suppressed == total");

    mt_destroy_ctx(ctx, &cap);
    TEST_PASS("rate limit under 8 producers");
}

void test_filter_mt_register(void) {
    test_add(test_filter_dedupe_mt, "filter_dedupe_mt");
    test_add(test_filter_rate_limit_mt, "filter_rate_limit_mt");
}

#else

void test_filter_mt_register(void) {
}

#endif /* LOG_FEATURE_FILTER */
