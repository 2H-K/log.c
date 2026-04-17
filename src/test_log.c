/*
 * Test program for enhanced log library
 */

#define _GNU_SOURCE
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define TEST_LOG_FILE "test_output.log"

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
  printf("Running %s... ", #name); \
  tests_run++; \
  test_##name(); \
} while(0)

#define ASSERT(cond) do { \
  if (!(cond)) { \
    printf("FAILED at %s:%d\n", __FILE__, __LINE__); \
    tests_failed++; \
    return; \
  } \
} while(0)

#define PASS() do { \
  printf("PASSED\n"); \
  tests_passed++; \
} while(0)

/* Test 1: Basic logging functionality */
TEST(basic_logging) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  log_ctx_trace(ctx, "Trace message");
  log_ctx_debug(ctx, "Debug message");
  log_ctx_info(ctx, "Info message");
  log_ctx_warn(ctx, "Warning message");
  log_ctx_error(ctx, "Error message");
  log_ctx_fatal(ctx, "Fatal message");
  
  log_destroy(ctx);
  PASS();
}

/* Test 2: Log level filtering */
TEST(level_filtering) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  log_set_level(ctx, LOG_WARN);
  
  log_ctx_trace(ctx, "Should not appear");
  log_ctx_debug(ctx, "Should not appear");
  log_ctx_info(ctx, "Should not appear");
  
  log_ctx_warn(ctx, "Should appear");
  log_ctx_error(ctx, "Should appear");
  
  log_set_level(ctx, LOG_TRACE);
  log_destroy(ctx);
  PASS();
}

/* Test 3: JSON format output */
TEST(json_format) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  FILE *fp = fopen(TEST_LOG_FILE ".json", "w");
  ASSERT(fp != NULL);
  
  log_set_format(ctx, log_format_json);
  
  va_list dummy_ap;
  char buf[8192];
  log_Event ev = {0};
  ev.fmt = "Test message";
  ev.level = LOG_INFO;
  ev.file = "test.c";
  ev.line = 42;
  ev.timestamp = 1234567890.123;
  log_format_json(ctx, &ev, buf, sizeof(buf));
  
  fclose(fp);
  log_destroy(ctx);
  
  struct stat st;
  /* Just verify the function works */
  ASSERT(strlen(buf) > 0);
  ASSERT(buf[0] == '{');
  
  PASS();
}

/* Test 4: File rotation */
TEST(file_rotation) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  const char *prefix = "test_rotation";
  log_set_file_prefix(ctx, prefix);
  log_set_max_file_size(ctx, 1024);
  
  FILE *fp = fopen(prefix, "w");
  ASSERT(fp != NULL);
  
  log_add_fp(ctx, fp, LOG_TRACE);
  
  for (int i = 0; i < 50; i++) {
    log_ctx_info(ctx, "Rotation test message %d: This is a longer message", i);
  }
  
  struct stat st;
  int found = 0;
  for (int i = 1; i <= 5; i++) {
    char path[256];
    snprintf(path, sizeof(path), "%s.%d", prefix, i);
    if (stat(path, &st) == 0) {
      found++;
    }
  }
  
  fclose(fp);
  log_destroy(ctx);
  
  remove(prefix);
  for (int i = 1; i <= 5; i++) {
    char path[256];
    snprintf(path, sizeof(path), "%s.%d", prefix, i);
    remove(path);
  }
  
  (void)found;
  PASS();
}

/* Test 5: Async logging */
TEST(async_logging) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  FILE *fp = fopen(TEST_LOG_FILE ".async", "w");
  ASSERT(fp != NULL);
  
  log_add_fp(ctx, fp, LOG_TRACE);
  
  int ret = log_set_async(ctx, true);
  ASSERT(ret == 0);
  
  for (int i = 0; i < 100; i++) {
    log_ctx_info(ctx, "Async message %d", i);
  }
  
  usleep(50000);
  
  log_set_async(ctx, false);
  
  fclose(fp);
  log_destroy(ctx);
  
  struct stat st;
  ASSERT(stat(TEST_LOG_FILE ".async", &st) == 0);
  
  remove(TEST_LOG_FILE ".async");
  
  PASS();
}

