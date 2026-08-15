#include "log.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define SLEEP(ms) Sleep(ms)
#define THREAD_FN unsigned __stdcall
#define THREAD_RET return 0;
#else
#include <unistd.h>
#include <pthread.h>
#define SLEEP(ms) usleep((ms)*1000)
#define THREAD_FN void*
#define THREAD_RET return NULL;
#endif

static int g_fails = 0;
static int g_tests = 0;

#define CHECK(cond, name) do { \
  g_tests++; \
  if (cond) printf("[PASS] %s\n", name); \
  else { printf("[FAIL] %s\n", name); g_fails++; } \
} while (0)

static int count_lines(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  int lines = 0, ch;
  while ((ch = fgetc(f)) != EOF) if (ch == '\n') lines++;
  fclose(f);
  return lines;
}

static const char *test_files[] = {
  "t_levels.txt", "t_text.txt", "t_json.txt", "t_custom.txt",
  "t_rot.log", "t_rot.log.1", "t_rot.log.2", "t_rot.log.3", "t_rot.log.4",
  "t_async.txt", "t_afmt.txt", "t_mt_sync.txt", "t_mt_async.txt", "t_null.txt",
  "t_drop.txt", "t_block.txt", "t_fb.txt"
};
static void cleanup_test_files(void) {
  for (size_t i = 0; i < sizeof(test_files) / sizeof(test_files[0]); i++) {
    remove(test_files[i]);
  }
}

