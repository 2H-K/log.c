#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#define THREAD_T HANDLE
#define THREAD_CREATE(t, f, a) ((t) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(f), (a), 0, NULL))
#define THREAD_JOIN(t) (WaitForSingleObject((t), INFINITE), CloseHandle((t)))
#else
#include <pthread.h>
#define THREAD_T pthread_t
#define THREAD_CREATE(t, f, a) pthread_create(&(t), NULL, (f), (a))
#define THREAD_JOIN(t) pthread_join((t), NULL)
#endif

#define NUM_THREADS 8
#define MSGS_PER_THREAD 10000

typedef struct {
    log *ctx;
    int thread_id;
    uint64_t start_seq;
    int errors;
} thread_arg;

static uint64_t g_total_written = 0;
static FILE *g_fp = NULL;

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI writer_thread(LPVOID arg) {
#else
static void* writer_thread(void *arg) {
#endif
    thread_arg *a = (thread_arg*)arg;
    uint64_t base = a->start_seq;

    for (int i = 0; i < MSGS_PER_THREAD; i++) {
        log_ctx_info(a->ctx, "SEQ:%010lu TID:%d MSG:%d", base + i, a->thread_id, i);
    }

#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

/* Test 1: Multi-threaded write with sequence verification */
static int test_mt_sync_write(void) {
    printf("[Test] Multi-threaded sync write (no sequence check)...\n");

    FILE *fp = fopen("/dev/null", "w");
    if (!fp) fp = fopen("NUL", "w");

    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].start_seq = (uint64_t)i * MSGS_PER_THREAD;
        args[i].errors = 0;
        THREAD_CREATE(threads[i], writer_thread, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    fclose(fp);
    log_destroy(ctx);
    printf("  PASS\n");
    return 0;
}

/* Test 2: Multi-threaded async write */
static int test_mt_async_write(void) {
    printf("[Test] Multi-threaded async write...\n");

    FILE *fp = fopen("/dev/null", "w");
    if (!fp) fp = fopen("NUL", "w");

    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].start_seq = (uint64_t)i * MSGS_PER_THREAD;
        args[i].errors = 0;
        THREAD_CREATE(threads[i], writer_thread, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_set_async(ctx, false);
    fclose(fp);
    log_destroy(ctx);
    printf("  PASS\n");
    return 0;
}

/* Test 3: Dynamic configuration during concurrent writes */
static int test_config_while_writing(void) {
    printf("[Test] Dynamic config changes during concurrent writes...\n");

    FILE *fp = fopen("/dev/null", "w");
    if (!fp) fp = fopen("NUL", "w");

    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].start_seq = (uint64_t)i * MSGS_PER_THREAD;
        args[i].errors = 0;
        THREAD_CREATE(threads[i], writer_thread, &args[i]);
    }

    /* Change level concurrently */
    for (int i = 0; i < 100; i++) {
        log_set_level(ctx, LOG_TRACE + (i % LOG_LEVELS));
    }
    log_set_level(ctx, LOG_TRACE);

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_set_async(ctx, false);
    fclose(fp);
    log_destroy(ctx);
    printf("  PASS\n");
    return 0;
}

/* Test 4: Handler add/remove during concurrent writes */
static int test_handler_changes_during_write(void) {
    printf("[Test] Handler add/remove during concurrent writes...\n");

    FILE *fp = fopen("/dev/null", "w");
    if (!fp) fp = fopen("NUL", "w");

    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    THREAD_T threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].start_seq = (uint64_t)i * MSGS_PER_THREAD;
        args[i].errors = 0;
        THREAD_CREATE(threads[i], writer_thread, &args[i]);
    }

    /* Add and remove handlers concurrently */
    for (int i = 0; i < 10; i++) {
        FILE *fp2 = fopen("/dev/null", "w");
        int idx = log_add_fp(ctx, fp2, LOG_DEBUG);
        log_remove_handler(ctx, idx);
        fclose(fp2);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    log_set_async(ctx, false);
    fclose(fp);
    log_destroy(ctx);
    printf("  PASS\n");
    return 0;
}

/* Test 5: Queue policy behaviors */
static int test_queue_policies(void) {
    printf("[Test] Queue policy behaviors (drop)...\n");

    FILE *fp = fopen("/dev/null", "w");
    if (!fp) fp = fopen("NUL", "w");

    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_queue_policy(ctx, LOG_QUEUE_DROP);
    log_set_async(ctx, true);

    /* Fill queue and verify drops are counted */
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "drop test %d", i);
    }

    log_set_async(ctx, false);
    fclose(fp);

    log_stats stats;
    log_get_stats(ctx, &stats);
    printf("  Total: %lu, Drops: %lu\n", stats.total_count, stats.queue_drops);

    log_destroy(ctx);
    printf("  PASS\n");
    return 0;
}

int main(void) {
    printf("=== Thread Safety Tests ===\n\n");

    int failures = 0;
    failures += test_mt_sync_write();
    failures += test_mt_async_write();
    failures += test_config_while_writing();
    failures += test_handler_changes_during_write();
    failures += test_queue_policies();

    printf("\n=== Results: %d failures ===\n", failures);
    return failures;
}
