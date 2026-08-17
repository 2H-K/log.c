#include "log.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP(ms) usleep((ms) * 1000)
#endif

static int custom_format(log *ctx, log_event *ev, char *buf, size_t buf_size) {
  (void)ctx;
  return snprintf(buf, buf_size, "[%s] %s:%d - ", log_level_string(ev->level),
                  ev->file ? ev->file : "", ev->line);
}

int main(void) {
  log *ctx = log_create();
  if (!ctx) return 1;

  /* Basic logging */
  log_ctx_trace(ctx, "Detailed debug info");
  log_ctx_info(ctx, "Application started");
  log_ctx_error(ctx, "Failed to connect: %s", "timeout");

  /* Level filtering */
  log_set_level(ctx, LOG_WARN);
  log_ctx_info(ctx, "This won't appear");
  log_ctx_warn(ctx, "This appears");

  /* JSON format */
  log_enable_json_format(ctx);
  log_ctx_info(ctx, "User login: id=%d", 42);
  log_enable_text_format(ctx);

  /* Custom format */
  log_set_format(ctx, custom_format);
  log_ctx_info(ctx, "custom prefix");
  log_set_format(ctx, NULL);

  /* File logging with rotation */
  FILE *fp = fopen("example.log", "w");
  if (fp) {
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    log_set_file_prefix(ctx, "example.log");
    log_set_max_file_size(ctx, 10 * 1024 * 1024);
    for (int i = 0; i < 100; i++) {
      log_ctx_info(ctx, "file message %d", i);
    }
    log_remove_handler(ctx, idx);
    fclose(fp);
  }

  /* Async logging */
  log_set_async(ctx, true);
  log_ctx_info(ctx, "async message");
  log_set_async(ctx, false);

  log_destroy(ctx);

  /* Default logger macros */
  log_info("default logger: info");
  log_error("default logger: error");

  return 0;
}
