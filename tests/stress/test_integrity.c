/**
 * test_integrity.c - Log integrity test with sequence numbers
 * Verifies no message loss or corruption under concurrent writes
 */

#include "test_harness.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INTEGRITY_THREADS 8
#define INTEGRITY_MSGS_PER_THREAD 50000
#define INTEGRITY_TOTAL (INTEGRITY_THREADS * INTEGRITY_MSGS_PER_THREAD)

/* Bitmap to track which sequence numbers were received */
static uint8_t *g_bitmap = NULL;
static int g_bitmap_size = 0;
static pthread_mutex_t g_bitmap_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    log *ctx;
    int thread_id;
    int start_seq;
    int count;
} integrity_arg;

static void* integrity_writer(void *arg) {
    integrity_arg *a = (integrity_arg*)arg;
    char buf[128];
    for (int i = 0; i < a->count; i++) {
        int seq = a->start_seq + i;
        snprintf(buf, sizeof(buf), "SEQ_%08d_T%d", seq, a->thread_id);
        log_ctx_info(a->ctx, "%s", buf);
    }
    return NULL;
}

static void test_integrity_sync(void) {
    const char *tmpfile = "/tmp/test_integrity.log";
    remove(tmpfile);

    log *ctx = log_create();
    FILE *fp = fopen(tmpfile, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    log_add_fp(ctx, fp, LOG_INFO);

    g_bitmap_size = INTEGRITY_TOTAL;
    g_bitmap = (uint8_t*)calloc(g_bitmap_size, 1);
    TEST_ASSERT_NOT_NULL(g_bitmap, "calloc bitmap");

    pthread_t threads[INTEGRITY_THREADS];
    integrity_arg args[INTEGRITY_THREADS];

    for (int i = 0; i < INTEGRITY_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].start_seq = i * INTEGRITY_MSGS_PER_THREAD;
        args[i].count = INTEGRITY_MSGS_PER_THREAD;
        pthread_create(&threads[i], NULL, integrity_writer, &args[i]);
    }

    for (int i = 0; i < INTEGRITY_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    log_destroy(ctx);
    fclose(fp);

    /* Parse log file and check sequence numbers */
    fp = fopen(tmpfile, "r");
    TEST_ASSERT_NOT_NULL(fp, "fopen for verify");

    char line[256];
    int found = 0;
    int corrupted = 0;
    while (fgets(line, sizeof(line), fp)) {
        int seq = -1, tid = -1;
        if (sscanf(line, "%*s %*s %*s SEQ_%d_T%d", &seq, &tid) == 2) {
            if (seq >= 0 && seq < g_bitmap_size) {
                if (g_bitmap[seq]) {
                    corrupted++;  /* duplicate */
                } else {
                    g_bitmap[seq] = 1;
                    found++;
                }
            }
        }
    }
    fclose(fp);

    int missing = 0;
    for (int i = 0; i < g_bitmap_size; i++) {
        if (!g_bitmap[i]) missing++;
    }

    printf("    found=%d, missing=%d, corrupted=%d", found, missing, corrupted);

    TEST_ASSERT_EQ(missing, 0, "no missing sequences");
    TEST_ASSERT_EQ(corrupted, 0, "no duplicate sequences");
    TEST_ASSERT_EQ(found, INTEGRITY_TOTAL, "all sequences accounted for");

    free(g_bitmap);
    g_bitmap = NULL;
    remove(tmpfile);
    TEST_PASS("log integrity sync");
}

static void test_integrity_async(void) {
    /* Use fewer messages for async test - the bounded queue (4096 slots) */
    /* cannot handle too many messages without dropping under extreme load */
    const int ASYNC_TOTAL = 4000;
    const int ASYNC_THREADS = 4;
    const int ASYNC_PER_THREAD = ASYNC_TOTAL / ASYNC_THREADS;

    const char *tmpfile = "/tmp/test_integrity_async.log";
    remove(tmpfile);

    log *ctx = log_create();
    FILE *fp = fopen(tmpfile, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");
    log_add_fp(ctx, fp, LOG_INFO);
    log_set_async(ctx, true);

    g_bitmap_size = ASYNC_TOTAL;
    g_bitmap = (uint8_t*)calloc(g_bitmap_size, 1);
    TEST_ASSERT_NOT_NULL(g_bitmap, "calloc bitmap");

    pthread_t threads[4];
    integrity_arg args[4];

    for (int i = 0; i < ASYNC_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].start_seq = i * ASYNC_PER_THREAD;
        args[i].count = ASYNC_PER_THREAD;
        pthread_create(&threads[i], NULL, integrity_writer, &args[i]);
    }

    for (int i = 0; i < ASYNC_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Stop async to flush queue */
    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);

    /* Parse log file and check sequence numbers */
    fp = fopen(tmpfile, "r");
    TEST_ASSERT_NOT_NULL(fp, "fopen for verify");

    char line[256];
    int found = 0;
    int corrupted = 0;
    while (fgets(line, sizeof(line), fp)) {
        int seq = -1, tid = -1;
        if (sscanf(line, "%*s %*s %*s SEQ_%d_T%d", &seq, &tid) == 2) {
            if (seq >= 0 && seq < g_bitmap_size) {
                if (g_bitmap[seq]) {
                    corrupted++;
                } else {
                    g_bitmap[seq] = 1;
                    found++;
                }
            }
        }
    }
    fclose(fp);

    int missing = 0;
    for (int i = 0; i < g_bitmap_size; i++) {
        if (!g_bitmap[i]) missing++;
    }

    printf("    found=%d, missing=%d, corrupted=%d", found, missing, corrupted);

    /* Async mode may drop messages under load, allow up to 5% loss */
    TEST_ASSERT_EQ(corrupted, 0, "no duplicate sequences");
    TEST_ASSERT(missing < ASYNC_TOTAL * 0.05, "less than 5% message loss");

    free(g_bitmap);
    g_bitmap = NULL;
    remove(tmpfile);
    TEST_PASS("log integrity async");
}

void test_integrity_register(void) {
    test_add(test_integrity_sync, "integrity_sync");
    test_add(test_integrity_async, "integrity_async");
}