/* 1. Level filtering */
static void test_levels(void) {
  FILE *fp = fopen("t_levels.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_WARN);
  ctx->handlers[0].active = false;
  log_ctx_trace(ctx, "hidden trace");
  log_ctx_debug(ctx, "hidden debug");
  log_ctx_info(ctx, "hidden info");
  log_ctx_warn(ctx, "shown warn");
  log_ctx_error(ctx, "shown error");
  fclose(fp);
  log_destroy(ctx);
  char buf[4096] = {0};
  FILE *rf = fopen("t_levels.txt", "r");
  size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
  buf[n] = '\0';
  fclose(rf);
  CHECK(strstr(buf, "hidden trace") == NULL, "level filter drops TRACE");
  CHECK(strstr(buf, "hidden info") == NULL, "level filter drops INFO");
  CHECK(strstr(buf, "shown warn") != NULL, "level filter keeps WARN");
  CHECK(strstr(buf, "shown error") != NULL, "level filter keeps ERROR");
}

/* 2. Text format correctness */
static void test_text_format(void) {
  FILE *fp = fopen("t_text.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_ctx_info(ctx, "payload-%d", 123);
  fclose(fp);
  log_destroy(ctx);
  char buf[4096] = {0};
  FILE *rf = fopen("t_text.txt", "r");
  size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
  buf[n] = '\0';
  fclose(rf);
  CHECK(strstr(buf, "INFO") != NULL, "text line has level");
  CHECK(strstr(buf, "payload-123") != NULL, "text line has formatted message");
}

/* 3. JSON format */
static void test_json(void) {
  FILE *fp = fopen("t_json.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_enable_json_format(ctx);
  log_ctx_info(ctx, "q\"%d", 5);
  fclose(fp);
  log_destroy(ctx);
  char buf[4096] = {0};
  FILE *rf = fopen("t_json.txt", "r");
  size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
  buf[n] = '\0';
  fclose(rf);
  CHECK(strstr(buf, "\"level\": \"INFO\"") != NULL, "json has level");
  CHECK(strstr(buf, "\"message\": \"q\\\"5\"") != NULL, "json escapes quotes");
}

/* 4. Custom formatter */
static int custom_format(log *ctx, log_event *ev, char *buf, size_t buf_size) {
  (void)ctx;
  return snprintf(buf, buf_size, "[CUSTOM:%s] ", log_level_string(ev->level));
}
static void test_custom_format(void) {
  FILE *fp = fopen("t_custom.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_set_format(ctx, custom_format);
  log_ctx_info(ctx, "x");
  fclose(fp);
  log_destroy(ctx);
  char buf[1024] = {0};
  FILE *rf = fopen("t_custom.txt", "r");
  size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
  buf[n] = '\0';
  fclose(rf);
  CHECK(strstr(buf, "[CUSTOM:INFO]") != NULL, "custom prefix applied");
}

/* 5. Rotation */
static void test_rotation(void) {
  FILE *fp = fopen("t_rot.log", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_set_file_prefix(ctx, "t_rot.log");
  log_set_max_file_size(ctx, 1024);
  for (int i = 0; i < 5000; i++) {
    log_ctx_info(ctx, "line %d padding padding padding", i);
  }
  fclose(fp);
  log_destroy(ctx);
  FILE *r1 = fopen("t_rot.log.1", "r");
  CHECK(r1 != NULL, "rotation produced .1");
  if (r1) fclose(r1);
  FILE *r4 = fopen("t_rot.log.4", "r");
  CHECK(r4 != NULL, "rotation kept up to .4");
  if (r4) fclose(r4);
}

/* 6. Async keeps all messages */
static void test_async_no_loss(void) {
  FILE *fp = fopen("t_async.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_set_async(ctx, true);
  for (int i = 0; i < 1000; i++) {
    log_ctx_info(ctx, "a%d", i);
  }
  log_set_async(ctx, false);
  fclose(fp);
  log_destroy(ctx);
  int lines = count_lines("t_async.txt");
  CHECK(lines == 1000, "async writes every message");
}

/* 7. Async format-string safety */
static void test_async_fmt_safety(void) {
  FILE *fp = fopen("t_afmt.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_set_async(ctx, true);
  log_ctx_info(ctx, "100%% done");
  log_set_async(ctx, false);
  fclose(fp);
  log_destroy(ctx);
  char buf[1024] = {0};
  FILE *rf = fopen("t_afmt.txt", "r");
  size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
  buf[n] = '\0';
  fclose(rf);
  CHECK(strstr(buf, "100% done") != NULL, "async does not re-parse format string");
}

/* 8. Multithreaded writes */
static THREAD_FN worker(void *arg) {
  log *ctx = (log *)arg;
  for (int i = 0; i < 2000; i++) {
    log_ctx_info(ctx, "t%d", i);
  }
  THREAD_RET
}
static void run_threads(log *ctx) {
#ifdef _WIN32
  HANDLE th[4];
  for (int i = 0; i < 4; i++)
    th[i] = (HANDLE)_beginthreadex(NULL, 0, worker, ctx, 0, NULL);
  WaitForMultipleObjects(4, th, TRUE, INFINITE);
#else
  pthread_t th[4];
  for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, worker, ctx);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
#endif
}
static void test_multithread(void) {
  FILE *fp = fopen("t_mt_sync.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  run_threads(ctx);
  fclose(fp);
  log_destroy(ctx);
  CHECK(count_lines("t_mt_sync.txt") == 8000, "sync: 4 threads x 2000 written");

  FILE *fp2 = fopen("t_mt_async.txt", "w");
  log *ctx2 = log_create();
  log_add_fp(ctx2, fp2, LOG_INFO);
  ctx2->handlers[0].active = false;
  log_set_async(ctx2, true);
  run_threads(ctx2);
  log_set_async(ctx2, false);
  fclose(fp2);
  log_destroy(ctx2);
  CHECK(count_lines("t_mt_async.txt") == 8000, "async: 4 threads x 2000 written");
}

/* 9. Path traversal rejected */
static void test_path_safety(void) {
  log *ctx = log_create();
  log_set_file_prefix(ctx, "safe.log");
  log_set_file_prefix(ctx, "../evil.log");
  CHECK(strcmp(ctx->file_prefix, "safe.log") == 0, "path traversal rejected");
  log_set_file_prefix(ctx, NULL);
  log_destroy(ctx);
}

/* 10. NULL safety */
static void test_null_safety(void) {
  FILE *fp = fopen("t_null.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  log_ctx_info(ctx, "%s", (char *)NULL);
  fclose(fp);
  log_destroy(ctx);
  log_destroy(NULL);
  CHECK(1, "NULL strings / NULL ctx do not crash");
}

/* 11. Handler capacity (log_add_fp must not overflow the handler array) */
static void test_handler_capacity(void) {
  log *ctx = log_create();
  int idx = -1, rejected = 0, accepted = 0;
  for (int i = 0; i < 64; i++) {
    idx = log_add_fp(ctx, NULL, LOG_INFO);
    if (idx < 0) rejected++;
    else accepted++;
  }
  CHECK(accepted == 31, "31 extra handlers accepted (capacity 32 minus stdout)");
  CHECK(rejected == 33, "further handlers rejected, no overflow");
  log_destroy(ctx);
}

/* 12. Queue policy: DROP (queue full -> message dropped, caller never blocks) */
#define QPOL_N 50000
static void test_queue_drop(void) {
  FILE *fp = fopen("t_drop.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  ctx->queue.max_size = 8;
  log_set_queue_policy(ctx, LOG_QUEUE_DROP);
  log_set_async(ctx, true);
  for (int i = 0; i < QPOL_N; i++) {
    log_ctx_info(ctx, "m%d", i);
  }
  log_set_async(ctx, false);
  fclose(fp);
  log_destroy(ctx);
  int lines = count_lines("t_drop.txt");
  CHECK(lines < QPOL_N, "DROP: overloaded queue loses messages");
  CHECK(lines > 0, "DROP: some messages still delivered");
}

/* 13. Queue policy: BLOCK (queue full -> producer waits, zero loss) */
static void test_queue_block(void) {
  FILE *fp = fopen("t_block.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  ctx->queue.max_size = 8;
  log_set_queue_policy(ctx, LOG_QUEUE_BLOCK);
  log_set_async(ctx, true);
  for (int i = 0; i < QPOL_N; i++) {
    log_ctx_info(ctx, "m%d", i);
  }
  log_set_async(ctx, false);
  log_stats st = {0};
  log_get_stats(ctx, &st);
  fclose(fp);
  log_destroy(ctx);
  CHECK(count_lines("t_block.txt") == QPOL_N, "BLOCK: every message written (zero loss)");
  CHECK(st.queue_drops == 0, "BLOCK: no drops");
  CHECK(st.sync_writes == 0, "BLOCK: no sync fallback");
}

/* 14. Queue policy: FALLBACK_SYNC (default, zero loss via sync writes) */
static void test_queue_fallback(void) {
  FILE *fp = fopen("t_fb.txt", "w");
  log *ctx = log_create();
  log_add_fp(ctx, fp, LOG_INFO);
  ctx->handlers[0].active = false;
  ctx->queue.max_size = 8;
  log_set_async(ctx, true);
  for (int i = 0; i < QPOL_N; i++) {
    log_ctx_info(ctx, "m%d", i);
  }
  log_set_async(ctx, false);
  log_stats st = {0};
  log_get_stats(ctx, &st);
  fclose(fp);
  log_destroy(ctx);
  CHECK(count_lines("t_fb.txt") == QPOL_N, "FALLBACK: zero loss via synchronous writes");
  CHECK(st.sync_writes > 0, "FALLBACK: sync fallback was exercised");
}

int main(void) {
  cleanup_test_files();
  test_levels();
  test_text_format();
  test_json();
  test_custom_format();
  test_rotation();
  test_async_no_loss();
  test_async_fmt_safety();
  test_multithread();
  test_path_safety();
  test_null_safety();
  test_handler_capacity();
  test_queue_drop();
  test_queue_block();
  test_queue_fallback();
  cleanup_test_files();

  printf("\n%d tests, %d failure(s)\n", g_tests, g_fails);
  return g_fails ? 1 : 0;
}
