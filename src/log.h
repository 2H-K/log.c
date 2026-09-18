/**
 * Copyright (c) 2020 rxi
 * Modified for enhanced features (2026)
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */

#ifndef LOG_H
#define LOG_H

/* Platform detection (must precede feature-test macros and system headers) */
#if defined(_WIN32) || defined(_WIN64)
  #define LOG_PLATFORM_WINDOWS
#else
  #define LOG_PLATFORM_POSIX
  #if !defined(_GNU_SOURCE)
    #define _GNU_SOURCE
  #endif
#endif

/* MSVC CRT deprecation noise: the POSIX names used here (tzset, ...) are
 * standard on Windows too. Define before any CRT header so builds with /WX
 * stay clean; CMake passes the same definitions on the command line. */
#if defined(_MSC_VER)
  #ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
  #endif
  #ifndef _CRT_NONSTDC_NO_DEPRECATE
    #define _CRT_NONSTDC_NO_DEPRECATE
  #endif
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

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
  /* MSVC C mode: bool is not a keyword, use unsigned char */
  #ifndef __cplusplus
    #ifndef bool
      #define bool unsigned char
      #define true 1
      #define false 0
    #endif
  #endif
#else
  #error "C11 or later with stdatomic.h required, or MSVC"
#endif

/* Cross-platform alignment macro */
#if defined(_MSC_VER)
  #define LOG_ALIGN_64 __declspec(align(64))
  #define LOG_MEMBER_ALIGN_64
#elif defined(__GNUC__) || defined(__clang__)
  #define LOG_ALIGN_64 __attribute__((aligned(64)))
  #define LOG_MEMBER_ALIGN_64
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define LOG_ALIGN_64
  #define LOG_MEMBER_ALIGN_64 _Alignas(64)
#else
  #define LOG_ALIGN_64
  #define LOG_MEMBER_ALIGN_64
#endif

/* Cross-platform atomic size type */
#if defined(LOG_USE_STDATOMIC)
  #include <stdatomic.h>
  typedef atomic_size_t log_atomic_size_t;
#elif defined(LOG_USE_MSVC_ATOMIC)
  typedef volatile size_t log_atomic_size_t;
#else
  typedef volatile size_t log_atomic_size_t;
#endif

#if defined(LOG_PLATFORM_POSIX)
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

#define LOG_VERSION "0.1.0"
#define LOG_MAX_QUEUE_SIZE 4096
#define LOG_MAX_ROTATION_FILES 5
#define LOG_DEFAULT_MAX_SIZE (10 * 1024 * 1024)
#define MAX_HANDLERS 32

/* Static-allocation mode: sizes the storage embedded in log_handle when
 * LOG_STATIC_ALLOC is defined. Must be a power of two. Memory cost is
 * LOG_RING_CAPACITY * sizeof(log_ring_entry) (~664 bytes per slot). */
#ifndef LOG_RING_CAPACITY
  #define LOG_RING_CAPACITY LOG_MAX_QUEUE_SIZE
#endif

/* In-memory flight recorder (B1). Each memory handler keeps its last N
 * rendered lines. The requested line count is clamped to LOG_MEMORY_MAX_LINES
 * and a stored line is truncated to LOG_MEMORY_LINE_MAX bytes. Storage is
 * heap-allocated per handler at log_add_memory_handler() time, so contexts
 * that never add one pay nothing. */
#ifndef LOG_MEMORY_MAX_LINES
  #define LOG_MEMORY_MAX_LINES 1024
#endif
#ifndef LOG_MEMORY_LINE_MAX
  #define LOG_MEMORY_LINE_MAX 512
#endif

/* ======================================================================== */
/* Compile-time feature flags                                               */
/* Define these BEFORE including log.h to disable features                  */
/* ======================================================================== */

/* Convenience: Disable all optional features for a minimal build.
 * This MUST run before the individual flags below are evaluated, otherwise
 * it defines LOG_DISABLE_* too late and the features stay enabled. */
#ifdef LOG_MINIMAL
  #ifndef LOG_DISABLE_JSON
    #define LOG_DISABLE_JSON
  #endif
  #ifndef LOG_DISABLE_SYSLOG
    #define LOG_DISABLE_SYSLOG
  #endif
  #ifndef LOG_DISABLE_ASYNC
    #define LOG_DISABLE_ASYNC
  #endif
  #ifndef LOG_DISABLE_MPOOL
    #define LOG_DISABLE_MPOOL
  #endif
  #ifndef LOG_DISABLE_RING_QUEUE
    #define LOG_DISABLE_RING_QUEUE
  #endif
  #ifndef LOG_DISABLE_STATS
    #define LOG_DISABLE_STATS
  #endif
  #ifndef LOG_DISABLE_FILE_OPS
    #define LOG_DISABLE_FILE_OPS
  #endif
  #ifndef LOG_DISABLE_THREAD_ID
    #define LOG_DISABLE_THREAD_ID
  #endif
  #ifndef LOG_DISABLE_TS_CACHE
    #define LOG_DISABLE_TS_CACHE
  #endif
  #ifndef LOG_DISABLE_KV
    #define LOG_DISABLE_KV
  #endif
  #ifndef LOG_DISABLE_LIFECYCLE
    #define LOG_DISABLE_LIFECYCLE
  #endif
  #ifndef LOG_DISABLE_MEMORY_HANDLER
    #define LOG_DISABLE_MEMORY_HANDLER
  #endif
  #ifndef LOG_DISABLE_FILTER
    #define LOG_DISABLE_FILTER
  #endif
  #ifndef LOG_DISABLE_NAMED
    #define LOG_DISABLE_NAMED
  #endif
