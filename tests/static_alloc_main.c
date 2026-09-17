/**
 * static_alloc_main.c - Static allocation mode tests (A3 roadmap)
 *
 * Built as its own binary with -DLOG_STATIC_ALLOC and linker wrapping
 * (-Wl,--wrap=malloc/calloc/realloc) so the malloc counters are real, not
 * vacuous. Verifies:
 *   - log_create_static builds a working context inside caller memory
 *   - the logging hot path performs zero heap allocation (sync and async)
 *   - oversized messages are truncated instead of heap-allocated
 *   - log_destroy tears down without freeing the caller's storage, which
 *     can be reused for a fresh context
 *
 * Linux + GNU toolchain only (wrap is a GNU ld feature).
 */

#include "test_harness.h"
#include "log.h"

#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>

/* Wrapped allocator counters (see CMake/Makefile: -Wl,--wrap=malloc). */
static atomic_long g_malloc_calls = 0;
static atomic_long g_calloc_calls = 0;
static atomic_long g_realloc_calls = 0;

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);

void *__wrap_malloc(size_t size) {
    atomic_fetch_add(&g_malloc_calls, 1);
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    atomic_fetch_add(&g_calloc_calls, 1);
    return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    atomic_fetch_add(&g_realloc_calls, 1);
    return __real_realloc(ptr, size);
}

static long wrap_snapshot(void) {
    return atomic_load(&g_malloc_calls) + atomic_load(&g_calloc_calls) +
           atomic_load(&g_realloc_calls);
}

/* One context-sized storage block, reused across tests via create/destroy. */
static log_static_storage_t g_storage;

static long count_occurrences(const char *haystack, const char *needle) {
    long n = 0;
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p++;
    }
    return n;
}

static char *read_file_all(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void test_static_create_destroy_reuse(void) {
    TEST_ASSERT(log_static_ctx_size() > 0, "ctx size positive");
    TEST_ASSERT(log_static_ctx_size() <= sizeof(g_storage), "ctx fits storage");

    /* Undersized buffer must be rejected. */
    char tiny[64];
    TEST_ASSERT(log_create_static(tiny, sizeof(tiny)) == NULL, "undersized rejected");

    log_handle *ctx = log_create_static(&g_storage, sizeof(g_storage));
    TEST_ASSERT_NOT_NULL(ctx, "create_static works");
    log_ctx_info(ctx, "static hello");

    log_destroy(ctx);

    /* Storage must be reusable after teardown. */
    ctx = log_create_static(&g_storage, sizeof(g_storage));
    TEST_ASSERT_NOT_NULL(ctx, "storage reusable");
    log_ctx_info(ctx, "static hello again");
    log_destroy(ctx);
    TEST_PASS("static create/destroy/reuse");
}

static void test_static_hot_path_zero_malloc_sync(void) {
    const char *path = "/tmp/test_static_sync.log";
    remove(path);

    log_handle *ctx = log_create_static(&g_storage, sizeof(g_storage));
    TEST_ASSERT_NOT_NULL(ctx, "create_static");
    ctx->handlers[0].active = false;   /* silence stderr handler */

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    TEST_ASSERT(log_add_fp(ctx, fp, LOG_INFO) >= 0, "add fp handler");

    /* Warmup: tz rules, stdio buffer, lazy inits. */
    for (int i = 0; i < 200; i++) {
        log_ctx_info(ctx, "warmup %d", i);
    }

    long before = wrap_snapshot();
    for (int i = 0; i < 1000; i++) {
        log_ctx_info(ctx, "static sync msg %d", i);
    }
    long delta = wrap_snapshot() - before;

    log_destroy(ctx);
    fclose(fp);

    TEST_ASSERT_EQ(delta, 0, "zero heap allocations for 1000 sync messages");

    char *content = read_file_all(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT_EQ(count_occurrences(content, "static sync msg"), 1000,
                   "all messages written");
    free(content);
    remove(path);
    TEST_PASS("static hot path zero malloc (sync)");
}

static void test_static_hot_path_zero_malloc_async(void) {
    const char *path = "/tmp/test_static_async.log";
    remove(path);

    log_handle *ctx = log_create_static(&g_storage, sizeof(g_storage));
    TEST_ASSERT_NOT_NULL(ctx, "create_static");
    ctx->handlers[0].active = false;

    /* log_add_file owns the file and flushes at destroy. */
    TEST_ASSERT(log_add_file(ctx, path, LOG_INFO) >= 0, "add file handler");
    TEST_ASSERT_EQ(log_set_async(ctx, true), 0, "async on (embedded ring)");

    for (int i = 0; i < 200; i++) {
        log_ctx_info(ctx, "warmup %d", i);
    }

    long before = wrap_snapshot();
    for (int i = 0; i < 1000; i++) {
        log_ctx_info(ctx, "static async msg %d", i);
    }
    long delta = wrap_snapshot() - before;

    TEST_ASSERT_EQ(delta, 0, "zero heap allocations for 1000 async messages");

    log_set_async(ctx, false);   /* drain */
    log_destroy(ctx);            /* flushes owned file */

    char *content = read_file_all(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT_EQ(count_occurrences(content, "static async msg"), 1000,
                   "no message lost in static async mode");
    free(content);
    remove(path);
    TEST_PASS("static hot path zero malloc (async)");
}

static void test_static_sync_truncation(void) {
    const char *path = "/tmp/test_static_trunc.log";
    remove(path);

    log_handle *ctx = log_create_static(&g_storage, sizeof(g_storage));
    TEST_ASSERT_NOT_NULL(ctx, "create_static");
    ctx->handlers[0].active = false;

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    TEST_ASSERT(log_add_fp(ctx, fp, LOG_INFO) >= 0, "add fp handler");

    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "warmup %d", i);
    }

    static char big[20000];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    log_stats stats;
    log_get_stats(ctx, &stats);
    long truncated_before = (long)stats.truncated_count;
    long before = wrap_snapshot();

    log_ctx_info(ctx, "%s", big);

    long delta = wrap_snapshot() - before;
    log_get_stats(ctx, &stats);
    TEST_ASSERT_EQ(delta, 0, "oversized message truncated without malloc");
    TEST_ASSERT((long)stats.truncated_count == truncated_before + 1,
                "truncated_count incremented");

    log_destroy(ctx);
    fclose(fp);

    /* The written line must be truncated, not oversized. */
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(f, "read back");
    char line[8192];
    int max_len = 0;
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        if (len > max_len) max_len = len;
    }
    fclose(f);
    TEST_ASSERT(max_len < 4600, "line truncated to buffer size");
    remove(path);
    TEST_PASS("static sync truncation");
}

