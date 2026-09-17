/**
 * test_filter.c - Rate limiting & duplicate suppression tests (B3)
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

#if LOG_FEATURE_FILTER

#define FILTER_CAP_TEXT 8192

typedef struct {
    int count;
    int len;
    char text[FILTER_CAP_TEXT];
} filter_capture;

static void filter_capture_fn(log_handle *ctx, log_event *ev) {
    (void)ctx;
    filter_capture *c = (filter_capture*)ev->udata;
    const char *m = ev->raw_msg ? ev->raw_msg : "";
    size_t n = strlen(m);
    c->count++;
    if (c->len + (int)n + 2 < FILTER_CAP_TEXT) {
        memcpy(c->text + c->len, m, n);
        c->len += (int)n;
        c->text[c->len++] = '\n';
        c->text[c->len] = '\0';
    }
}

static void filter_sleep_ms(unsigned ms) {
#if defined(_WIN32) || defined(_WIN64)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Build a context whose only active handler records every emitted line. */
static log_handle* filter_make_ctx(filter_capture *cap) {
    memset(cap, 0, sizeof(*cap));
    log_handle *ctx = log_create();
    if (!ctx) return NULL;
    ctx->handlers[0].active = false;   /* silence stderr */
    if (log_add_handler(ctx, filter_capture_fn, cap, LOG_TRACE) < 0) {
        log_destroy(ctx);
        return NULL;
    }
    return ctx;
}

static void test_filter_rate_limit(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_rate_limit(ctx, LOG_ERROR, 10);
    for (int i = 0; i < 100; i++) log_ctx_error(ctx, "err-%d", i);
    TEST_ASSERT_EQ(cap.count, 10, "at most max_per_sec emitted");

    log_stats st;
    log_get_stats(ctx, &st);
    TEST_ASSERT_EQ(st.suppressed_count, 90, "suppressed messages counted");

    /* Levels without a rule are untouched. */
    for (int i = 0; i < 5; i++) log_ctx_info(ctx, "info-%d", i);
    TEST_ASSERT_EQ(cap.count, 15, "other levels unaffected");

    log_destroy(ctx);
    TEST_PASS("rate limit");
}

static void test_filter_rate_limit_window_reset(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_rate_limit(ctx, LOG_ERROR, 5);
    for (int i = 0; i < 20; i++) log_ctx_error(ctx, "burst1-%d", i);
    TEST_ASSERT_EQ(cap.count, 5, "first window limits to 5");

    filter_sleep_ms(1100);   /* let the one-second window elapse */

    for (int i = 0; i < 20; i++) log_ctx_error(ctx, "burst2-%d", i);
    TEST_ASSERT_EQ(cap.count, 10, "new window resets the counter");
    TEST_ASSERT(cap.text[cap.len - 1] == '\n' || cap.len == 0, "capture sane");

    log_destroy(ctx);
    TEST_PASS("rate limit window reset");
}

static void test_filter_dedupe_summary(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_dedupe(ctx, LOG_INFO, 5000);
    for (int i = 0; i < 5; i++) log_ctx_info(ctx, "same message");
    TEST_ASSERT_EQ(cap.count, 1, "identical messages collapse to one");

    log_stats st;
    log_get_stats(ctx, &st);
    TEST_ASSERT_EQ(st.suppressed_count, 4, "duplicates counted as suppressed");

    log_ctx_info(ctx, "different message");
    TEST_ASSERT_EQ(cap.count, 3, "different message flushes summary then emits");
    TEST_ASSERT(strstr(cap.text, "last message repeated 4 times") != NULL,
                "summary text present");

    log_destroy(ctx);
    TEST_PASS("dedupe summary");
}

static void test_filter_dedupe_window_flush(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_dedupe(ctx, LOG_INFO, 30);
    for (int i = 0; i < 4; i++) log_ctx_info(ctx, "burst");
    TEST_ASSERT_EQ(cap.count, 1, "burst collapses");

    filter_sleep_ms(60);
    log_flush_suppressed(ctx);
    TEST_ASSERT_EQ(cap.count, 2, "flush emits the pending summary");
    TEST_ASSERT(strstr(cap.text, "last message repeated 3 times") != NULL,
                "flushed summary text");

    /* Flushing again with no pending group is a no-op. */
    log_flush_suppressed(ctx);
    TEST_ASSERT_EQ(cap.count, 2, "second flush is a no-op");

    log_destroy(ctx);
    TEST_PASS("dedupe window flush");
}

static void test_filter_dedupe_acceptance(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_dedupe(ctx, LOG_INFO, 5000);
    for (int i = 0; i < 1000; i++) log_ctx_info(ctx, "flood");
    TEST_ASSERT_EQ(cap.count, 1, "1000 duplicates -> one original");

    log_stats st;
    log_get_stats(ctx, &st);
    TEST_ASSERT_EQ(st.suppressed_count, 999, "suppressed_count == 999");

    log_flush_suppressed(ctx);
    TEST_ASSERT_EQ(cap.count, 2, "exactly original + summary");
    TEST_ASSERT(strstr(cap.text, "last message repeated 999 times") != NULL,
                "summary reports 999 repeats");

    log_destroy(ctx);
    TEST_PASS("dedupe acceptance (1000 -> 2 lines)");
}