#endif

/* Disable JSON formatting support (saves ~2KB binary) */
#ifndef LOG_DISABLE_JSON
  #define LOG_FEATURE_JSON 1
#else
  #define LOG_FEATURE_JSON 0
#endif

/* Disable Syslog support (POSIX only, saves ~1KB binary) */
#ifndef LOG_DISABLE_SYSLOG
  #define LOG_FEATURE_SYSLOG 1
#else
  #define LOG_FEATURE_SYSLOG 0
#endif

/* Disable async logging support (saves ~3KB binary) */
#ifndef LOG_DISABLE_ASYNC
  #define LOG_FEATURE_ASYNC 1
#else
  #define LOG_FEATURE_ASYNC 0
#endif

/* Disable memory pool for queue entries (saves ~1KB binary) */
#ifndef LOG_DISABLE_MPOOL
  #define LOG_FEATURE_MPOOL 1
#else
  #define LOG_FEATURE_MPOOL 0
#endif

/* Disable ring buffer queue (saves ~2KB binary) */
#ifndef LOG_DISABLE_RING_QUEUE
  #define LOG_FEATURE_RING_QUEUE 1
#else
  #define LOG_FEATURE_RING_QUEUE 0
#endif

/* Disable performance statistics (saves ~0.5KB binary) */
#ifndef LOG_DISABLE_STATS
  #define LOG_FEATURE_STATS 1
#else
  #define LOG_FEATURE_STATS 0
#endif

/* Disable file operations (rotation, file handlers) (saves ~3KB binary) */
#ifndef LOG_DISABLE_FILE_OPS
  #define LOG_FEATURE_FILE_OPS 1
#else
  #define LOG_FEATURE_FILE_OPS 0
#endif

/* Disable thread ID support (saves ~0.3KB binary) */
#ifndef LOG_DISABLE_THREAD_ID
  #define LOG_FEATURE_THREAD_ID 1
#else
  #define LOG_FEATURE_THREAD_ID 0
#endif

/* Disable timestamp cache (saves ~0.2KB binary) */
#ifndef LOG_DISABLE_TS_CACHE
  #define LOG_FEATURE_TS_CACHE 1
#else
  #define LOG_FEATURE_TS_CACHE 0
#endif

/* Typed key-value metadata / structured logging (B2) */
#ifndef LOG_DISABLE_KV
  #define LOG_FEATURE_KV 1
#else
  #define LOG_FEATURE_KV 0
#endif

/* Process lifecycle safety: fork / exit flushing (B6) */
#ifndef LOG_DISABLE_LIFECYCLE
  #define LOG_FEATURE_LIFECYCLE 1
#else
  #define LOG_FEATURE_LIFECYCLE 0
#endif

/* In-memory flight recorder handler (B1): keep the last N formatted lines in
 * a ring and dump them on demand / from a fatal-signal handler. Auto-disabled
 * by LOG_MINIMAL. */
#ifndef LOG_DISABLE_MEMORY_HANDLER
  #define LOG_FEATURE_MEMORY_HANDLER 1
#else
  #define LOG_FEATURE_MEMORY_HANDLER 0
#endif

/* Rate limiting & duplicate suppression (B3): keep log floods from
 * amplifying an incident. Auto-disabled by LOG_MINIMAL. */
#ifndef LOG_DISABLE_FILTER
  #define LOG_FEATURE_FILTER 1
#else
  #define LOG_FEATURE_FILTER 0
#endif

/* Named loggers (B4): per-module level switches sharing the default context's
 * handlers. Auto-disabled by LOG_MINIMAL. */
#ifndef LOG_DISABLE_NAMED
  #define LOG_FEATURE_NAMED 1
#else
  #define LOG_FEATURE_NAMED 0
#endif

/* Named-logger registry: fixed-size flat table with linear lookup. */
#ifndef LOG_NAMED_MAX
  #define LOG_NAMED_MAX 16
#endif
#ifndef LOG_NAMED_NAME_MAX
  #define LOG_NAMED_NAME_MAX 16   /* 15 chars + NUL */
#endif

/* Crash-safe logging: per-line flush plus a fatal-signal marker line
 * (POSIX: SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE). Intentionally NOT
 * auto-disabled by LOG_MINIMAL: reliability is wanted in minimal daemon
 * builds too. Trim explicitly with LOG_DISABLE_CRASH_MODE. */
#ifndef LOG_DISABLE_CRASH_MODE
  #define LOG_FEATURE_CRASH_MODE 1
#else
  #define LOG_FEATURE_CRASH_MODE 0
#endif

/* Static-allocation mode (opt-in, define LOG_STATIC_ALLOC):
 * - log_create_static() builds the context inside caller-provided memory
 * - the async ring queue and handler table are embedded, not heap-allocated
 * - oversized messages are truncated instead of heap-allocated
 * - setvbuf buffer sizing is skipped (stdio keeps its default buffer)
 * Setup-time allocations (fopen, strdup of file_prefix/filename) still
 * happen; the guarantee is zero heap allocation on the logging hot path. */
#if defined(LOG_STATIC_ALLOC)
  #define LOG_FEATURE_STATIC_ALLOC 1
#else
  #define LOG_FEATURE_STATIC_ALLOC 0
#endif

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