static void test_static_async_truncation(void) {
    const char *path = "/tmp/test_static_trunc_async.log";
    remove(path);

    log_handle *ctx = log_create_static(&g_storage, sizeof(g_storage));
    TEST_ASSERT_NOT_NULL(ctx, "create_static");
    ctx->handlers[0].active = false;

    TEST_ASSERT(log_add_file(ctx, path, LOG_INFO) >= 0, "add file handler");
    TEST_ASSERT_EQ(log_set_async(ctx, true), 0, "async on");

    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "warmup %d", i);
    }

    static char big[20000];
    memset(big, 'B', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    log_stats stats;
    log_get_stats(ctx, &stats);
    long truncated_before = (long)stats.truncated_count;
    long before = wrap_snapshot();

    log_ctx_info(ctx, "%s", big);

    long delta = wrap_snapshot() - before;
    TEST_ASSERT_EQ(delta, 0, "oversized async message truncated without malloc");

    log_get_stats(ctx, &stats);
    TEST_ASSERT((long)stats.truncated_count == truncated_before + 1,
                "truncated_count incremented (producer side)");

    log_set_async(ctx, false);
    log_destroy(ctx);

    char *content = read_file_all(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "BBBB") != NULL, "truncated content present");
    free(content);
    remove(path);
    TEST_PASS("static async truncation");
}

int main(void) {
    test_add(test_static_create_destroy_reuse, "static_create_destroy_reuse");
    test_add(test_static_hot_path_zero_malloc_sync, "static_zero_malloc_sync");
    test_add(test_static_hot_path_zero_malloc_async, "static_zero_malloc_async");
    test_add(test_static_sync_truncation, "static_sync_truncation");
    test_add(test_static_async_truncation, "static_async_truncation");
    return test_run_all() ? 1 : 0;
}
