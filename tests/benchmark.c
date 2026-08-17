#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static double now_ns(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart * 1e9;
}
#else
#include <time.h>
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

#define MSG_COUNT 500000
#define THREAD_COUNT 8

static volatile int g_stop = 0;
static uint64_t g_total_messages = 0;

/* ==================== Sync Single Thread ==================== */
static double bench_sync_single(FILE *fp, int n) {
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;

    double t0 = now_ns();
    for (int i = 0; i < n; i++) {
        log_ctx_info(ctx, "benchmark message %d with some data %f", i, 3.14159);
    }
    double t1 = now_ns();

    log_destroy(ctx);
    return t1 - t0;
}

/* ==================== Sync Multi Thread ==================== */
typedef struct {
    log *ctx;
    int thread_id;
    int count;
    double elapsed_ns;
} thread_args;

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI sync_thread_func(LPVOID arg) {
#else
static void* sync_thread_func(void *arg) {
#endif
    thread_args *a = (thread_args*)arg;
    double t0 = now_ns();
    for (int i = 0; i < a->count; i++) {
        log_ctx_info(a->ctx, "thread %d message %d data=%f", a->thread_id, i, 2.71828);
    }
    double t1 = now_ns();
    a->elapsed_ns = t1 - t0;
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static double bench_sync_multi(FILE *fp, int total_n, int nthreads) {
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;

    thread_args args[THREAD_COUNT];
#if defined(_WIN32) || defined(_WIN64)
    HANDLE threads[THREAD_COUNT];
#else
    pthread_t threads[THREAD_COUNT];
#endif

    int per_thread = total_n / nthreads;

    double t0 = now_ns();
    for (int i = 0; i < nthreads; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = per_thread;
        args[i].elapsed_ns = 0;
#if defined(_WIN32) || defined(_WIN64)
        threads[i] = CreateThread(NULL, 0, sync_thread_func, &args[i], 0, NULL);
#else
        pthread_create(&threads[i], NULL, sync_thread_func, &args[i]);
#endif
    }
    for (int i = 0; i < nthreads; i++) {
#if defined(_WIN32) || defined(_WIN64)
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
    double t1 = now_ns();

    log_destroy(ctx);
    return t1 - t0;
}

/* ==================== Async Single Thread ==================== */
static double bench_async_single(FILE *fp, int n) {
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    double t0 = now_ns();
    for (int i = 0; i < n; i++) {
        log_ctx_info(ctx, "async message %d data=%f", i, 1.61803);
    }
    double t1 = now_ns();

    log_set_async(ctx, false);
    log_destroy(ctx);
    return t1 - t0;
}

/* ==================== Async Multi Producer ==================== */
#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI async_thread_func(LPVOID arg) {
#else
static void* async_thread_func(void *arg) {
#endif
    thread_args *a = (thread_args*)arg;
    double t0 = now_ns();
    for (int i = 0; i < a->count; i++) {
        log_ctx_info(a->ctx, "thread %d async msg %d", a->thread_id, i);
    }
    double t1 = now_ns();
    a->elapsed_ns = t1 - t0;
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static double bench_async_multi(FILE *fp, int total_n, int nthreads) {
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_set_async(ctx, true);

    thread_args args[THREAD_COUNT];
#if defined(_WIN32) || defined(_WIN64)
    HANDLE threads[THREAD_COUNT];
#else
    pthread_t threads[THREAD_COUNT];
#endif

    int per_thread = total_n / nthreads;

    double t0 = now_ns();
    for (int i = 0; i < nthreads; i++) {
        args[i].ctx = ctx;
        args[i].thread_id = i;
        args[i].count = per_thread;
        args[i].elapsed_ns = 0;
#if defined(_WIN32) || defined(_WIN64)
        threads[i] = CreateThread(NULL, 0, async_thread_func, &args[i], 0, NULL);
#else
        pthread_create(&threads[i], NULL, async_thread_func, &args[i]);
#endif
    }
    for (int i = 0; i < nthreads; i++) {
#if defined(_WIN32) || defined(_WIN64)
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
    double t1 = now_ns();

    log_set_async(ctx, false);
    log_destroy(ctx);
    return t1 - t0;
}

/* ==================== Async Ring Queue ==================== */
static double bench_async_ring_single(FILE *fp, int n) {
    log *ctx = log_create();
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;
    log_enable_ring_queue(ctx, true);
    log_set_async(ctx, true);

    double t0 = now_ns();
    for (int i = 0; i < n; i++) {
        log_ctx_info(ctx, "ring msg %d data=%f", i, 0.57721);
    }
    double t1 = now_ns();

    log_set_async(ctx, false);
    log_destroy(ctx);
    return t1 - t0;
}

/* ==================== Main ==================== */
int main(void) {
    FILE *fp = fopen("/dev/null", "w");
    if (!fp) fp = fopen("NUL", "w");
    if (!fp) { fprintf(stderr, "Cannot open null device\n"); return 1; }

    printf("=== log.c Performance Benchmark ===\n");
    printf("Messages per test: %d\n", MSG_COUNT);
    printf("Thread count: %d\n\n", THREAD_COUNT);

    /* Warmup */
    bench_sync_single(fp, 1000);

    /* Sync Single */
    double sync_single_ns = bench_sync_single(fp, MSG_COUNT);
    double sync_single_msg_s = MSG_COUNT / (sync_single_ns / 1e9);
    double sync_single_us = sync_single_ns / MSG_COUNT / 1000.0;
    printf("[Sync Single Thread]\n");
    printf("  Total: %.2f ms\n", sync_single_ns / 1e6);
    printf("  Throughput: %.0f msg/s\n", sync_single_msg_s);
    printf("  Latency: %.2f us/msg\n\n", sync_single_us);

    /* Sync Multi */
    double sync_multi_ns = bench_sync_multi(fp, MSG_COUNT, THREAD_COUNT);
    double sync_multi_msg_s = MSG_COUNT / (sync_multi_ns / 1e9);
    printf("[Sync Multi Thread (%d)]\n", THREAD_COUNT);
    printf("  Total: %.2f ms\n", sync_multi_ns / 1e6);
    printf("  Throughput: %.0f msg/s\n", sync_multi_msg_s);
    printf("  vs Single: %.1f%%\n\n", sync_multi_msg_s / sync_single_msg_s * 100);

    /* Async Single */
    double async_single_ns = bench_async_single(fp, MSG_COUNT);
    double async_single_msg_s = MSG_COUNT / (async_single_ns / 1e9);
    double async_single_us = async_single_ns / MSG_COUNT / 1000.0;
    printf("[Async Single Thread (linked queue)]\n");
    printf("  Total: %.2f ms\n", async_single_ns / 1e6);
    printf("  Throughput: %.0f msg/s\n", async_single_msg_s);
    printf("  Latency: %.2f us/msg\n\n", async_single_us);

    /* Async Multi */
    double async_multi_ns = bench_async_multi(fp, MSG_COUNT, THREAD_COUNT);
    double async_multi_msg_s = MSG_COUNT / (async_multi_ns / 1e9);
    printf("[Async Multi Thread (%d) (linked queue)]\n", THREAD_COUNT);
    printf("  Total: %.2f ms\n", async_multi_ns / 1e6);
    printf("  Throughput: %.0f msg/s\n", async_multi_msg_s);
    printf("  vs Single: %.1f%%\n\n", async_multi_msg_s / async_single_msg_s * 100);

    /* Async Ring Single */
    double ring_single_ns = bench_async_ring_single(fp, MSG_COUNT);
    double ring_single_msg_s = MSG_COUNT / (ring_single_ns / 1e9);
    double ring_single_us = ring_single_ns / MSG_COUNT / 1000.0;
    printf("[Async Single Thread (ring queue)]\n");
    printf("  Total: %.2f ms\n", ring_single_ns / 1e6);
    printf("  Throughput: %.0f msg/s\n", ring_single_msg_s);
    printf("  Latency: %.2f us/msg\n\n", ring_single_us);

    /* Summary vs Targets */
    printf("=== Target Comparison ===\n");
    printf("Sync Single:    %.0f msg/s  (target: >= 1,000,000)  %s\n",
           sync_single_msg_s, sync_single_msg_s >= 1e6 ? "PASS" : "FAIL");
    printf("Sync Multi(%d):  %.0f msg/s  (target: >= 500,000)   %s\n",
           THREAD_COUNT, sync_multi_msg_s, sync_multi_msg_s >= 5e5 ? "PASS" : "FAIL");
    printf("Async Single:   %.0f msg/s  (target: >= 2,000,000) %s\n",
           async_single_msg_s, async_single_msg_s >= 2e6 ? "PASS" : "FAIL");
    printf("Async Latency:  %.2f us    (target: < 1 us)       %s\n",
           async_single_us, async_single_us < 1.0 ? "PASS" : "FAIL");

    fclose(fp);
    return 0;
}