/* Queue full handling policy (async mode) */
enum {
  LOG_QUEUE_FALLBACK_SYNC = 0,  /* Queue full -> write synchronously (default) */
  LOG_QUEUE_DROP,               /* Queue full -> drop the message */
  LOG_QUEUE_BLOCK               /* Queue full -> block until space is available */
};

/* Per-handler flush policy (durability vs throughput).
 * fflush moves data from stdio buffers to the kernel; fsync (separate
 * switch via log_handler_set_fsync) makes it durable. */
enum {
  LOG_FLUSH_NEVER = 0,   /* stdio default buffering (fastest, may lose data on crash) */
  LOG_FLUSH_EVERY,       /* fflush after every message */
  LOG_FLUSH_INTERVAL    /* fflush at most once per flush_interval_ms (sync mode:
                            applied on the next write after the interval elapses;
                            async mode: also driven by the writer thread timer) */
};

#ifndef LOG_USE_COLOR
  #define LOG_USE_COLOR
#endif

#if defined(LOG_PLATFORM_POSIX)
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
typedef struct log_handle log_handle;
typedef struct log_event log_event;
typedef void (*log_LogFn)(log_handle *ctx, log_event *ev);
typedef void (*log_LockFn)(bool lock, void *udata);

/**
 * Format function that formats the log message into a buffer.
 * @param ctx The logger context
 * @param ev Log event
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @return Number of characters written (excluding null terminator)
 */
typedef int (*log_FormatFn)(log_handle *ctx, log_event *ev, char *buf, size_t buf_size);

#if LOG_FEATURE_KV
/**
 * @brief Typed key-value metadata for structured logging.
 *
 * Pairs are assembled on the caller's stack by the log_*_kv() macros and
 * passed to log_log_kv(). The JSON formatter emits them as top-level fields;
 * the text formatter appends them as `key=value` after the message. At most
 * LOG_KV_MAX_PAIRS pairs are used; extra pairs are dropped and counted in
 * log_stats::truncated_count. A NULL key skips that pair.
 */
#define LOG_KV_MAX_PAIRS 8
#if LOG_FEATURE_STATIC_ALLOC
/* Static mode embeds the encoded kv blob in each ring slot. Cost is
 * LOG_KV_INLINE_MAX * LOG_RING_CAPACITY bytes; larger blobs are dropped
 * whole (counted in truncated_count) rather than heap-allocated. */
  #ifndef LOG_KV_INLINE_MAX
    #define LOG_KV_INLINE_MAX 256
  #endif
#endif

enum {
  LOG_KV_T_INT,
  LOG_KV_T_DOUBLE,
  LOG_KV_T_STR,
  LOG_KV_T_BOOL
};

typedef struct log_kv {
  const char *key;   /* field name; NULL entries are skipped */
  int type;          /* one of LOG_KV_T_* */
  long long i;       /* backing value for INT / BOOL */
  double d;          /* backing value for DOUBLE */
  const char *s;     /* borrowed string for STR (copied during the call) */
} log_kv;

/* Use as initializers of a stack log_kv array; LOG_KV_END is optional. */
#define LOG_KV_INT(k, v)    { (k), LOG_KV_T_INT,    (long long)(v), 0.0,         NULL }
#define LOG_KV_DOUBLE(k, v) { (k), LOG_KV_T_DOUBLE, 0,               (double)(v), NULL }
#define LOG_KV_STR(k, v)    { (k), LOG_KV_T_STR,    0,               0.0,         (v) }
#define LOG_KV_BOOL(k, v)   { (k), LOG_KV_T_BOOL,   ((v) ? 1 : 0),   0.0,         NULL }
#define LOG_KV_END          { NULL, LOG_KV_T_INT,   0,               0.0,         NULL }
#endif /* LOG_FEATURE_KV */

/**
 * @brief Log event structure
 */
struct log_event {
  va_list ap;
  const char *fmt;
  const char *raw_msg;  /* Pre-formatted message (async path); used instead of fmt */
  const char *file;
  struct tm *time;
  void *udata;
  int line;
  int level;
  double timestamp;  /* High-precision timestamp in seconds */
#if LOG_FEATURE_KV
  const char *kv;    /* encoded KV blob (internal); NULL when none */
#endif
};

/**
 * @brief Logger configuration
 */
typedef struct log_config {
  int level;
  bool quiet;
  size_t max_file_size;      /* For rotation */
  bool async_enabled;
  size_t queue_size;
  const char *file_prefix;   /* For log file rotation */
  log_FormatFn format_fn;    /* Custom format function */
} log_config;

/**
 * @brief Performance statistics.
 * Counters are kept per thread (contention-free writes) and aggregated
 * across every thread that logged to the context when read via
 * log_get_stats(), so any thread observes the same process-wide totals.
 * Values are best-effort (plain aligned increments, not atomic).
 */
typedef struct log_stats {
  uint64_t total_count;
  uint64_t level_counts[LOG_LEVELS];
  uint64_t queue_drops;
  uint64_t queue_blocked;
  uint64_t rotation_count;
  double avg_queue_latency_ms;  /* mean async enqueue->dequeue latency, ms */
  uint64_t async_writes;
  uint64_t sync_writes;
  uint64_t truncated_count;   /* messages / kv pairs dropped by size limits */
  uint64_t suppressed_count;  /* messages dropped by rate limit / dedupe (B3) */
} log_stats;

/**
 * @brief Per-thread statistics (cache-line aligned to prevent false sharing)
 * @note The counters are plain (non-atomic) and contention-free by design:
 *       each thread only ever writes its own slot. Snapshots may observe a
 *       torn intermediate value under concurrent writes — acceptable for
 *       best-effort monitoring counters.
 */
