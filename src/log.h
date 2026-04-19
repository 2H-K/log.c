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
#include <string.h>

/* Platform detection */
#if defined(_WIN32) || defined(_WIN64)
  #define LOG_PLATFORM_WINDOWS
#else
  #define LOG_PLATFORM_POSIX
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(_MSC_VER)
  #define LOG_USE_STDATOMIC 1
  #include <stdatomic.h>
#elif defined(_MSC_VER)
  #define LOG_USE_MSVC_ATOMIC 1
  #include <windows.h>
  #if _MSC_VER < 1900
    #error "MSVC 2015 or later is required"
  #endif
  #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
  #endif
#else
  #error "C11 or later with stdatomic.h required, or MSVC"
#endif

#if LOG_PLATFORM_POSIX
  #include <pthread.h>
  #include <unistd.h>
  #include <sys/param.h>
  #define LOG_THREAD_T pthread_t
  #define LOG_THREAD_CREATE(t, f, a) pthread_create(&(t), NULL, (f), (a))
  #define LOG_THREAD_JOIN(t) pthread_join((t), NULL)
  #define LOG_THREAD_ID_T unsigned long
  #define LOG_GET_THREAD_ID() ((LOG_THREAD_ID_T)pthread_self())
#endif

#ifdef LOG_PLATFORM_WINDOWS
  #include <windows.h>
  #define LOG_THREAD_T HANDLE
  #define LOG_THREAD_CREATE(t, f, a) ((t) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(f), (a), 0, NULL))
  #define LOG_THREAD_JOIN(t) (WaitForSingleObject((t), INFINITE), CloseHandle((t)))
  #define LOG_THREAD_ID_T unsigned long
  #define LOG_GET_THREAD_ID() ((LOG_THREAD_ID_T)GetCurrentThreadId())
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define LOG_THREAD_LOCAL _Thread_local
#elif defined(_MSC_VER)
  #define LOG_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__)
  #define LOG_THREAD_LOCAL __thread
#else
  #define LOG_THREAD_LOCAL
#endif

#define LOG_VERSION "2.0.1"
#define LOG_MAX_QUEUE_SIZE 4096
#define LOG_MAX_ROTATION_FILES 5
#define LOG_DEFAULT_MAX_SIZE (10 * 1024 * 1024)

enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL, LOG_LEVELS };

#define LOG_SYSLOG_EMERG   0
#define LOG_SYSLOG_ALERT   1
#define LOG_SYSLOG_CRIT    2
#define LOG_SYSLOG_ERR     3
#define LOG_SYSLOG_WARNING 4
#define LOG_SYSLOG_NOTICE  5
#define LOG_SYSLOG_INFO    6
#define LOG_SYSLOG_DEBUG   7

enum { LOG_FORMAT_TEXT, LOG_FORMAT_JSON };

#define LOG_USE_COLOR

#if LOG_PLATFORM_POSIX
  #include <syslog.h>
  #ifdef LOG_EMERG
    #undef LOG_EMERG
  #endif
  #ifdef LOG_ALERT
    #undef LOG_ALERT
  #endif
  #ifdef LOG_CRIT
    #undef LOG_CRIT
  #endif
  #ifdef LOG_ERR
    #undef LOG_ERR
  #endif
  #ifdef LOG_WARNING
    #undef LOG_WARNING
  #endif
  #ifdef LOG_NOTICE
    #undef LOG_NOTICE
  #endif
  #ifdef LOG_INFO
    #undef LOG_INFO
  #endif
  #ifdef LOG_DEBUG
    #undef LOG_DEBUG
  #endif
  #define LOG_HAVE_SYSLOG 1
  #ifndef LOG_USER
    #define LOG_USER 0
  #endif
  #ifndef LOG_PID
    #define LOG_PID 0
  #endif
  #ifndef LOG_NDELAY
    #define LOG_NDELAY 0
  #endif
#else
  #define LOG_HAVE_SYSLOG 0
  #define LOG_USER 0
  #define LOG_PID 0
  #define LOG_NDELAY 0
#endif

/* strdup compatibility */
#ifdef _MSC_VER
  #define strdup _strdup
#endif

#ifdef __cplusplus
extern "C" {
#endif

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
  double timestamp;  /* High-precision timestamp in seconds */
};

