/**
 * test_memory_mt.c - Concurrent memory-handler dump tests (B1)
 * Platform: All
 *
 * Verifies that dumping the flight recorder while producers keep logging
 * yields consistent snapshots (no torn entries, no reordering).
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

#if LOG_FEATURE_MEMORY_HANDLER

typedef struct {
    log_handle *ctx;
    int count;
    volatile int *done;
} memory_mt_arg;

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI memory_writer(LPVOID arg) {
#else
static void* memory_writer(void *arg) {
#endif
    memory_mt_arg *a = (memory_mt_arg*)arg;
    for (int i = 0; i < a->count; i++) {
        log_ctx_info(a->ctx, "seq-%06d", i);
    }
    *a->done = 1;
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

/* Pull the integer after each "seq-" in order. Returns the count. */
static int snapshot_seqs(const char *buf, int *out, int max) {
    int n = 0;
    const char *p = buf;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        const char *m = strstr(p, "seq-");
        if (m && (!eol || m < eol)) {
            if (n < max) out[n++] = atoi(m + 4);
        }
        if (!eol) break;
        p = eol + 1;
    }
    return n;
}

static void test_memory_dump_concurrent_writes(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;

    int idx = log_add_memory_handler(ctx, 128, LOG_TRACE);
    TEST_ASSERT(idx >= 0, "add memory handler");

    volatile int done = 0;
    memory_mt_arg arg = { ctx, 100000, &done };
    THREAD_T th;
    THREAD_CREATE(th, memory_writer, &arg);

    char buf[65536];
    int dumps = 0;
    int max_seen = 0;
    int samples = 0;
    while (!done && dumps < 500) {
        FILE *out = tmpfile();
        TEST_ASSERT_NOT_NULL(out, "tmpfile");
        log_dump_memory_handler(ctx, idx, out);
        fflush(out);
        rewind(out);
        size_t n = fread(buf, 1, sizeof(buf) - 1, out);
        buf[n] = '\0';
        fclose(out);

        int seqs[256];
        int cnt = snapshot_seqs(buf, seqs, 256);
        if (cnt > max_seen) max_seen = cnt;
        if (cnt > 1) {
            samples++;
            for (int k = 1; k < cnt; k++) {
                /* Single producer logs strictly increasing sequence numbers;
                 * a consistent snapshot must preserve that order. */
                TEST_ASSERT(seqs[k] > seqs[k - 1], "snapshot strictly increasing");
            }
        }
        dumps++;
    }

    THREAD_JOIN(th);
    TEST_ASSERT(dumps > 0, "at least one concurrent dump");
    TEST_ASSERT(samples > 0, "at least one non-trivial snapshot");
    TEST_ASSERT(max_seen <= 128, "snapshot never exceeds capacity");

    log_destroy(ctx);
    TEST_PASS("memory dump concurrent with writes");
}

static void test_memory_multi_producer_smoke(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    ctx->handlers[0].active = false;

    int idx = log_add_memory_handler(ctx, 64, LOG_TRACE);
    TEST_ASSERT(idx >= 0, "add memory handler");

    volatile int done[NUM_THREADS];
    memory_mt_arg args[NUM_THREADS];
    THREAD_T threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        done[i] = 0;
        args[i].ctx = ctx;
        args[i].count = 5000;
        args[i].done = &done[i];
        THREAD_CREATE(threads[i], memory_writer, &args[i]);
    }

    /* Dump concurrently: must never crash or emit a line without the marker. */
    char buf[65536];
    for (int round = 0; round < 50; round++) {
        FILE *out = tmpfile();
        TEST_ASSERT_NOT_NULL(out, "tmpfile");
        log_dump_memory_handler(ctx, idx, out);
        fflush(out);
        rewind(out);
        size_t n = fread(buf, 1, sizeof(buf) - 1, out);
        buf[n] = '\0';
        fclose(out);
        /* Every complete line carries the prefix marker "seq-". */
        const char *p = buf;
        while (p && *p) {
            const char *eol = strchr(p, '\n');
            if (!eol) break;
            if (strstr(p, "seq-") == NULL || strstr(p, "seq-") > eol) {
                TEST_ASSERT(0, "dumped line missing marker");
            }
            p = eol + 1;
        }
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_destroy(ctx);
    TEST_PASS("memory multi-producer smoke");
}

void test_memory_mt_register(void) {
    test_add(test_memory_dump_concurrent_writes, "memory_dump_concurrent");
    test_add(test_memory_multi_producer_smoke, "memory_multi_producer");
}

#else

void test_memory_mt_register(void) {
}

#endif /* LOG_FEATURE_MEMORY_HANDLER */