static void test_filter_dedupe_level_isolation(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_dedupe(ctx, LOG_INFO, 5000);
    log_set_dedupe(ctx, LOG_WARN, 5000);
    for (int i = 0; i < 3; i++) log_ctx_info(ctx, "same text");
    for (int i = 0; i < 3; i++) log_ctx_warn(ctx, "same text");
    /* Two independent groups (one per level): one line each. */
    TEST_ASSERT_EQ(cap.count, 2, "dedupe state is per level");

    log_destroy(ctx);
    TEST_PASS("dedupe level isolation");
}

static void test_filter_disabled_by_zero(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_rate_limit(ctx, LOG_ERROR, 0);
    for (int i = 0; i < 50; i++) log_ctx_error(ctx, "e-%d", i);
    TEST_ASSERT_EQ(cap.count, 50, "zero rate limit disables the rule");

    log_set_dedupe(ctx, LOG_INFO, 0);
    for (int i = 0; i < 50; i++) log_ctx_info(ctx, "i");
    TEST_ASSERT_EQ(cap.count, 100, "zero window disables dedupe");

    log_destroy(ctx);
    TEST_PASS("zero disables filter rules");
}

static void test_filter_null_and_bounds(void) {
    log_set_rate_limit(NULL, LOG_INFO, 1);
    log_set_dedupe(NULL, LOG_INFO, 1);
    log_flush_suppressed(NULL);

    log_handle *ctx = log_default();
    log_set_rate_limit(ctx, -1, 5);
    log_set_rate_limit(ctx, 999, 5);
    log_set_dedupe(ctx, -1, 5);
    log_set_dedupe(ctx, 999, 5);
    log_flush_suppressed(ctx);

    filter_capture cap;
    log_handle *c2 = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(c2, "log_create");
    log_set_rate_limit(c2, LOG_TRACE, 1);
    log_set_dedupe(c2, LOG_FATAL, 1);
    log_flush_suppressed(c2);

    /* Out-of-range levels must be clamped, not index the per-level tables. */
    log_set_dedupe(c2, LOG_INFO, 5000);
    log_ctx_info(c2, "normal");
    log_log(c2, 999, __FILE__, __LINE__, "%s", "too high");
    log_log(c2, -5, __FILE__, __LINE__, "%s", "too low");
    log_flush_suppressed(c2);

    log_destroy(c2);
    TEST_PASS("filter null and bounds");
}

static void test_filter_async_transport(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_dedupe(ctx, LOG_INFO, 5000);
    TEST_ASSERT_EQ(log_set_async(ctx, true), 0, "async on");

    for (int i = 0; i < 10; i++) log_ctx_info(ctx, "async-same");
    log_flush_suppressed(ctx);        /* enqueue summary behind the original */
    log_set_async(ctx, false);        /* drain */

    TEST_ASSERT_EQ(cap.count, 2, "original + summary delivered");
    TEST_ASSERT(strstr(cap.text, "last message repeated 9 times") != NULL,
                "async summary text");
    log_stats st;
    log_get_stats(ctx, &st);
    TEST_ASSERT_EQ(st.suppressed_count, 9, "async suppressions counted");

    log_destroy(ctx);
    TEST_PASS("filter async transport");
}

#if LOG_FEATURE_KV
static void test_filter_dedupe_kv_distinct(void) {
    filter_capture cap;
    log_handle *ctx = filter_make_ctx(&cap);
    TEST_ASSERT_NOT_NULL(ctx, "log_create");

    log_set_dedupe(ctx, LOG_INFO, 5000);
    for (int i = 0; i < 4; i++) {
        log_ctx_info_kv(ctx, "event", LOG_KV_INT("id", i));
    }
    TEST_ASSERT_EQ(cap.count, 4, "distinct kv payloads are not collapsed");

    /* Same body + same kv -> collapsed after the first. */
    for (int i = 0; i < 4; i++) {
        log_ctx_info_kv(ctx, "event", LOG_KV_INT("id", 7));
    }
    TEST_ASSERT_EQ(cap.count, 5, "identical kv collapsed");
    log_flush_suppressed(ctx);
    TEST_ASSERT(strstr(cap.text, "last message repeated 3 times") != NULL,
                "kv dedupe summary");

    log_destroy(ctx);
    TEST_PASS("dedupe kv distinct");
}
#endif /* LOG_FEATURE_KV */

void test_filter_register(void) {
    test_add(test_filter_rate_limit, "filter_rate_limit");
    test_add(test_filter_rate_limit_window_reset, "filter_rate_limit_window");
    test_add(test_filter_dedupe_summary, "filter_dedupe_summary");
    test_add(test_filter_dedupe_window_flush, "filter_dedupe_window_flush");
    test_add(test_filter_dedupe_acceptance, "filter_dedupe_acceptance");
    test_add(test_filter_dedupe_level_isolation, "filter_dedupe_level_isolation");
    test_add(test_filter_disabled_by_zero, "filter_disabled_by_zero");
    test_add(test_filter_null_and_bounds, "filter_null_and_bounds");
    test_add(test_filter_async_transport, "filter_async_transport");
#if LOG_FEATURE_KV
    test_add(test_filter_dedupe_kv_distinct, "filter_dedupe_kv_distinct");
#endif
}

#else  /* !LOG_FEATURE_FILTER */

void test_filter_register(void) {
}

#endif /* LOG_FEATURE_FILTER */
