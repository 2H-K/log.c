/**
 * test_thread.h - Common definitions for thread tests
 * Included by all test files in tests/thread/
 */

#ifndef TEST_THREAD_H
#define TEST_THREAD_H

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
#define MSGS_PER_THREAD 1000

typedef struct {
    log *ctx;
    int thread_id;
    int count;
} thread_arg;

#endif /* TEST_THREAD_H */
