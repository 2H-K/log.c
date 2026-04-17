/**
 * Copyright (c) 2020 rxi
 * Modified for enhanced features (2026)
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_VERSION "2.0.0"
#define LOG_MAX_QUEUE_SIZE 4096
#define LOG_MAX_ROTATION_FILES 5
#define LOG_DEFAULT_MAX_SIZE (10 * 1024 * 1024)  // 10MB
#define _GNU_SOURCE  /* For strdup */

/* Log levels */
enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL, LOG_LEVELS };

/* Output format modes */
enum { LOG_FORMAT_TEXT, LOG_FORMAT_JSON };

/* Forward declarations */
typedef struct log log;
typedef struct log_event log_Event;
typedef void (*log_LogFn)(log *ctx, log_Event *ev);
typedef void (*log_LockFn)(bool lock, void *udata);

/**
 * Format function that formats the log message into a buffer.
 * @param ctx The logger context
 * @param ev Log event
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @return Number of characters written (excluding null terminator)
 */
typedef int (*log_FormatFn)(log *ctx, log_Event *ev, char *buf, size_t buf_size);

/**
 * @brief Log event structure
 */
struct log_event {
  va_list ap;
  const char *fmt;
  const char *file;
  struct tm *time;
  void *udata;
  int line;
  int level;
  double timestamp;  // High-precision timestamp in seconds
};

/**
 * @brief Logger configuration
 */
typedef struct log_config {
  int level;
  bool quiet;
  int format_mode;           // LOG_FORMAT_TEXT or LOG_FORMAT_JSON
  size_t max_file_size;      // For rotation
  bool async_enabled;
  size_t queue_size;
  const char *file_prefix;   // For log file rotation
  log_FormatFn format_fn;    // Custom format function
} log_config;

/**
 * @brief Performance statistics
 */
typedef struct log_stats {
  uint64_t total_count;
  uint64_t level_counts[LOG_LEVELS];
  uint64_t queue_drops;
  uint64_t rotation_count;
  double avg_queue_latency_ms;
  uint64_t async_writes;
  uint64_t sync_writes;
} log_stats;

/**
 * @brief Log queue entry for async mode
 */
typedef struct log_queue_entry {
  char *message;
  int level;
  char *file;
  int line;
  double timestamp;
  struct log_queue_entry *next;
} log_queue_entry;

/**
 * @brief Log queue structure (lock-free single-producer single-consumer)
 */
typedef struct log_queue {
  _Atomic(log_queue_entry*) head;
  _Atomic(log_queue_entry*) tail;
  atomic_size_t size;
  atomic_size_t high_water_mark;
  size_t max_size;
} log_queue;

/**
 * @brief Reader-writer lock structure (lightweight)
 */
typedef struct log_rwlock {
  _Atomic(int) readers;
  _Atomic(int) writer;
  _Atomic(bool) write_waiting;
} log_rwlock;

/**
 * @brief Output handler
 */
typedef struct log_handler {
  void *udata;
  log_LogFn fn;
  int level;
  bool active;
  FILE *fp;
  char *filename;
  size_t file_size;
} log_handler;
struct log {
  log_rwlock rwlock;
  
  void *udata;
  log_LockFn lock;
  
  /* Config */
  int level;
  bool quiet;
  int format_mode;
  size_t max_file_size;
  
  /* Async */
  bool async_enabled;
  log_queue queue;
  pthread_t async_thread;
  atomic_bool async_running;
  
  /* Handlers */
  log_handler *handlers;
  int handler_count;
  int handler_capacity;
  
  /* Format function */
  log_FormatFn format_fn;
  
  /* Stats */
  log_stats stats;
  atomic_uint_fast64_t last_timestamp;
  
  /* File rotation */
  char *file_prefix;
  
  /* Thread safety */
  pthread_mutex_t mutex;
};

/* Core functions */
log* log_create(void);
void log_destroy(log *ctx);

log* log_default(void);

const char* log_level_string(int level);
void log_set_level(log *ctx, int level);
void log_set_quiet(log *ctx, bool enable);
void log_set_format(log *ctx, log_FormatFn fn);
int log_set_async(log *ctx, bool enable);
void log_set_max_file_size(log *ctx, size_t size);
void log_set_file_prefix(log *ctx, const char *prefix);
int log_add_handler(log *ctx, log_LogFn fn, void *udata, int level);
int log_add_fp(log *ctx, FILE *fp, int level);
void log_remove_handler(log *ctx, int idx);

void log_log(log *ctx, int level, const char *file, int line, const char *fmt, ...);
void log_rotate(log *ctx);

int log_get_stats(log *ctx, log_stats *stats);
const char* log_format_json(log *ctx, log_Event *ev, char *buf, size_t buf_size);

/* Default context macros */
#define log_trace(...) log_log(log_default(), LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_log(log_default(), LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_log(log_default(), LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_log(log_default(), LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(log_default(), LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(log_default(), LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

/* Context-specific macros */
#define log_ctx_trace(ctx, ...) log_log(ctx, LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_ctx_debug(ctx, ...) log_log(ctx, LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_ctx_info(ctx, ...)  log_log(ctx, LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_ctx_warn(ctx, ...)  log_log(ctx, LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_ctx_error(ctx, ...) log_log(ctx, LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_ctx_fatal(ctx, ...) log_log(ctx, LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