#if defined(_MSC_VER)
/* Intentional cache-line alignment: log_thread_stats is padded to 64 bytes and
 * any struct embedding one (log_handle) is padded to match. MSVC flags that as
 * C4324, which /WX promotes to an error in the /Oi regression guard and in any
 * consumer building with warnings-as-errors. The padding is by design. */
#pragma warning(push)
#pragma warning(disable: 4324)
#endif

typedef struct log_thread_stats {
  LOG_MEMBER_ALIGN_64 uint64_t total_count;
  uint64_t level_counts[LOG_LEVELS];
  uint64_t queue_drops;
  uint64_t queue_blocked;
  uint64_t rotation_count;
  uint64_t async_writes;
  uint64_t sync_writes;
  uint64_t truncated_count;
  uint64_t suppressed_count;          /* rate limit / dedupe drops (B3) */
  uint64_t queue_latency_count;       /* number of async messages with latency sampled */
  double queue_latency_total_ms;      /* sum of enqueue->dequeue latency, ms */
  uint64_t padding[2];
} LOG_ALIGN_64 log_thread_stats;

/* =====================================================================
 * INTERNAL — implementation detail, NOT part of the public API.
 *
 * Everything from here through the end of `struct log_handle` describes
 * the concrete layout of the logger. It is visible only so the two-file
 * build and its white-box tests compile; treat it as private. Do not
 * read or write these fields from application code, and do not rely on
 * this layout being stable across versions (no ABI guarantee).
 *
 * Public types (log_kv, log_event, log_config, log_stats,
 * log_thread_stats) are defined above this block.
 * ===================================================================== */

/**
 * @brief Per-context pool of per-thread stats blocks.
 * A logging thread claims a slot on its first call into a context and
 * records counters there; log_get_stats() sums every claimed slot, so
 * multi-threaded totals are correct regardless of which thread reads them.
 * Bounded at LOG_STATS_MAX_SLOTS distinct registrations per context; beyond
 * that a thread still counts locally but is not aggregated. Slots are owned
 * by the context, so a snapshot never follows a dangling thread pointer.
 * Guarded by log_handle::mutex.
 */
#define LOG_STATS_MAX_SLOTS 64
typedef struct log_stats_registry {
  log_thread_stats slots[LOG_STATS_MAX_SLOTS];
  int count;
} log_stats_registry;

/**
 * @brief Log queue entry for async mode
 */
typedef struct log_queue_entry {
  char *msg;    /* Pure message string (formatted once, no prefix) */
  int level;
  char *file;
  int line;
  double timestamp;
#if LOG_FEATURE_KV
  char *kv;     /* encoded kv blob (owned; NULL when none) */
#endif
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
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_t mtx;
#else
  CRITICAL_SECTION mtx;
#endif
} log_mpool;

/**
 * @brief Ring buffer entry (pre-allocated, cache-friendly)
 */
typedef struct log_ring_entry {
  char msg[512];
  char file[128];
  int level;
  int line;
  double timestamp;
  bool has_large_msg;
  bool has_large_file;
#if LOG_FEATURE_KV
  bool has_kv;
#if LOG_FEATURE_STATIC_ALLOC
  char kv_storage[LOG_KV_INLINE_MAX];
#endif
#endif
} log_ring_entry;

/**
 * @brief Ring buffer queue for async logging (mutex + condvar protected)
 */
typedef struct log_ring_queue {
  log_ring_entry *buffer;
  size_t capacity;
  size_t mask;
  bool storage_owned;   /* false when the buffer is embedded static storage */
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  pthread_cond_t space_cond;
#else
  CRITICAL_SECTION mtx;
  CONDITION_VARIABLE cond;
  CONDITION_VARIABLE space_cond;
#endif
  log_atomic_size_t head;
  log_atomic_size_t tail;
  bool closed;
} log_ring_queue;

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
 * @brief Log queue structure (mutex + condition variables for async mode)
 */
typedef struct log_queue {
  log_queue_entry *head;
  log_queue_entry *tail;
  size_t size;
  size_t max_size;
  bool closed;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  pthread_cond_t space_cond;
#else
  CRITICAL_SECTION mtx;
  CONDITION_VARIABLE cond;
  CONDITION_VARIABLE space_cond;
#endif
} log_queue;

/**
 * @brief Clock source selection for timestamp generation
 */
enum { LOG_CLOCK_REALTIME, LOG_CLOCK_REALTIME_COARSE, LOG_CLOCK_MONOTONIC, LOG_CLOCK_MONOTONIC_COARSE };

/**
 * @brief Reader-writer lock structure (wraps the platform rwlock)
 */
typedef struct log_rwlock {
#if defined(LOG_PLATFORM_POSIX)
  pthread_rwlock_t lock;
#else
  SRWLOCK lock;
#endif
} log_rwlock;

#if LOG_FEATURE_FILTER
/**
 * @brief Per-level fixed-window rate limiter (B3).
 *
 * max_per_sec == 0 disables the rule. The first accepted message of a window
 * starts the window; once `count` reaches max_per_sec every further message
 * of that level is suppressed until the window elapses.
 */
typedef struct log_rate_limit {
  unsigned max_per_sec;
  unsigned count;
  double window_start;   /* monotonic seconds; 0 = window not started */
} log_rate_limit;

