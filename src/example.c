/*
 * Example program demonstrating enhanced log library features
 */

#define _GNU_SOURCE
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

void example_basic_logging(log *ctx) {
  printf("\n=== Basic Logging ===\n");
  
  log_ctx_trace(ctx, "This is a TRACE message");
  log_ctx_debug(ctx, "This is a DEBUG message");
  log_ctx_info(ctx, "This is an INFO message");
  log_ctx_warn(ctx, "This is a WARN message");
  log_ctx_error(ctx, "This is an ERROR message");
  log_ctx_fatal(ctx, "This is a FATAL message");
}

void example_level_filtering(log *ctx) {
  printf("\n=== Level Filtering ===\n");
  
  printf("Setting log level to WARN...\n");
  log_set_level(ctx, LOG_WARN);
  
  log_ctx_trace(ctx, "This TRACE should be filtered");
  log_ctx_debug(ctx, "This DEBUG should be filtered");
  log_ctx_info(ctx, "This INFO should be filtered");
  log_ctx_warn(ctx, "This WARN should appear");
  log_ctx_error(ctx, "This ERROR should appear");
  
  log_set_level(ctx, LOG_TRACE);
}

void example_json_format(log *ctx) {
  printf("\n=== JSON Format Output ===\n");
  
  FILE *fp = fopen("example_json.log", "w");
  if (!fp) return;
  
  log_set_format(ctx, log_format_json);
  log_add_fp(ctx, fp, LOG_TRACE);
  
  log_ctx_info(ctx, "User logged in: user_id=123, ip=192.168.1.1");
  log_ctx_error(ctx, "Database connection failed: error=\"timeout\" retry_count=3");
  log_ctx_warn(ctx, "High memory usage: used=85%%, threshold=80%%");
  
  log_remove_handler(ctx, 1);
  log_set_format(ctx, NULL);
  fclose(fp);
  
  printf("JSON output written to example_json.log\n");
}

void example_file_rotation(log *ctx) {
  printf("\n=== File Rotation ===\n");
  
  log_set_file_prefix(ctx, "example_rotating.log");
  log_set_max_file_size(ctx, 2048);
  
  FILE *fp = fopen("example_rotating.log", "w");
  if (!fp) return;
  
  log_add_fp(ctx, fp, LOG_TRACE);
  
  printf("Writing messages to trigger rotation...\n");
  for (int i = 0; i < 100; i++) {
    log_ctx_info(ctx, "Rotation message %d: This is a longer message to quickly fill up the log file", i);
    if (i % 20 == 0) {
      usleep(1000);
    }
  }
  
  log_remove_handler(ctx, 1);
  fclose(fp);
  
  printf("Check example_rotating.log and example_rotating.log.1 for rotated files\n");
}

void example_async_logging(log *ctx) {
  printf("\n=== Async Logging ===\n");
  
  FILE *fp = fopen("example_async.log", "w");
  if (!fp) return;
  
  log_add_fp(ctx, fp, LOG_TRACE);
  
  printf("Enabling async mode...\n");
  log_set_async(ctx, true);
  
  log_stats stats_before = {0};
  log_get_stats(ctx, &stats_before);
  
  printf("Writing 500 messages in async mode...\n");
  for (int i = 0; i < 500; i++) {
    log_ctx_info(ctx, "Async message %d from thread %lu", i, pthread_self());
  }
  
  usleep(50000);
  
  log_stats stats_after = {0};
  log_get_stats(ctx, &stats_after);
  
  printf("Disabling async mode...\n");
  log_set_async(ctx, false);
  
  printf("Async stats:\n");
  printf("  Total messages: %lu\n", stats_after.total_count - stats_before.total_count);
  printf("  Async writes: %lu\n", stats_after.async_writes - stats_before.async_writes);
  printf("  Queue drops: %lu\n", stats_after.queue_drops);
  printf("  Avg queue latency: %.3f ms\n", stats_after.avg_queue_latency_ms);
  
  log_remove_handler(ctx, 1);
  fclose(fp);
  
  printf("Async output written to example_async.log\n");
}

void example_performance_stats(log *ctx) {
  printf("\n=== Performance Statistics ===\n");
  
  log_stats stats = {0};
  log_get_stats(ctx, &stats);
  
  printf("Current statistics:\n");
  printf("  Total messages: %lu\n", stats.total_count);
  printf("  Level counts:\n");
  printf("    TRACE: %lu\n", stats.level_counts[LOG_TRACE]);
  printf("    DEBUG: %lu\n", stats.level_counts[LOG_DEBUG]);
  printf("    INFO:  %lu\n", stats.level_counts[LOG_INFO]);
  printf("    WARN:  %lu\n", stats.level_counts[LOG_WARN]);
  printf("    ERROR: %lu\n", stats.level_counts[LOG_ERROR]);
  printf("    FATAL: %lu\n", stats.level_counts[LOG_FATAL]);
  printf("  Sync writes: %lu\n", stats.sync_writes);
  printf("  Async writes: %lu\n", stats.async_writes);
  printf("  Rotations: %lu\n", stats.rotation_count);
  printf("  Queue drops: %lu\n", stats.queue_drops);
  printf("  Avg queue latency: %.3f ms\n", stats.avg_queue_latency_ms);
}

