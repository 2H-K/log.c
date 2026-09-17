/**
 * test_memory.c - In-memory flight recorder handler tests (B1)
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

#if LOG_FEATURE_MEMORY_HANDLER

/* Dump the memory handler to a temp file, slurp it into buf, return the
 * number of '\n'-terminated lines (or -1 on I/O failure). */
static long dump_to_buf(log_handle *ctx, int idx, char *buf, size_t cap) {
    FILE *out = tmpfile();
    if (!out) return -1;
    log_dump_memory_handler(ctx, idx, out);
    fflush(out);
    rewind(out);
    size_t n = fread(buf, 1, cap - 1, out);
    buf[n] = '\0';
    fclose(out);
    long lines = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') lines++;
    }
    return lines;
}

/* Extract the integer that follows each "msg-" / "seq-" marker, in order. */
static int parse_seqs(const char *buf, const char *marker, int *out, int max) {
    int n = 0;
    const char *p = buf;
    size_t mlen = strlen(marker);
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        const char *m = strstr(p, marker);
        if (m && (!eol || m < eol)) {
            if (n < max) out[n++] = atoi(m + mlen);
        }
        if (!eol) break;
        p = eol + 1;
    }
    return n;
}

static void test_memory_entry_is_pod(void) {
    TEST_ASSERT_EQ(sizeof(log_memory_entry),
                   LOG_MEMORY_LINE_MAX + 2 * sizeof(int),
                   "log_memory_entry stays POD (fixed array + two ints)");
    TEST_PASS("memory entry is POD");
}

static void test_memory_retains_last_n_in_order(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;   /* silence stderr */

    int idx = log_add_memory_handler(ctx, 100, LOG_TRACE);
    TEST_ASSERT(idx >= 0, "add memory handler");

    for (int i = 0; i < 600; i++) {
        log_ctx_info(ctx, "msg-%d", i);
    }

    char buf[65536];
    long lines = dump_to_buf(ctx, idx, buf, sizeof(buf));
    TEST_ASSERT_EQ(lines, 100, "exactly capacity lines retained");
    TEST_ASSERT(strstr(buf, "msg-499") == NULL, "oldest entries evicted");

    int seqs[128];
    int n = parse_seqs(buf, "msg-", seqs, 128);
    TEST_ASSERT_EQ(n, 100, "parsed 100 records");
    for (int k = 0; k < n; k++) {
        TEST_ASSERT_EQ(seqs[k], 500 + k, "records are the last N in order");
    }
    TEST_ASSERT(buf[strlen(buf) - 1] == '\n', "dump ends with newline");

    log_destroy(ctx);
    TEST_PASS("memory retains last N in order");
}

static void test_memory_level_filter(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;

    int idx = log_add_memory_handler(ctx, 16, LOG_WARN);
    TEST_ASSERT(idx >= 0, "add memory handler");

    log_ctx_trace(ctx, "no-trace");
    log_ctx_debug(ctx, "no-debug");
    log_ctx_info(ctx, "no-info");
    log_ctx_warn(ctx, "yes-warn");
    log_ctx_error(ctx, "yes-error");

    char buf[8192];
    long lines = dump_to_buf(ctx, idx, buf, sizeof(buf));
    TEST_ASSERT_EQ(lines, 2, "only warn and error recorded");
    TEST_ASSERT(strstr(buf, "no-info") == NULL, "info filtered out");
    TEST_ASSERT(strstr(buf, "yes-warn") != NULL, "warn recorded");
    TEST_ASSERT(strstr(buf, "yes-error") != NULL, "error recorded");

    log_destroy(ctx);
    TEST_PASS("memory level filter");
}

static void test_memory_async_transport(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;

    int idx = log_add_memory_handler(ctx, 32, LOG_TRACE);
    TEST_ASSERT(idx >= 0, "add memory handler");

    log_set_async(ctx, true);
    for (int i = 0; i < 20; i++) {
        log_ctx_info(ctx, "seq-%d", i);
    }
    log_set_async(ctx, false);   /* drains the queue */

    char buf[16384];
    long lines = dump_to_buf(ctx, idx, buf, sizeof(buf));
    TEST_ASSERT_EQ(lines, 20, "all async records recorded");
    int seqs[32];
    int n = parse_seqs(buf, "seq-", seqs, 32);
    TEST_ASSERT_EQ(n, 20, "parsed 20 records");
    for (int k = 0; k < n; k++) {
        TEST_ASSERT_EQ(seqs[k], k, "async order preserved");
    }

    log_destroy(ctx);
    TEST_PASS("memory async transport");
}

static void test_memory_truncation(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;

    int idx = log_add_memory_handler(ctx, 4, LOG_TRACE);
    TEST_ASSERT(idx >= 0, "add memory handler");

    /* A message far larger than LOG_MEMORY_LINE_MAX. */
    char big[4096];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    log_ctx_info(ctx, "%s", big);

    char buf[8192];
    long lines = dump_to_buf(ctx, idx, buf, sizeof(buf));
    TEST_ASSERT_EQ(lines, 1, "one truncated record");
    TEST_ASSERT(strlen(buf) <= LOG_MEMORY_LINE_MAX, "stored line respects cap");
    TEST_ASSERT(buf[strlen(buf) - 1] == '\n', "truncated line still terminated");

    log_stats st;
    log_get_stats(ctx, &st);
    TEST_ASSERT(st.truncated_count >= 1, "truncation counted in stats");

    log_destroy(ctx);
    TEST_PASS("memory truncation");
}

static void test_memory_null_and_bounds(void) {
    TEST_ASSERT_EQ(log_add_memory_handler(NULL, 8, LOG_INFO), -1, "NULL ctx");
    TEST_ASSERT_EQ(log_add_memory_handler(log_default(), 0, LOG_INFO), -1, "lines=0");
    TEST_ASSERT_EQ(log_add_memory_handler(log_default(), -5, LOG_INFO), -1, "lines<0");

    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;
    int idx = log_add_memory_handler(ctx, 4, LOG_TRACE);
    TEST_ASSERT(idx >= 0, "add memory handler");

    /* Out-of-range / wrong-kind dumps must be no-ops, not crashes. */
    log_dump_memory_handler(ctx, -1, stdout);
    log_dump_memory_handler(ctx, 9999, stdout);
    log_dump_memory_handler(NULL, idx, stdout);
    log_dump_memory_handler(ctx, idx, NULL);
    log_dump_memory_handler(ctx, 0, stdout);   /* index 0 is the stderr handler */

    log_destroy(ctx);
    TEST_PASS("memory null and bounds");
}

void test_memory_register(void) {
    test_add(test_memory_entry_is_pod, "memory_entry_is_pod");
    test_add(test_memory_retains_last_n_in_order, "memory_retains_last_n");
    test_add(test_memory_level_filter, "memory_level_filter");
    test_add(test_memory_async_transport, "memory_async_transport");
    test_add(test_memory_truncation, "memory_truncation");
    test_add(test_memory_null_and_bounds, "memory_null_and_bounds");
}

#else  /* !LOG_FEATURE_MEMORY_HANDLER */

void test_memory_register(void) {
    /* Feature compiled out: nothing to register. */
}

#endif /* LOG_FEATURE_MEMORY_HANDLER */
