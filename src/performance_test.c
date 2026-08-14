#include "log.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define NOW() GetTickCount()
#else
#include <time.h>
static long long NOW(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

#define N 100000

static long long run_sync(int n) {
  FILE *fp = fopen("perf_sync.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  long long t0 = NOW();
  for (int i = 0; i < n; i++) {
    log_ctx_info(ctx, "message %d", i);
  }
  long long dt = NOW() - t0;
  fclose(fp);
  log_destroy(ctx);
  return dt;
}

static long long run_async(int n) {
  FILE *fp = fopen("perf_async.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_set_async(ctx, true);
  long long t0 = NOW();
  for (int i = 0; i < n; i++) {
    log_ctx_info(ctx, "message %d", i);
  }
  log_set_async(ctx, false);
  long long dt = NOW() - t0;
  fclose(fp);
  log_destroy(ctx);
  return dt;
}

int main(void) {
  long long sync_ms = run_sync(N);
  long long async_ms = run_async(N);

  printf("messages: %d\n", N);
  printf("sync  : %lld ms  (%.0f msg/s)\n", sync_ms, N * 1000.0 / (double)sync_ms);
  printf("async : %lld ms  (%.0f msg/s)\n", async_ms, N * 1000.0 / (double)async_ms);

  remove("perf_sync.txt");
  remove("perf_async.txt");
  return 0;
}
