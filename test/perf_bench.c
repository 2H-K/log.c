#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define SLEEP(ms) Sleep(ms)
#define THREAD_T HANDLE
#define THREAD_CREATE(t, fn, a) ((t) = (HANDLE)_beginthreadex(NULL, 0, (fn), (a), 0, NULL))
#define THREAD_JOIN(t) WaitForSingleObject((t), INFINITE)
static long long NOW(void) {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return t.QuadPart * 1000000 / f.QuadPart;
}
#else
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#define SLEEP(ms) usleep((ms) * 1000)
#define THREAD_T pthread_t
#define THREAD_CREATE(t, fn, a) pthread_create(&(t), NULL, (fn), (a))
#define THREAD_JOIN(t) pthread_join((t), NULL)
static long long NOW(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

#define MSG_LEN 64

typedef struct {
  log *ctx;
  int thread_id;
  int count;
  long long elapsed_us;
} thread_arg;

static void* worker_sync(void *arg) {
  thread_arg *a = (thread_arg*)arg;
  char msg[MSG_LEN];
  memset(msg, 'X', MSG_LEN - 1);
  msg[MSG_LEN - 1] = '\0';

  long long t0 = NOW();
  for (int i = 0; i < a->count; i++) {
    log_ctx_info(a->ctx, "%s idx=%d", msg, i);
  }
  long long t1 = NOW();
  a->elapsed_us = t1 - t0;
  return NULL;
}

static void* worker_async(void *arg) {
  thread_arg *a = (thread_arg*)arg;
  char msg[MSG_LEN];
  memset(msg, 'X', MSG_LEN - 1);
  msg[MSG_LEN - 1] = '\0';

  long long t0 = NOW();
  for (int i = 0; i < a->count; i++) {
    log_ctx_info(a->ctx, "%s idx=%d", msg, i);
  }
  long long t1 = NOW();
  a->elapsed_us = t1 - t0;
  return NULL;
}

static double run_test_sync(int nthreads, int per_thread) {
  FILE *fp = fopen("/tmp/logc_test/perf_out.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;

  THREAD_T threads[32];
  thread_arg args[32];

  long long t0 = NOW();
  for (int i = 0; i < nthreads; i++) {
    args[i].ctx = ctx;
    args[i].thread_id = i;
    args[i].count = per_thread;
    THREAD_CREATE(threads[i], worker_sync, &args[i]);
  }
  for (int i = 0; i < nthreads; i++) {
    THREAD_JOIN(threads[i]);
  }
  long long t1 = NOW();

  fclose(fp);
  log_destroy(ctx);
  return (double)(t1 - t0) / 1000.0;
}

static double run_test_async(int nthreads, int per_thread) {
  FILE *fp = fopen("/tmp/logc_test/perf_out.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_set_async(ctx, true);

  THREAD_T threads[32];
  thread_arg args[32];

  long long t0 = NOW();
  for (int i = 0; i < nthreads; i++) {
    args[i].ctx = ctx;
    args[i].thread_id = i;
    args[i].count = per_thread;
    THREAD_CREATE(threads[i], worker_async, &args[i]);
  }
  for (int i = 0; i < nthreads; i++) {
    THREAD_JOIN(threads[i]);
  }
  long long t1 = NOW();

  log_set_async(ctx, false);
  fclose(fp);
  log_destroy(ctx);
  return (double)(t1 - t0) / 1000.0;
}

static void run_timestamp_test(void) {
  printf("\n=== Timestamp Generation ===\n");
  const int N = 10000000;
  long long t0 = NOW();
  for (int i = 0; i < N; i++) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
  }
  long long t1 = NOW();
  double us_per_call = (double)(t1 - t0) / N;
  printf("  CLOCK_MONOTONIC: %.3f us/call\n", us_per_call);

  t0 = NOW();
  for (int i = 0; i < N; i++) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  }
  t1 = NOW();
  us_per_call = (double)(t1 - t0) / N;
  printf("  CLOCK_MONOTONIC_COARSE: %.3f us/call\n", us_per_call);
}

static void run_format_test(void) {
  printf("\n=== Message Formatting ===\n");
  const int N = 1000000;

  char msg[64];
  memset(msg, 'X', 63);
  msg[63] = '\0';

  long long t0 = NOW();
  for (int i = 0; i < N; i++) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s idx=%d", msg, i);
  }
  long long t1 = NOW();
  double us_per_call = (double)(t1 - t0) / N;
  printf("  snprintf (64-byte msg): %.3f us/call\n", us_per_call);

  char big_msg[4096];
  memset(big_msg, 'Y', 4095);
  big_msg[4095] = '\0';

  t0 = NOW();
  for (int i = 0; i < N; i++) {
    char buf[8192];
    snprintf(buf, sizeof(buf), "%s idx=%d", big_msg, i);
  }
  t1 = NOW();
  us_per_call = (double)(t1 - t0) / N;
  printf("  snprintf (4KB msg): %.3f us/call\n", us_per_call);
}

int main(void) {
  const int TOTAL = 500000;
  int threads[] = {1, 2, 4, 8};
  int nthreads = sizeof(threads) / sizeof(threads[0]);

  printf("=== Performance Benchmark ===\n");
  printf("Total messages: %d\n\n", TOTAL);

  printf("=== Sync Mode ===\n");
  printf("%-8s %-12s %-15s %-12s\n", "Threads", "Time(ms)", "Throughput", "Per-msg(us)");
  printf("%-8s %-12s %-15s %-12s\n", "-------", "------------", "---------------", "------------");
  for (int i = 0; i < nthreads; i++) {
    int nt = threads[i];
    int per_thread = TOTAL / nt;
    double ms = run_test_sync(nt, per_thread);
    double throughput = TOTAL * 1000.0 / ms;
    double per_msg = ms * 1000.0 / TOTAL;
    printf("%-8d %-12.1f %-15.0f %-12.1f\n", nt, ms, throughput, per_msg);
  }

  printf("\n=== Async Mode ===\n");
  printf("%-8s %-12s %-15s %-12s\n", "Threads", "Time(ms)", "Throughput", "Per-msg(us)");
  printf("%-8s %-12s %-15s %-12s\n", "-------", "------------", "---------------", "------------");
  for (int i = 0; i < nthreads; i++) {
    int nt = threads[i];
    int per_thread = TOTAL / nt;
    double ms = run_test_async(nt, per_thread);
    double throughput = TOTAL * 1000.0 / ms;
    double per_msg = ms * 1000.0 / TOTAL;
    printf("%-8d %-12.1f %-15.0f %-12.1f\n", nt, ms, throughput, per_msg);
  }

  run_timestamp_test();
  run_format_test();

  remove("/tmp/logc_test/perf_out.txt");
  return 0;
}