/**
 * @brief Per-level duplicate-suppression state (B3).
 *
 * One active group per level: `hash` is the FNV-1a hash of the rendered
 * message (plus kv bytes); repeated identical messages inside the window are
 * counted in `suppressed` and emitted later as a single summary line. Each
 * level owns its group, so there is no cross-level collision.
 */
typedef struct log_dedupe_group {
  uint64_t hash;
  unsigned suppressed;
  double window_start;   /* monotonic seconds */
  bool active;
} log_dedupe_group;

/**
 * @brief Filtering state (rate limit + dedupe), guarded by `mtx`.
 *
 * Guarded by its own mutex rather than the context rwlock because every log
 * call mutates it. Callers never hold `mtx` while acquiring the rwlock, so
 * the two locks do not nest in both directions.
 */
typedef struct log_filter {
  bool enabled;
  log_rate_limit rate[LOG_LEVELS];
  log_dedupe_group dedupe[LOG_LEVELS];
  unsigned dedupe_window_ms[LOG_LEVELS];
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_t mtx;
#else
  CRITICAL_SECTION mtx;
#endif
} log_filter;
#endif /* LOG_FEATURE_FILTER */

/**
 * @brief Handler kinds (used to switch text/json output per handler)
 */
enum { HANDLER_STDOUT, HANDLER_FILE, HANDLER_SYSLOG, HANDLER_CUSTOM, HANDLER_MEMORY };

#if LOG_FEATURE_MEMORY_HANDLER
/**
 * @brief One recorded line of the in-memory flight recorder (B1).
 *
 * POD by construction: a fixed char array plus scalar fields, no pointers.
 * This is what lets a fatal-signal handler walk the ring and write(2) each
 * entry without locks or allocation. The `line` is a fully rendered line
 * including its trailing '\n'; `len` is the number of valid bytes.
 */
typedef struct log_memory_entry {
  char line[LOG_MEMORY_LINE_MAX];
  int level;
  int len;
} log_memory_entry;

/**
 * @brief Ring storage backing one memory handler.
 *
 * Heap-allocated at log_add_memory_handler() time (setup; not the hot path).
 * Producers serialize on mtx; the crash path reads entries/count/head
 * lock-free and tolerates the single in-flight entry being inconsistent.
 */
typedef struct log_memory_store {
  log_memory_entry *entries;   /* capacity rows */
  size_t capacity;
  size_t count;                /* valid entries, <= capacity */
  size_t head;                 /* next slot to overwrite */
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_t mtx;
#else
  CRITICAL_SECTION mtx;
#endif
} log_memory_store;
#endif /* LOG_FEATURE_MEMORY_HANDLER */

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
  int kind;
  bool owns_file;
  int flush_policy;            /* LOG_FLUSH_* */
  unsigned flush_interval_ms;  /* for LOG_FLUSH_INTERVAL */
  bool flush_fsync;            /* fsync(_commit) after flush */
  double last_flush;           /* monotonic seconds, interval bookkeeping */
  log_FormatFn format_fn;      /* per-handler formatter (overrides ctx->format_fn) */
} log_handler;

#if LOG_FEATURE_NAMED
/**
 * @brief One named-logger registry slot (B4).
 *
 * The handle is a lightweight alias (log_handle::base != NULL) that owns only
 * its level/quiet state; logging through it is redirected to the default
 * context, so all handlers are shared. Created lazily by log_get().
 */
typedef struct log_named_entry {
  char name[LOG_NAMED_NAME_MAX];
  struct log_handle *handle;
} log_named_entry;
#endif

struct log_handle {
  log_rwlock rwlock;

  void *udata;
  log_LockFn lock;

  int level;
  bool quiet;
  size_t max_file_size;

  bool async_enabled;
  int queue_policy;
  log_queue queue;
  LOG_THREAD_T async_thread;

  log_handler *handlers;
  int handler_count;
  int handler_capacity;

  log_FormatFn format_fn;

  log_stats stats;

  char *file_prefix;

#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_t mutex;
  pthread_mutex_t file_mtx;
#else
  CRITICAL_SECTION mutex;
  CRITICAL_SECTION file_mtx;
#endif

  char *syslog_ident;
  int syslog_facility;
  bool syslog_enabled_global;

  log_mpool mpool;
  bool enable_mpool;
  bool enable_ts_cache;

#if LOG_FEATURE_STATS
  log_stats_registry stats_registry;
  uint64_t stats_epoch;   /* unique per context creation; disambiguates
                             TLS registration hints after address reuse */
  log_thread_stats async_writer_stats;  /* dedicated slot for the writer thread
                                           (must not take ctx->mutex: the writer
                                           is joined while that mutex is held) */
#endif

  log_ring_queue ring_queue;
  bool use_ring_queue;
  int clock_source;

