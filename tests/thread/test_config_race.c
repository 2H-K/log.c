/**
 * test_config_race.c - Concurrent configuration change tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI config_changer(LPVOID arg) {
#else
static void* config_changer(void *arg) {
#endif
    log *ctx = (log*)arg;
    for (int i = 0; i < 200; i++) {
        log_set_level(ctx, LOG_TRACE + (i % LOG_LEVELS));
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI concurrent_writer(LPVOID arg) {
#else
static void* concurrent_writer(void *arg) {
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

static void test_config_change_during_write(void) {
    FILE *fp = fopen("/dev/null", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_TRACE);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    THREAD_T writer_threads[NUM_THREADS];
    THREAD_T config_thread;
    thread_arg args[NUM_THREADS];

    THREAD_CREATE(config_thread, config_changer, ctx);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = MSGS_PER_THREAD;
        THREAD_CREATE(writer_threads[i], concurrent_writer, &args[i]);
    }

    THREAD_JOIN(config_thread);
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(writer_threads[i]);
    }

    log_set_level(ctx, LOG_TRACE);
    log_set_async(ctx, false);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("config change during write");
}

static void test_handler_change_during_write(void) {
    FILE *fp = fopen("/dev/null", "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    if (idx >= 0) ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    THREAD_T writer_threads[NUM_THREADS];
    thread_arg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = 500;
        THREAD_CREATE(writer_threads[i], concurrent_writer, &args[i]);
    }

    for (int i = 0; i < 20; i++) {
        FILE *fp2 = fopen("/dev/null", "w");
        int hidx = log_add_fp(ctx, fp2, LOG_DEBUG);
        if (hidx >= 0) {
            log_remove_handler(ctx, hidx);
        }
        fclose(fp2);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(writer_threads[i]);
    }

    log_set_async(ctx, false);
    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("handler change during write");
}

void test_config_race_register(void) {
    test_add(test_config_change_during_write, "config_change_during_write");
    test_add(test_handler_change_during_write, "handler_change_during_write");
}