/**
 * @brief Logger configuration
 */
typedef struct log_config {
  int level;
  bool quiet;
  int format_mode;           /* LOG_FORMAT_TEXT or LOG_FORMAT_JSON */
  size_t max_file_size;      /* For rotation */
  bool async_enabled;
  size_t queue_size;
  const char *file_prefix;   /* For log file rotation */
  log_FormatFn format_fn;    /* Custom format function */
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
 * @brief Memory pool for log entries (reduces malloc/free overhead)
 */
typedef struct log_mpool {
  log_queue_entry *free_list;
  size_t allocated;
  size_t max_size;
  size_t chunk_count;
} log_mpool;

/**
 * @brief Fixed-size buffer for thread-local formatting (avoids stack allocation)
 */
typedef struct log_thread_buffer {
  char format_buf[4096];
  char time_buf[64];
  size_t fmt_offset;
  size_t time_offset;
} log_thread_buffer;

/**
 * @brief Timestamp cache to avoid repeated formatting
 */
typedef struct log_ts_cache {
  double last_timestamp;
  char cached_string[32];
  unsigned int cache_hits;
  unsigned int cache_misses;
} log_ts_cache;

/**
 * @brief Log queue structure (lock-free single-producer single-consumer)
 */
typedef struct log_queue {
#ifdef LOG_USE_STDATOMIC
  _Atomic(log_queue_entry*) head;
  _Atomic(log_queue_entry*) tail;
  atomic_size_t size;
  atomic_size_t high_water_mark;
#else
  /* MSVC atomic pointers using InterlockedXxx functions */
  log_queue_entry* volatile head;
  log_queue_entry* volatile tail;
  volatile size_t size;
  volatile size_t high_water_mark;
#endif
  size_t max_size;
} log_queue;

/**
 * @brief Reader-writer lock structure (lightweight)
 */
typedef struct log_rwlock {
#ifdef LOG_USE_STDATOMIC
  _Atomic(int) readers;
  _Atomic(int) writer;
  _Atomic(bool) write_waiting;
#else
  volatile int readers;
  volatile int writer;
  volatile bool write_waiting;
#endif
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
  bool syslog_enabled;
  int syslog_facility;
  bool show_thread_id;
} log_handler;
struct log {
  log_rwlock rwlock;

  void *udata;
  log_LockFn lock;

  int level;
  bool quiet;
  int format_mode;
  size_t max_file_size;

  bool async_enabled;
  log_queue queue;
  LOG_THREAD_T async_thread;
#ifdef LOG_USE_STDATOMIC
  atomic_bool async_running;
#else
  volatile bool async_running;
#endif

  log_handler *handlers;
  int handler_count;
  int handler_capacity;

  log_FormatFn format_fn;

  log_stats stats;
#ifdef LOG_USE_STDATOMIC
  atomic_uint_fast64_t last_timestamp;
#else
  volatile uint64_t last_timestamp;
#endif

  char *file_prefix;

#if LOG_PLATFORM_POSIX
  pthread_mutex_t mutex;
#else
  CRITICAL_SECTION mutex;
#endif

  char *syslog_ident;
  int syslog_facility;
  bool syslog_enabled_global;

  log_mpool mpool;
  log_ts_cache ts_cache;
  bool enable_ts_cache;
  bool enable_mpool;
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
/* Performance optimization functions */
void log_enable_mpool(log *ctx, bool enable);
void log_enable_ts_cache(log *ctx, bool enable);
void log_get_perf_stats(log *ctx, log_stats *stats);

int log_add_handler(log *ctx, log_LogFn fn, void *udata, int level);
int log_add_fp(log *ctx, FILE *fp, int level);
void log_remove_handler(log *ctx, int idx);

/* Thread ID and Syslog support */
void log_enable_thread_id(log *ctx, int handler_idx, bool enable);
int log_add_syslog_handler(log *ctx, const char *ident, int facility, int level);
void log_handler_enable_syslog(log *ctx, int handler_idx, bool enable);
int log_level_to_syslog(int level);

void log_handler_set_level(log *ctx, int handler_idx, int new_level);
void log_handler_set_formatter(log *ctx, int handler_idx, log_FormatFn new_fn);
void log_enable_text_format(log* ctx);
void log_enable_json_format(log* ctx);

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