  bool crash_safe;   /* crash-safe mode: forces sync + per-line flush */

#if LOG_FEATURE_FILTER
  log_filter filter;   /* rate limiting / dedupe state (B3) */
#endif

#if LOG_FEATURE_NAMED
  /* Named-logger registry (B4), populated on the default context. */
  struct log_handle *base;   /* non-NULL on a named alias */
  log_named_entry named[LOG_NAMED_MAX];
  int named_count;
#endif

#if LOG_FEATURE_STATIC_ALLOC
  /* Embedded storage for static mode (sized by MAX_HANDLERS /
   * LOG_RING_CAPACITY; capacity must be a power of two, checked in log.c) */
  log_handler handlers_storage[MAX_HANDLERS];
#endif
#if LOG_FEATURE_STATIC_ALLOC && LOG_FEATURE_RING_QUEUE
  log_ring_entry ring_storage[LOG_RING_CAPACITY];
#endif
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* ==================== End of internal layout ==================== */

/* ==================== Stubs for disabled features ==================== */
/* These must come after all types they reference (log_stats, log_handle). */

#if !LOG_FEATURE_JSON
static inline int log_format_json(log_handle *ctx, log_event *ev, char *buf, size_t buf_size) {
  (void)ctx; (void)ev; (void)buf; (void)buf_size; return 0;
}
static inline void log_enable_json_format(log_handle* ctx) { (void)ctx; }
#endif

#if !LOG_FEATURE_THREAD_ID
static inline void log_enable_thread_id(log_handle *ctx, int handler_idx, bool enable) {
  (void)ctx; (void)handler_idx; (void)enable;
}
#endif

#if !LOG_FEATURE_TS_CACHE
static inline void log_enable_ts_cache(log_handle *ctx, bool enable) { (void)ctx; (void)enable; }
#endif

#if !LOG_FEATURE_MPOOL
static inline void log_enable_mpool(log_handle *ctx, bool enable) { (void)ctx; (void)enable; }
#endif

#if !LOG_FEATURE_RING_QUEUE
static inline void log_enable_ring_queue(log_handle *ctx, bool enable) { (void)ctx; (void)enable; }
#endif

#if !LOG_FEATURE_STATS
static inline void log_get_perf_stats(log_handle *ctx, log_stats *stats) { (void)ctx; (void)stats; }
#endif

#if !LOG_FEATURE_ASYNC
static inline int log_set_async(log_handle *ctx, bool enable) { (void)ctx; (void)enable; return -1; }
#endif

#if !LOG_FEATURE_FILE_OPS
static inline int log_add_file(log_handle *ctx, const char *filename, int level) {
  (void)ctx; (void)filename; (void)level; return -1;
}
static inline void log_rotate(log_handle *ctx) { (void)ctx; }
static inline void log_set_max_file_size(log_handle *ctx, size_t size) { (void)ctx; (void)size; }
static inline void log_set_file_prefix(log_handle *ctx, const char *prefix) { (void)ctx; (void)prefix; }
#endif

#if !LOG_FEATURE_SYSLOG
static inline int log_add_syslog_handler(log_handle *ctx, const char *ident, int facility, int level) {
  (void)ctx; (void)ident; (void)facility; (void)level; return -1;
}
static inline int log_level_to_syslog(int level) { (void)level; return 6; }
static inline void log_handler_enable_syslog(log_handle *ctx, int handler_idx, bool enable) {
  (void)ctx; (void)handler_idx; (void)enable;
}
#endif

#if !LOG_FEATURE_CRASH_MODE
static inline int log_set_crash_safe(log_handle *ctx, bool enable) { (void)ctx; (void)enable; return -1; }
static inline int log_install_crash_handler(log_handle *ctx) { (void)ctx; return -1; }
#endif

#if !LOG_FEATURE_LIFECYCLE
static inline int log_install_atfork(log_handle *ctx) { (void)ctx; return -1; }
static inline int log_install_atexit(log_handle *ctx) { (void)ctx; return -1; }
#endif

#if !LOG_FEATURE_MEMORY_HANDLER
static inline int log_add_memory_handler(log_handle *ctx, int lines, int level) {
  (void)ctx; (void)lines; (void)level; return -1;
}
static inline void log_dump_memory_handler(log_handle *ctx, int handler_idx, FILE *out) {
  (void)ctx; (void)handler_idx; (void)out;
}
#endif

#if !LOG_FEATURE_NAMED
static inline log_handle* log_get(const char *name) { (void)name; return NULL; }
static inline void log_named_set_level(const char *name, int level) {
  (void)name; (void)level;
}
#endif

#if !LOG_FEATURE_FILTER
static inline void log_set_rate_limit(log_handle *ctx, int level, unsigned max_per_sec) {
  (void)ctx; (void)level; (void)max_per_sec;
}
static inline void log_set_dedupe(log_handle *ctx, int level, unsigned window_ms) {
  (void)ctx; (void)level; (void)window_ms;
}
static inline void log_flush_suppressed(log_handle *ctx) { (void)ctx; }
#endif

#if !LOG_FEATURE_STATIC_ALLOC
static inline size_t log_static_ctx_size(void) { return 0; }
static inline log_handle* log_create_static(void *buf, size_t buf_size) {
  (void)buf; (void)buf_size; return NULL;
}
#endif

/* Core functions */
log_handle* log_create(void);
void log_destroy(log_handle *ctx);

log_handle* log_default(void);

/* Named loggers (B4): per-module level switches that share the default
 * context's handlers. log_get() lazily creates (once) and returns an alias
 * whose level/quiet are independent; logging through it emits via the default
 * handlers. Invalid / overlong names or a full registry (LOG_NAMED_MAX) return
 * the default context and emit a warning. Aliases live until the default
 * context is destroyed. Trim with LOG_DISABLE_NAMED (or LOG_MINIMAL). */
#if LOG_FEATURE_NAMED
log_handle* log_get(const char *name);
void log_named_set_level(const char *name, int level);
#endif

const char* log_level_string(int level);
void log_set_level(log_handle *ctx, int level);
void log_set_quiet(log_handle *ctx, bool enable);
void log_set_format(log_handle *ctx, log_FormatFn fn);
#if LOG_FEATURE_ASYNC
int log_set_async(log_handle *ctx, bool enable);
#endif
void log_set_queue_policy(log_handle *ctx, int policy);
#if LOG_FEATURE_FILE_OPS
void log_set_max_file_size(log_handle *ctx, size_t size);
void log_set_file_prefix(log_handle *ctx, const char *prefix);
#endif
/* Performance optimization functions */
#if LOG_FEATURE_MPOOL
void log_enable_mpool(log_handle *ctx, bool enable);
#endif
#if LOG_FEATURE_TS_CACHE
void log_enable_ts_cache(log_handle *ctx, bool enable);
#endif
#if LOG_FEATURE_STATS
void log_get_perf_stats(log_handle *ctx, log_stats *stats);
#endif
#if LOG_FEATURE_RING_QUEUE
void log_enable_ring_queue(log_handle *ctx, bool enable);
#endif
void log_set_clock_source(log_handle *ctx, int clock_source);
void log_set_queue_size(log_handle *ctx, size_t size);

int log_add_handler(log_handle *ctx, log_LogFn fn, void *udata, int level);
int log_add_fp(log_handle *ctx, FILE *fp, int level);
#if LOG_FEATURE_FILE_OPS
int log_add_file(log_handle *ctx, const char *filename, int level);
#endif
#if LOG_FEATURE_MEMORY_HANDLER
/* In-memory flight recorder (B1): keep the most recent `lines` lines whose
 * level is >= `level` in a ring, with no I/O. `lines` is clamped to
 * LOG_MEMORY_MAX_LINES; lines <= 0 returns -1. Returns the handler index.
 * Dump the retained lines oldest-first with log_dump_memory_handler(); the
 * snapshot is taken under the context and store locks, so it is consistent
 * with concurrent producers. */
int log_add_memory_handler(log_handle *ctx, int lines, int level);
void log_dump_memory_handler(log_handle *ctx, int handler_idx, FILE *out);
#endif
void log_remove_handler(log_handle *ctx, int idx);

/* Thread ID and Syslog support */
#if LOG_FEATURE_THREAD_ID
void log_enable_thread_id(log_handle *ctx, int handler_idx, bool enable);
#endif
#if LOG_FEATURE_SYSLOG
int log_add_syslog_handler(log_handle *ctx, const char *ident, int facility, int level);
void log_handler_enable_syslog(log_handle *ctx, int handler_idx, bool enable);
int log_level_to_syslog(int level);
#endif

void log_handler_set_level(log_handle *ctx, int handler_idx, int new_level);
void log_handler_set_formatter(log_handle *ctx, int handler_idx, log_FormatFn new_fn);
void log_enable_text_format(log_handle* ctx);
#if LOG_FEATURE_JSON
void log_enable_json_format(log_handle* ctx);
#endif

/* Durability: per-handler flush/fsync policy (file and stdout handlers).
 * Returns 0 on success, -1 on invalid arguments. */
int log_handler_set_flush(log_handle *ctx, int handler_idx, int policy, unsigned interval_ms);
int log_handler_set_fsync(log_handle *ctx, int handler_idx, bool enable);

/* Log flood control (B3): rate limiting and duplicate suppression.
 *
 * log_set_rate_limit: allow at most max_per_sec messages of `level` per
 *   one-second window. max_per_sec == 0 disables the rule for that level.
 * log_set_dedupe: suppress consecutive identical messages of `level` within
 *   window_ms. When the window expires (or a different message arrives) a
 *   single "last message repeated N times" line is emitted. window_ms == 0
 *   disables the rule for that level.
 * log_flush_suppressed: immediately emit any pending dedupe summaries (also
 *   done automatically by log_destroy).
 *
 * Suppressed messages are counted in log_stats::suppressed_count. Each call
 * configures exactly the given level (rate limit and dedupe state are
 * per-level); out-of-range levels are ignored. When LOG_FEATURE_FILTER is
 * disabled these are no-ops. */
#if LOG_FEATURE_FILTER
void log_set_rate_limit(log_handle *ctx, int level, unsigned max_per_sec);
void log_set_dedupe(log_handle *ctx, int level, unsigned window_ms);
void log_flush_suppressed(log_handle *ctx);
#endif

/* Crash-safe mode: forces the synchronous path (async is refused while on)
 * and switches every file/stdout handler to per-line flush, so all messages
 * written before a fatal signal have already reached the kernel.
 * log_install_crash_handler() (POSIX only) additionally writes a final
 * marker line to the handler files when the process dies from
 * SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE, dumps the tail of any registered
 * memory flight recorder (log_add_memory_handler) to those files, then
 * re-raises with default disposition (core dumps keep working). */
#if LOG_FEATURE_CRASH_MODE
int log_set_crash_safe(log_handle *ctx, bool enable);
int log_install_crash_handler(log_handle *ctx);
#endif

/* Process lifecycle safety (POSIX).
 *
 * log_install_atfork: registers pthread_atfork handlers for the context.
 *   Around fork() the context is quiesced (locks held); in the child the
 *   synchronization primitives are reinitialized (the parent's threads do
 *   not exist there) and async logging is downgraded to synchronous, so the
 *   child can keep logging without deadlocking on inherited locks.
 * log_install_atexit: registers an atexit handler that drains a still-async
 *   context when the process exits through exit() / return from main.
 *
 * Both are idempotent per context. Call them once during setup, before
 * spawning threads, and from a context that is not inside a log handler.
 * Return 0 on success, -1 on failure / unsupported platform. */
#if LOG_FEATURE_LIFECYCLE
int log_install_atfork(log_handle *ctx);
int log_install_atexit(log_handle *ctx);
#endif

/* Static-allocation mode (requires LOG_STATIC_ALLOC).
 * Declare storage as log_static_storage_t to get correct alignment:
 *   static log_static_storage_t g_log_storage;
 *   log_handle *ctx = log_create_static(&g_log_storage, sizeof g_log_storage);
 * One context per storage block; log_destroy() tears the context down but
 * does not free the storage. */
#if LOG_FEATURE_STATIC_ALLOC
typedef union {
  log_handle ctx;
  long double ld_align;
  long long ll_align;
  void *ptr_align;
  char bytes[sizeof(log_handle)];
} log_static_storage_t;

size_t log_static_ctx_size(void);
log_handle* log_create_static(void *buf, size_t buf_size);
#endif

void log_log(log_handle *ctx, int level, const char *file, int line, const char *fmt, ...);
#if LOG_FEATURE_KV
/* Structured log call: msg is emitted literally (not printf-formatted) and
 * the typed pairs are attached to the event. */
void log_log_kv(log_handle *ctx, int level, const char *file, int line,
                const log_kv *kvs, int kv_count, const char *msg);
#endif
#if LOG_FEATURE_FILE_OPS
void log_rotate(log_handle *ctx);
#endif

int log_get_stats(log_handle *ctx, log_stats *stats);
#if LOG_FEATURE_JSON
int log_format_json(log_handle *ctx, log_event *ev, char *buf, size_t buf_size);
#endif

/* Advanced pipeline configuration (must be called with no active async logging) */
typedef struct {
  log_FormatFn transform;
  log_LogFn output;
  void* context;
} log_stage_function;
void log_configure_pipeline(log_handle* ctx, log_stage_function* stages, int stage_count);

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

/* Structured (key-value) logging. The message is emitted literally.
 * Example:
 *   log_ctx_info_kv(ctx, "login", LOG_KV_STR("user", "alice"),
 *                   LOG_KV_INT("id", 42));
 * When LOG_FEATURE_KV is disabled these degrade to plain message logging. */
#if LOG_FEATURE_KV
#define LOG_KV_LOG_(ctx, level, msg, ...) do { \
    log_kv log_kv_items_[] = { __VA_ARGS__ }; \
    log_log_kv((ctx), (level), __FILE__, __LINE__, log_kv_items_, \
               (int)(sizeof(log_kv_items_) / sizeof(log_kv_items_[0])), (msg)); \
  } while (0)