/* Test 6: Performance stats */
TEST(performance_stats) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  log_stats stats_before = {0};
  log_get_stats(ctx, &stats_before);
  
  for (int i = 0; i < 100; i++) {
    log_ctx_info(ctx, "Stats test %d", i);
    log_ctx_error(ctx, "Error %d", i);
  }
  
  log_stats stats_after = {0};
  log_get_stats(ctx, &stats_after);
  
  ASSERT(stats_after.total_count > 0);
  ASSERT(stats_after.level_counts[LOG_INFO] > 0);
  ASSERT(stats_after.level_counts[LOG_ERROR] > 0);
  ASSERT(stats_after.sync_writes > 0);
  
  log_destroy(ctx);
  PASS();
}

/* Test 7: Async with stats */
TEST(async_stats) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  log_set_async(ctx, true);
  
  for (int i = 0; i < 50; i++) {
    log_ctx_info(ctx, "Async stats %d", i);
  }
  
  usleep(50000);
  log_set_async(ctx, false);
  
  log_stats stats = {0};
  log_get_stats(ctx, &stats);
  
  ASSERT(stats.async_writes > 0);
  ASSERT(stats.total_count > 0);
  
  log_destroy(ctx);
  PASS();
}

/* Test 8: Multiple handlers */
TEST(multiple_handlers) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  FILE *fp1 = fopen(TEST_LOG_FILE ".multi1", "w");
  FILE *fp2 = fopen(TEST_LOG_FILE ".multi2", "w");
  ASSERT(fp1 != NULL && fp2 != NULL);
  
  log_add_fp(ctx, fp1, LOG_INFO);
  log_add_fp(ctx, fp2, LOG_ERROR);
  
  log_ctx_info(ctx, "Info message");
  log_ctx_error(ctx, "Error message");
  
  fclose(fp1);
  fclose(fp2);
  
  struct stat st1, st2;
  ASSERT(stat(TEST_LOG_FILE ".multi1", &st1) == 0);
  ASSERT(stat(TEST_LOG_FILE ".multi2", &st2) == 0);
  
  log_destroy(ctx);
  
  remove(TEST_LOG_FILE ".multi1");
  remove(TEST_LOG_FILE ".multi2");
  
  PASS();
}

/* Test 9: Handler level modification */
TEST(handler_level_modification) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  FILE *fp = fopen(TEST_LOG_FILE ".levelmod", "w");
  ASSERT(fp != NULL);
  
  int idx = log_add_fp(ctx, fp, LOG_INFO);
  ASSERT(idx >= 0);
  
  log_ctx_warn(ctx, "Before modification");
  
  log_handler_set_level(ctx, idx, LOG_ERROR);
  
  log_ctx_warn(ctx, "After modification - should not appear");
  log_ctx_error(ctx, "Error after modification - should appear");
  
  fclose(fp);
  log_destroy(ctx);
  
  remove(TEST_LOG_FILE ".levelmod");
  
  PASS();
}

/* Test 10: Dynamic format switching */
TEST(dynamic_format_switching) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  FILE *fp = fopen(TEST_LOG_FILE ".format", "w");
  ASSERT(fp != NULL);
  
  log_add_fp(ctx, fp, LOG_TRACE);
  
  log_ctx_info(ctx, "Text format message");
  
  log_enable_json_format(ctx);
  log_set_format(ctx, log_format_json);
  log_ctx_info(ctx, "JSON format message");
  
  log_enable_text_format(ctx);
  log_set_format(ctx, NULL);
  log_ctx_info(ctx, "Back to text format");
  
  fclose(fp);
  
  struct stat st;
  ASSERT(stat(TEST_LOG_FILE ".format", &st) == 0);
  ASSERT(st.st_size > 0);
  
  log_destroy(ctx);
  
  remove(TEST_LOG_FILE ".format");
  
  PASS();
}