void example_thread_safety(log *ctx) {
  printf("\n=== Thread Safety Test ===\n");
  
  FILE *fp = fopen("example_thread.log", "w");
  if (!fp) return;
  
  log_add_fp(ctx, fp, LOG_TRACE);
  
  #define NUM_THREADS 4
  #define MSGS_PER_THREAD 50
  
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
      log_ctx_info(d->ctx, "Thread %d: Message %d", d->tid, i);
    }
    return NULL;
  }
  
  int thread_ids[NUM_THREADS] = {0, 1, 2, 3};
  
  printf("Creating %d threads, each writing %d messages...\n", NUM_THREADS, MSGS_PER_THREAD);
  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_create(&threads[i], NULL, thread_func, &thread_ids[i]);
  }
  
  for (int i = 0; i < NUM_THREADS; i++) {
    pthread_join(threads[i], NULL);
  }
  
  printf("All threads completed\n");
  
  log_remove_handler(ctx, 1);
  fclose(fp);
}

void example_dynamic_handler_config(log *ctx) {
  printf("\n=== Dynamic Handler Configuration ===\n");
  
  FILE *fp1 = fopen("example_dynamic1.log", "w");
  FILE *fp2 = fopen("example_dynamic2.log", "w");
  if (!fp1 || !fp2) return;
  
  int idx1 = log_add_fp(ctx, fp1, LOG_INFO);
  int idx2 = log_add_fp(ctx, fp2, LOG_ERROR);
  
  printf("Initial config:\n");
  printf("  Handler 0: stdout (all levels)\n");
  printf("  Handler 1: file1 (INFO+)\n");
  printf("  Handler 2: file2 (ERROR+)\n");
  
  log_ctx_info(ctx, "Info message - should go to all handlers");
  log_ctx_error(ctx, "Error message - should go to all handlers");
  
  printf("\nModifying handler 1 level to ERROR...\n");
  log_handler_set_level(ctx, idx1, LOG_ERROR);
  
  log_ctx_info(ctx, "Info after modification - should NOT go to handler 1");
  log_ctx_error(ctx, "Error after modification - should go to all");
  
  printf("Modifying format to JSON for subsequent messages...\n");
  log_enable_json_format(ctx);
  log_set_format(ctx, log_format_json);
  
  log_ctx_info(ctx, "This will be formatted as JSON");
  
  log_enable_text_format(ctx);
  log_set_format(ctx, NULL);
  
  log_remove_handler(ctx, 1);
  log_remove_handler(ctx, 1);
  fclose(fp1);
  fclose(fp2);
}

void example_mixed_formatting(log *ctx) {
  printf("\n=== Mixed Formatting ===\n");
  
  FILE *fp_text = fopen("example_text.log", "w");
  FILE *fp_json = fopen("example_json2.log", "w");
  if (!fp_text || !fp_json) return;
  
  log_add_fp(ctx, fp_text, LOG_TRACE);
  log_set_format(ctx, log_format_json);
  log_add_fp(ctx, fp_json, LOG_TRACE);
  
  log_ctx_info(ctx, "This message will be written in both formats");
  log_ctx_warn(ctx, "Warning with special chars: <>&\"'");
  log_ctx_error(ctx, "Error: errno=%d, file=%s", 123, "test.txt");
  
  log_set_format(ctx, NULL);
  log_remove_handler(ctx, 1);
  log_remove_handler(ctx, 1);
  fclose(fp_text);
  fclose(fp_json);
  
  printf("Text output: example_text.log\n");
  printf("JSON output: example_json2.log\n");
}

void example_default_logger(void) {
  printf("\n=== Default Logger (Global) ===\n");
  
  log *default_ctx = log_default();
  
  printf("Using global default logger context\n");
  
  log_trace("Default logger: Trace");
  log_debug("Default logger: Debug");
  log_info("Default logger: Info");
  log_warn("Default logger: Warn");
  log_error("Default logger: Error");
  log_fatal("Default logger: Fatal");
  
  log_destroy(default_ctx);
}

int main(void) {
  printf("======================================================\n");
  printf("     Enhanced Log Library Feature Demonstration       \n");
  printf("                    Version 2.0.0                     \n");
  printf("======================================================\n");
  
  log *ctx = log_create();
  if (!ctx) {
    fprintf(stderr, "Failed to create logger context\n");
    return 1;
  }
  
  printf("\nLogger created successfully\n");
  
  example_basic_logging(ctx);
  example_level_filtering(ctx);
  example_json_format(ctx);
  example_file_rotation(ctx);
  example_async_logging(ctx);
  example_thread_safety(ctx);
  example_dynamic_handler_config(ctx);
  example_mixed_formatting(ctx);
  example_performance_stats(ctx);
  example_default_logger();
  
  printf("\n======================================================\n");
  printf("              Demonstration Complete                  \n");
  printf("======================================================\n");
  
  log_destroy(ctx);
  
  return 0;
}
