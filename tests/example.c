/**
 * example.c - Demonstrates key features of the log.c library
 *
 * Build:
 *   gcc -std=c11 -Wall -Isrc -o example example.c src/log.c -lpthread
 */

#include "log.h"
#include <stdio.h>
#include <string.h>

/* Custom log formatter: writes only the level and message */
static int custom_formatter(log *ctx, log_event *ev, char *buf, size_t buf_size) {
  (void)ctx;
  return snprintf(buf, buf_size, "[%s] ", log_level_string(ev->level));
}

/* Custom log handler: writes to a memory buffer */
typedef struct {
  char *buffer;
  size_t offset;
  size_t capacity;
} membuf_t;

static void membuf_handler(log *ctx, log_event *ev) {
  (void)ctx;
  membuf_t *mb = (membuf_t *)ev->udata;
  char *msg = NULL;

  /* Format the message */
  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int len = vsnprintf(NULL, 0, ev->fmt, args_copy);
  va_end(args_copy);
  if (len < 0) return;

  msg = malloc((size_t)len + 1);
  if (!msg) return;
  va_list args_copy2;
  va_copy(args_copy2, ev->ap);
  vsnprintf(msg, (size_t)len + 1, ev->fmt, args_copy2);
  va_end(args_copy2);

  if (mb->offset < mb->capacity - 1) {
    int written = snprintf(mb->buffer + mb->offset, mb->capacity - mb->offset,
                           "[%s] %s\n", log_level_string(ev->level), msg);
    if (written > 0) {
      mb->offset += (size_t)written;
    }
  }
  free(msg);
}

int main(void) {
  printf("=== log.c Example ===\n\n");

  /* 1. Basic logging with default context (stderr) */
  printf("--- 1. Basic logging ---\n");
  log_set_level(log_default(), LOG_TRACE);
  log_trace("This is a trace message");
  log_debug("This is a debug message");
  log_info("This is an info message");
  log_warn("This is a warning message");
  log_error("This is an error message");

  /* 2. Custom logger context with file output */
  printf("\n--- 2. File output ---\n");
  log *ctx = log_create();
  log_add_file(ctx, "example_output.log", LOG_DEBUG);
  log_ctx_info(ctx, "Logging to file: example_output.log");
  log_ctx_debug(ctx, "Debug value: %d", 42);
  log_set_file_prefix(ctx, "example_output");
  log_set_max_file_size(ctx, 1024 * 1024); /* 1 MB rotation threshold */
  log_ctx_info(ctx, "File rotation configured at 1 MB");

  /* 3. Thread ID display */
  printf("\n--- 3. Thread ID ---\n");
  int hidx = log_add_fp(ctx, stderr, LOG_INFO);
  log_enable_thread_id(ctx, hidx, true);
  log_ctx_info(ctx, "This message includes thread ID");

  /* 4. JSON formatting */
  printf("\n--- 4. JSON format ---\n");
  log_enable_json_format(ctx);
  log_ctx_info(ctx, "This is a JSON-formatted message with value %d", 123);
  log_enable_text_format(ctx); /* revert to text */

  /* 5. Custom formatter */
  printf("\n--- 5. Custom formatter ---\n");
  log_set_format(ctx, custom_formatter);
  log_ctx_info(ctx, "Custom formatted message");
  log_set_format(ctx, NULL); /* revert to default */

  /* 6. Custom handler (memory buffer) */
  printf("\n--- 6. Custom handler (memory buffer) ---\n");
  static char mem_buffer[4096];
  membuf_t mb = { .buffer = mem_buffer, .offset = 0, .capacity = sizeof(mem_buffer) };
  mem_buffer[0] = '\0';
  int mem_hidx = log_add_handler(ctx, membuf_handler, &mb, LOG_INFO);
  log_ctx_info(ctx, "Captured in memory buffer");
  log_ctx_debug(ctx, "This debug won't appear (level too low)");
  log_remove_handler(ctx, mem_hidx);
  printf("Memory buffer contents:\n%s", mem_buffer);

  /* 7. Performance statistics */
  printf("\n--- 7. Performance stats ---\n");
  log_stats stats;
  log_get_perf_stats(ctx, &stats);
  printf("Total log calls: %llu\n", (unsigned long long)stats.total_count);
  printf("  INFO:  %llu\n", (unsigned long long)stats.level_counts[LOG_INFO]);
  printf("  ERROR: %llu\n", (unsigned long long)stats.level_counts[LOG_ERROR]);
  printf("Sync writes:  %llu\n", (unsigned long long)stats.sync_writes);
  printf("Async writes: %llu\n", (unsigned long long)stats.async_writes);

  /* 8. Async logging */
  printf("\n--- 8. Async logging ---\n");
  log_set_async(ctx, true);
  log_set_queue_policy(ctx, LOG_QUEUE_FALLBACK_SYNC);
  for (int i = 0; i < 5; i++) {
    log_ctx_info(ctx, "Async message %d", i);
  }
  log_set_async(ctx, false); /* flush and stop async thread */
  printf("Async messages written (see example_output.log)");

  /* Cleanup */
  log_destroy(ctx);
  log_destroy(log_default()); /* clean up default context */

  printf("\n=== Done ===\n");
  return 0;
}