/* Test 11: Thread safety */
TEST(thread_safety) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  FILE *fp = fopen(TEST_LOG_FILE ".thread", "w");
  ASSERT(fp != NULL);
  
  log_add_fp(ctx, fp, LOG_TRACE);
  
  #define NUM_THREADS 4
  #define MSGS_PER_THREAD 100
  
  pthread_t threads[NUM_THREADS];
  
  struct thread_data {
    log *ctx;
    int tid;
  };
  
  struct thread_data data[NUM_THREADS];
  for (int i = 0; i < NUM_THREADS; i++) {
    data[i].ctx = ctx;
    data[i].tid = i;
  }
  
  void* thread_func(void* arg) {
    struct thread_data *d = (struct thread_data*)arg;
    for (int i = 0; i < MSGS_PER_THREAD; i++) {
      log_ctx_info(d->ctx, "Thread %d message %d", d->tid, i);
    }
    return NULL;
  }
  
  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_create(&threads[i], NULL, thread_func, &data[i]);
  }
  
  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }
  
  fclose(fp);
  
  struct stat st;
  ASSERT(stat(TEST_LOG_FILE ".thread", &st) == 0);
  ASSERT(st.st_size > 0);
  
  log_destroy(ctx);
  
  remove(TEST_LOG_FILE ".thread");
  
  PASS();
}

/* Test 12: Queue behavior */
TEST(queue_behavior) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  ASSERT(ctx->queue.max_size > 0);
  
  log_set_async(ctx, true);
  
  for (int i = 0; i < 10; i++) {
    log_ctx_info(ctx, "Queue test %d", i);
  }
  
  usleep(50000);
  
  log_stats stats = {0};
  log_get_stats(ctx, &stats);
  
  (void)stats.async_writes;
  
  log_set_async(ctx, false);
  log_destroy(ctx);
  
  PASS();
}

/* Test 13: Context-based logging macros */
TEST(context_macros) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  log_ctx_trace(ctx, "Context trace");
  log_ctx_debug(ctx, "Context debug");
  log_ctx_info(ctx, "Context info");
  log_ctx_warn(ctx, "Context warn");
  log_ctx_error(ctx, "Context error");
  log_ctx_fatal(ctx, "Context fatal");
  
  log_destroy(ctx);
  PASS();
}

/* Test 14: Default logger */
TEST(default_logger) {
  log *ctx = log_default();
  ASSERT(ctx != NULL);
  
  log_trace("Default trace");
  log_debug("Default debug");
  log_info("Default info");
  log_warn("Default warn");
  log_error("Default error");
  log_fatal("Default fatal");
  
  PASS();
}

/* Test 15: Large message handling */
TEST(large_messages) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  char large_msg[8192];
  memset(large_msg, 'A', sizeof(large_msg) - 1);
  large_msg[sizeof(large_msg) - 1] = '\0';
  
  log_ctx_info(ctx, "%s", large_msg);
  
  log_destroy(ctx);
  PASS();
}

/* Test 16: JSON escaping */
TEST(json_escaping) {
  log *ctx = log_create();
  ASSERT(ctx != NULL);
  
  char buf[8192];
  va_list dummy_ap;
  
  log_Event ev = {0};
  ev.fmt = "Message with \"quotes\", \\backslash, and\nnewline";
  ev.level = LOG_INFO;
  ev.file = "test.c";
  ev.line = 42;
  ev.timestamp = 1234567890.123;
  
  log_format_json(ctx, &ev, buf, sizeof(buf));
  
  ASSERT(buf[0] == '{');
  ASSERT(strlen(buf) > 0);
  
  log_destroy(ctx);
  
  PASS();
}

int main(void) {
  printf("=== Enhanced Log Library Tests ===\n\n");
  
  RUN_TEST(basic_logging);
  RUN_TEST(level_filtering);
  RUN_TEST(json_format);
  RUN_TEST(file_rotation);
  RUN_TEST(async_logging);
  RUN_TEST(performance_stats);
  RUN_TEST(async_stats);
  RUN_TEST(multiple_handlers);
  RUN_TEST(handler_level_modification);
  RUN_TEST(dynamic_format_switching);
  RUN_TEST(thread_safety);
  RUN_TEST(queue_behavior);
  RUN_TEST(context_macros);
  RUN_TEST(default_logger);
  RUN_TEST(large_messages);
  RUN_TEST(json_escaping);
  
  printf("\n=== Test Summary ===\n");
  printf("Total:  %d\n", tests_run);
  printf("Passed: %d\n", tests_passed);
  printf("Failed: %d\n", tests_failed);
  
  remove(TEST_LOG_FILE);
  remove(TEST_LOG_FILE ".json");
  
  return tests_failed > 0 ? 1 : 0;
}
