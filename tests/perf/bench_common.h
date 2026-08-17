/**
 * bench_common.h - Common definitions for benchmark tests
 * Included by all test files in tests/perf/
 */

#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

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

#define BENCH_N 500000

typedef struct {
    log *ctx;
    int thread_id;
    int count;
    uint64_t elapsed_ns;
} bench_thread_arg;

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI bench_writer(LPVOID arg) {
#else
static void* bench_writer(void *arg) {
#endif
    bench_thread_arg *a = (bench_thread_arg*)arg;
    uint64_t t0 = test_now_ns();
    for (int i = 0; i < a->count; i++) {
        log_ctx_info(a->ctx, "bench msg %d", i);
    }
    a->elapsed_ns = test_now_ns() - t0;
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

#endif /* BENCH_COMMON_H */