#define log_ctx_trace_kv(ctx, msg, ...) LOG_KV_LOG_((ctx), LOG_TRACE, (msg), __VA_ARGS__)
#define log_ctx_debug_kv(ctx, msg, ...) LOG_KV_LOG_((ctx), LOG_DEBUG, (msg), __VA_ARGS__)
#define log_ctx_info_kv(ctx, msg, ...)  LOG_KV_LOG_((ctx), LOG_INFO,  (msg), __VA_ARGS__)
#define log_ctx_warn_kv(ctx, msg, ...)  LOG_KV_LOG_((ctx), LOG_WARN,  (msg), __VA_ARGS__)
#define log_ctx_error_kv(ctx, msg, ...) LOG_KV_LOG_((ctx), LOG_ERROR, (msg), __VA_ARGS__)
#define log_ctx_fatal_kv(ctx, msg, ...) LOG_KV_LOG_((ctx), LOG_FATAL, (msg), __VA_ARGS__)

#define log_trace_kv(msg, ...) LOG_KV_LOG_(log_default(), LOG_TRACE, (msg), __VA_ARGS__)
#define log_debug_kv(msg, ...) LOG_KV_LOG_(log_default(), LOG_DEBUG, (msg), __VA_ARGS__)
#define log_info_kv(msg, ...)  LOG_KV_LOG_(log_default(), LOG_INFO,  (msg), __VA_ARGS__)
#define log_warn_kv(msg, ...)  LOG_KV_LOG_(log_default(), LOG_WARN,  (msg), __VA_ARGS__)
#define log_error_kv(msg, ...) LOG_KV_LOG_(log_default(), LOG_ERROR, (msg), __VA_ARGS__)
#define log_fatal_kv(msg, ...) LOG_KV_LOG_(log_default(), LOG_FATAL, (msg), __VA_ARGS__)
#else
#define log_ctx_trace_kv(ctx, msg, ...) log_ctx_trace((ctx), "%s", (msg))
#define log_ctx_debug_kv(ctx, msg, ...) log_ctx_debug((ctx), "%s", (msg))
#define log_ctx_info_kv(ctx, msg, ...)  log_ctx_info((ctx), "%s", (msg))
#define log_ctx_warn_kv(ctx, msg, ...)  log_ctx_warn((ctx), "%s", (msg))
#define log_ctx_error_kv(ctx, msg, ...) log_ctx_error((ctx), "%s", (msg))
#define log_ctx_fatal_kv(ctx, msg, ...) log_ctx_fatal((ctx), "%s", (msg))
#define log_trace_kv(msg, ...) log_trace("%s", (msg))
#define log_debug_kv(msg, ...) log_debug("%s", (msg))
#define log_info_kv(msg, ...)  log_info("%s", (msg))
#define log_warn_kv(msg, ...)  log_warn("%s", (msg))
#define log_error_kv(msg, ...) log_error("%s", (msg))
#define log_fatal_kv(msg, ...) log_fatal("%s", (msg))
#endif /* LOG_FEATURE_KV */

#ifdef __cplusplus
}
#endif

#endif
