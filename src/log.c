/*
 * Enhanced C17 Log Library with async support, rotation, and performance stats
 * Based on rxi/log.c (original copyright 2020 rxi)
 * Modified for enhanced features (2026)
 *
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef LOG_PLATFORM_POSIX
#include <sys/stat.h>
#include <signal.h>
#endif

#ifdef LOG_PLATFORM_WINDOWS
#include <windows.h>
#include <sys/stat.h>
#include <io.h>
#endif

#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(int) >= 4, "int must be at least 32 bits");
#if LOG_FEATURE_STATIC_ALLOC && LOG_FEATURE_RING_QUEUE
_Static_assert(LOG_RING_CAPACITY > 0 &&
               (LOG_RING_CAPACITY & (LOG_RING_CAPACITY - 1)) == 0,
               "LOG_RING_CAPACITY must be a power of two");
#endif
#if LOG_FEATURE_MEMORY_HANDLER
/* The crash path writes these entries with write(2) directly, so the entry
 * layout must stay POD: a fixed char array followed by two ints, no pointers
 * and no implicit padding. Adding a pointer (or a wider field that forces
 * padding) breaks this assertion. */
_Static_assert(LOG_MEMORY_LINE_MAX >= 8,
               "LOG_MEMORY_LINE_MAX must leave room for prefix + '\\n'");
_Static_assert(sizeof(log_memory_entry) == LOG_MEMORY_LINE_MAX + 2 * sizeof(int),
               "log_memory_entry must be POD (no pointers/extra padding)");
#endif
#endif
#endif

/* ==================== Platform-specific helpers ==================== */

#ifdef LOG_PLATFORM_POSIX
/* CLOCK_*_COARSE are Linux-specific; fall back to the plain clocks elsewhere */
#ifndef CLOCK_REALTIME_COARSE
  #define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef CLOCK_MONOTONIC_COARSE
  #define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

static int clock_id_map[] = {
  CLOCK_REALTIME,
  CLOCK_REALTIME_COARSE,
  CLOCK_MONOTONIC,
  CLOCK_MONOTONIC_COARSE,
};
#endif

/* Get timestamp using configured clock source */
static double get_timestamp_with_clock(int clock_source) {
#ifdef LOG_PLATFORM_POSIX
  struct timespec ts;
  clockid_t cid = clock_id_map[clock_source < 4 ? clock_source : 0];
  clock_gettime(cid, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#elif defined(LOG_PLATFORM_WINDOWS)
  (void)clock_source;
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER uli;
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  /* Convert from 100ns intervals since 1601 to seconds since 1970 */
  return (double)(uli.QuadPart - 116444736000000000LL) / 10000000.0;
#endif
}

/* ==================== Atomic operation wrappers ==================== */

#ifdef LOG_USE_MSVC_ATOMIC

#define atomic_store(p, v) InterlockedExchange64((volatile LONG64*)(p), (LONG64)(v))
#define atomic_load(p) ((size_t)InterlockedOr64((volatile LONG64*)(p), 0))
#define atomic_fetch_add(p, v) InterlockedExchangeAdd64((volatile LONG64*)(p), (LONG64)(v))
#define atomic_fetch_sub(p, v) InterlockedExchangeAdd64((volatile LONG64*)(p), -(LONG64)(v))

static inline bool atomic_compare_exchange_strong(volatile void* obj, void* expected, void* desired) {
  return InterlockedCompareExchangePointer((volatile PVOID*)obj, desired, *(PVOID*)expected) == *(PVOID*)expected;
}

static inline bool atomic_compare_exchange_weak(volatile void* obj, void* expected, void* desired) {
  return atomic_compare_exchange_strong(obj, expected, desired);
}

#endif

#define DEFAULT_QUEUE_SIZE LOG_MAX_QUEUE_SIZE

#define LOG_MPOOL_CHUNK_SIZE 64
#define LOG_MPOOL_MAX_CHUNKS 64

/* Thread-local message buffer: the sync path formats the message body here
 * instead of heap-allocating it. Messages that do not fit fall back to an
 * exact-size malloc (truncation in static mode). */
#define LOG_MSG_BUF_SIZE 4096
static LOG_THREAD_LOCAL char tl_msg_buf[LOG_MSG_BUF_SIZE];

/* Async writer drain batch: number of entries moved out of the queue under
 * a single lock acquisition. 64 * sizeof(log_ring_entry) ~ 42 KB on the
 * writer thread stack. */
#define LOG_DRAIN_BATCH 64

/* stdio buffer size applied to files opened by the library itself
 * (log_add_file / rotation reopen). Fewer write() syscalls per message. */
#define LOG_FILE_BUF_SIZE (256 * 1024)

#if LOG_FEATURE_KV
/* Upper bound for the stack buffer an event's typed pairs are encoded into.
 * Pairs beyond this (or beyond LOG_KV_MAX_PAIRS) are dropped and counted. */
#define LOG_KV_ENCODE_MAX 1024
/* Stack buffer used when a text handler renders the kv suffix. */
#define LOG_KV_TEXT_RENDER_MAX 512
static size_t kv_blob_size(const char *blob);   /* defined with the kv helpers */
#endif /* LOG_FEATURE_KV */

#if LOG_FEATURE_STATS
/* Per-thread statistics pointer. Points at a slot in the current context's
 * pool (stats_registry.slots), never at thread-local storage: slots outlive
 * any thread, so snapshots never follow a dangling pointer. */
static LOG_THREAD_LOCAL log_thread_stats *tl_stats = NULL;
/* TLS hint: the ctx this thread last registered its stats slot with, plus
 * that context's creation epoch. The epoch disambiguates reused addresses
 * (log_create reuse and log_create_static's fixed storage): a stale hint
 * can alias a brand-new context, so a plain pointer compare is not enough. */
static LOG_THREAD_LOCAL log_handle *tl_stats_ctx = NULL;
static LOG_THREAD_LOCAL uint64_t tl_stats_epoch = 0;
static log_atomic_size_t g_stats_epoch = 0;

#define STAT_INC(f) do { if (tl_stats) tl_stats->f++; } while (0)
#define STAT_ADD(f, v) do { if (tl_stats) tl_stats->f += (v); } while (0)
#define STAT_LOAD(f) (tl_stats ? tl_stats->f : 0)
#define STAT_STORE(f, v) do { if (tl_stats) tl_stats->f = (v); } while (0)

/* Claim a slot in ctx's pool for this thread. Guarded by ctx->mutex. A slot
 * is claimed per (thread, context) registration; when a thread keeps using
 * one context it claims exactly one. If the pool is exhausted (more than
 * LOG_STATS_MAX_SLOTS distinct registrations), this thread counts locally
 * but is not aggregated. */
static void stats_register_slot_locked(log_handle *ctx) {
  if (!ctx) return;
  if (ctx->stats_registry.count < LOG_STATS_MAX_SLOTS) {
    int idx = ctx->stats_registry.count++;
    memset(&ctx->stats_registry.slots[idx], 0, sizeof(log_thread_stats));
    tl_stats = &ctx->stats_registry.slots[idx];
  } else {
    tl_stats = NULL;
  }
  tl_stats_ctx = ctx;
  tl_stats_epoch = ctx->stats_epoch;
}

/* Fast path first: most threads log to a single context. Only on a hint
 * mismatch do we take ctx->mutex and (re)scan. MUST be called with no ctx
 * locks held: log_enable_mpool()/log_enable_ts_cache() take ctx->mutex and
 * then the rwlock, so taking ctx->mutex while holding the rwlock would
 * deadlock. */
static void stats_ensure_registered(log_handle *ctx) {
  if (!ctx) return;
  if (tl_stats_ctx == ctx && tl_stats_epoch == ctx->stats_epoch) return;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->mutex);
#else
  EnterCriticalSection(&ctx->mutex);
#endif
  stats_register_slot_locked(ctx);
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->mutex);
#else
  LeaveCriticalSection(&ctx->mutex);
#endif
}

static void stats_init_registry(log_handle *ctx) {
  memset(&ctx->stats_registry, 0, sizeof(ctx->stats_registry));
  ctx->stats_epoch = (uint64_t)atomic_fetch_add(&g_stats_epoch, (size_t)1) + 1;
  /* Register the creating thread so single-threaded reads are correct. */
  stats_ensure_registered(ctx);
}
#else /* !LOG_FEATURE_STATS */
#define STAT_INC(f) ((void)0)
#define STAT_ADD(f, v) ((void)(v))
#define STAT_LOAD(f) (0)
#define STAT_STORE(f, v) ((void)(v))
static void stats_ensure_registered(log_handle *ctx) { (void)ctx; }
static void stats_init_registry(log_handle *ctx) { (void)ctx; }
#endif /* LOG_FEATURE_STATS */

static log_handle *DEFAULT_LOG = NULL;
#if defined(LOG_PLATFORM_POSIX)
static pthread_mutex_t default_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#else
static CRITICAL_SECTION default_log_mutex;
static bool default_log_mutex_initialized = false;
#endif

static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[91m"
};
#endif

#if LOG_FEATURE_MPOOL

static void mpool_init(log_mpool *mp, size_t max_size) {
  mp->free_list = NULL;
  mp->allocated = 0;
  mp->max_size = max_size;
  mp->chunk_count = 0;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&mp->mtx, NULL);
#else
  InitializeCriticalSection(&mp->mtx);
#endif
}

static void mpool_destroy(log_mpool *mp) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&mp->mtx);
#else
  EnterCriticalSection(&mp->mtx);
#endif
  while (mp->free_list) {
    log_queue_entry *next = mp->free_list->next;
    free(mp->free_list->msg);
    free(mp->free_list->file);
    free(mp->free_list);
    mp->free_list = next;
  }
  mp->allocated = 0;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&mp->mtx);
  pthread_mutex_destroy(&mp->mtx);
#else
  LeaveCriticalSection(&mp->mtx);
  DeleteCriticalSection(&mp->mtx);
#endif
}

#if LOG_FEATURE_ASYNC
static log_queue_entry* mpool_alloc(log_mpool *mp) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&mp->mtx);
#else
  EnterCriticalSection(&mp->mtx);
#endif
  log_queue_entry *entry = NULL;
  if (mp->free_list) {
    entry = mp->free_list;
    mp->free_list = entry->next;
    entry->next = NULL;
  } else if (mp->allocated < mp->max_size) {
    entry = calloc(1, sizeof(log_queue_entry));
    if (entry) {
      entry->msg = malloc(512);
      entry->file = malloc(128);
      if (!entry->msg || !entry->file) {
        free(entry->msg);
        free(entry->file);
        free(entry);
        entry = NULL;
      } else {
        entry->msg[0] = '\0';
        entry->file[0] = '\0';
        mp->allocated++;
      }
    }
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&mp->mtx);
#else
  LeaveCriticalSection(&mp->mtx);
#endif
  return entry;
}

static void mpool_free(log_mpool *mp, log_queue_entry *entry) {
  if (!entry) return;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&mp->mtx);
#else
  EnterCriticalSection(&mp->mtx);
#endif
  entry->next = mp->free_list;
  mp->free_list = entry;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&mp->mtx);
#else
  LeaveCriticalSection(&mp->mtx);
#endif
}
#endif /* LOG_FEATURE_ASYNC */

#endif /* LOG_FEATURE_MPOOL */

#if LOG_FEATURE_TS_CACHE
static LOG_THREAD_LOCAL log_ts_cache ts_cache_local;
#endif

/* Format a high-precision timestamp. When use_cache is set, the
 * "%Y-%m-%dT%H:%M:%S" part is cached per thread and reused while the
 * whole-second value does not change (avoids localtime + strftime). */
static void format_timestamp(double ts, char *buf, size_t size, bool use_cache) {
  time_t t = (time_t)ts;
  struct tm tm_buf;
#if LOG_FEATURE_TS_CACHE
  log_ts_cache *cache = &ts_cache_local;
  if (use_cache) {
    if (cache->cached_string[0] != '\0' && cache->last_timestamp == (double)t) {
      cache->cache_hits++;
      int ms = (int)((ts - (double)t) * 1000);
      snprintf(buf, size, "%s.%03d", cache->cached_string, ms);
      return;
    }
#if defined(LOG_PLATFORM_POSIX)
    struct tm *tm_info = localtime_r(&t, &tm_buf);
#else
    struct tm *tm_info = localtime_s(&tm_buf, &t) == 0 ? &tm_buf : NULL;
#endif
    if (tm_info) {
      strftime(cache->cached_string, sizeof(cache->cached_string), "%Y-%m-%dT%H:%M:%S", tm_info);
      cache->last_timestamp = (double)t;
      cache->cache_misses++;
      int ms = (int)((ts - (double)t) * 1000);
      snprintf(buf, size, "%s.%03d", cache->cached_string, ms);
      return;
    }
    cache->cached_string[0] = '\0';
    buf[0] = '\0';
    return;
  }
#else
  (void)use_cache;
#endif /* LOG_FEATURE_TS_CACHE */
#if defined(LOG_PLATFORM_POSIX)
  struct tm *tm_info = localtime_r(&t, &tm_buf);
#else
  struct tm *tm_info = localtime_s(&tm_buf, &t) == 0 ? &tm_buf : NULL;
#endif
  if (tm_info) {
    strftime(buf, size, "%Y-%m-%dT%H:%M:%S", tm_info);
    int ms = (int)((ts - (double)t) * 1000);
    size_t len = strlen(buf);
    if (len + 5 <= size) {
      snprintf(buf + len, size - len, ".%03d", ms);
    }
  } else {
    buf[0] = '\0';
  }
}

/* ==================== Reader-Writer Lock (platform rwlock) ==================== */

static void rwlock_init(log_rwlock *lock) {
#if defined(LOG_PLATFORM_POSIX)
#if defined(__GLIBC__) && defined(PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP)
  pthread_rwlockattr_t attr;
  pthread_rwlockattr_init(&attr);
  pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
  pthread_rwlock_init(&lock->lock, &attr);
  pthread_rwlockattr_destroy(&attr);
#else
  pthread_rwlock_init(&lock->lock, NULL);
#endif
#else
  InitializeSRWLock(&lock->lock);
#endif
}

static void rwlock_destroy(log_rwlock *lock) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_rwlock_destroy(&lock->lock);
#else
  (void)lock;
#endif
}

static void rwlock_read_lock(log_rwlock *lock) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_rwlock_rdlock(&lock->lock);
#else
  AcquireSRWLockShared(&lock->lock);
#endif
}

static void rwlock_read_unlock(log_rwlock *lock) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_rwlock_unlock(&lock->lock);
#else
  ReleaseSRWLockShared(&lock->lock);
#endif
}

static void rwlock_write_lock(log_rwlock *lock) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_rwlock_wrlock(&lock->lock);
#else
  AcquireSRWLockExclusive(&lock->lock);
#endif
}

static void rwlock_write_unlock(log_rwlock *lock) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_rwlock_unlock(&lock->lock);
#else
  ReleaseSRWLockExclusive(&lock->lock);
#endif
}

/* ==================== Async Queue (linked-list) ==================== */

#if LOG_FEATURE_ASYNC
static log_queue_entry* queue_entry_create(log_handle *ctx, log_event *ev) {
  (void)ctx;
#if LOG_FEATURE_MPOOL
  log_mpool *mp = &ctx->mpool;
  bool use_mpool = ctx->enable_mpool;
  log_queue_entry *entry = use_mpool ? mpool_alloc(mp) : calloc(1, sizeof(log_queue_entry));
#else
  log_queue_entry *entry = calloc(1, sizeof(log_queue_entry));
#endif
  if (!entry) return NULL;

  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int len = vsnprintf(NULL, 0, ev->fmt, args_copy);
  va_end(args_copy);

  if (len < 0) goto fail;

#if LOG_FEATURE_MPOOL
  if (use_mpool) {
    /* Allocate msg buffer if NULL or too small */
    if (!entry->msg || (size_t)len >= 512) {
      char *new_msg = realloc(entry->msg, (size_t)len + 1);
      if (!new_msg) goto fail;
      entry->msg = new_msg;
    }
    vsnprintf(entry->msg, (size_t)len + 1, ev->fmt, ev->ap);

    size_t file_len = ev->file ? strlen(ev->file) : 0;
    if (!entry->file || file_len >= 128) {
      char *new_file = realloc(entry->file, file_len + 1);
      if (!new_file) goto fail;
      entry->file = new_file;
    }
    if (ev->file) {
      memcpy(entry->file, ev->file, file_len);
    }
    entry->file[file_len] = '\0';
  } else
#endif
  {
    entry->msg = malloc((size_t)len + 1);
    if (!entry->msg) goto fail;
    vsnprintf(entry->msg, (size_t)len + 1, ev->fmt, ev->ap);
    entry->file = strdup(ev->file ? ev->file : "");
    if (!entry->file) goto fail;
  }

  entry->level = ev->level;
  entry->line = ev->line;
  entry->timestamp = ev->timestamp;
  entry->next = NULL;
#if LOG_FEATURE_KV
  entry->kv = NULL;
#endif
  return entry;

fail:
  /* Roll back: pooled entries return to pool, heap entries freed (Bug 4). */
#if LOG_FEATURE_MPOOL
  if (use_mpool) {
    mpool_free(mp, entry);
    return NULL;
  }
#endif
  free(entry->msg);
  free(entry->file);
  free(entry);
  return NULL;
}

#if LOG_FEATURE_KV || LOG_FEATURE_FILTER
/* Pre-rendered-message variant of queue_entry_create(): the body (and an
 * optional encoded kv blob) are copied, never borrowed. Used by log_log_kv()
 * and by the filtering path (B3), which renders the body before deciding
 * whether to enqueue it. `kv` is NULL outside LOG_FEATURE_KV builds. */
static log_queue_entry* queue_entry_create_body(log_handle *ctx, const char *body,
                                                const char *kv, const char *file,
                                                int level, int line, double timestamp) {
  (void)ctx;
#if !LOG_FEATURE_KV
  (void)kv;
#endif
#if LOG_FEATURE_MPOOL
  log_mpool *mp = &ctx->mpool;
  bool use_mpool = ctx->enable_mpool;
  log_queue_entry *entry = use_mpool ? mpool_alloc(mp) : calloc(1, sizeof(log_queue_entry));
#else
  log_queue_entry *entry = calloc(1, sizeof(log_queue_entry));
#endif
  if (!entry) return NULL;

  size_t len = body ? strlen(body) : 0;
#if LOG_FEATURE_MPOOL
  size_t file_len = file ? strlen(file) : 0;
#endif

#if LOG_FEATURE_MPOOL
  if (use_mpool) {
    if (!entry->msg || len >= 512) {
      char *new_msg = realloc(entry->msg, len + 1);
      if (!new_msg) goto fail;
      entry->msg = new_msg;
    }
    if (body) memcpy(entry->msg, body, len);
    entry->msg[len] = '\0';

    if (!entry->file || file_len >= 128) {
      char *new_file = realloc(entry->file, file_len + 1);
      if (!new_file) goto fail;
      entry->file = new_file;
    }
    if (file) memcpy(entry->file, file, file_len);
    entry->file[file_len] = '\0';
  } else
#endif
  {
    entry->msg = malloc(len + 1);
    if (!entry->msg) goto fail;
    if (body) memcpy(entry->msg, body, len);
    entry->msg[len] = '\0';
    entry->file = strdup(file ? file : "");
    if (!entry->file) goto fail;
  }

#if LOG_FEATURE_KV
  size_t kv_bytes = kv_blob_size(kv);
  if (kv_bytes) {
    entry->kv = malloc(kv_bytes);
    if (!entry->kv) goto fail;
    memcpy(entry->kv, kv, kv_bytes);
  } else {
    entry->kv = NULL;
  }
#endif
  entry->level = level;
  entry->line = line;
  entry->timestamp = timestamp;
  entry->next = NULL;
  return entry;

fail:
#if LOG_FEATURE_MPOOL
  if (use_mpool) { mpool_free(mp, entry); return NULL; }
#endif
  free(entry->msg);
  free(entry->file);
#if LOG_FEATURE_KV
  free(entry->kv);
#endif
  free(entry);
  return NULL;
}
#endif /* LOG_FEATURE_KV || LOG_FEATURE_FILTER */

static void queue_entry_destroy(log_handle *ctx, log_queue_entry *entry) {
  if (!entry) return;
  (void)ctx;
#if LOG_FEATURE_MPOOL
  if (ctx->enable_mpool) {
    mpool_free(&ctx->mpool, entry);
    return;
  }
#endif
  free(entry->msg);
  free(entry->file);
#if LOG_FEATURE_KV
  free(entry->kv);
#endif
  free(entry);
}
#endif /* LOG_FEATURE_ASYNC */

static void queue_init(log_queue *q, size_t max_size) {
  q->head = NULL;
  q->tail = NULL;
  q->size = 0;
  q->max_size = max_size;
  q->closed = false;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&q->mtx, NULL);
  pthread_cond_init(&q->cond, NULL);
  pthread_cond_init(&q->space_cond, NULL);
#else
  InitializeCriticalSection(&q->mtx);
  InitializeConditionVariable(&q->cond);
  InitializeConditionVariable(&q->space_cond);
#endif
}

#if LOG_FEATURE_ASYNC
static bool queue_push(log_handle *ctx, log_queue_entry *entry, bool blocking) {
  log_queue *q = &ctx->queue;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&q->mtx);
#else
  EnterCriticalSection(&q->mtx);
#endif
  if (q->closed) {
#if defined(LOG_PLATFORM_POSIX)
    pthread_mutex_unlock(&q->mtx);
#else
    LeaveCriticalSection(&q->mtx);
#endif
    return false;
  }
  while (q->size >= q->max_size) {
    if (!blocking) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&q->mtx);
#else
      LeaveCriticalSection(&q->mtx);
#endif
      return false;
    }
    STAT_INC(queue_blocked);
#if defined(LOG_PLATFORM_POSIX)
    pthread_cond_wait(&q->space_cond, &q->mtx);
#else
    SleepConditionVariableCS(&q->space_cond, &q->mtx, INFINITE);
#endif
    if (q->closed) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&q->mtx);
#else
      LeaveCriticalSection(&q->mtx);
#endif
      return false;
    }
  }
  entry->next = NULL;
  if (q->tail) {
    q->tail->next = entry;
  } else {
    q->head = entry;
  }
  q->tail = entry;
  q->size++;
#if defined(LOG_PLATFORM_POSIX)
  pthread_cond_signal(&q->cond);
  pthread_mutex_unlock(&q->mtx);
#else
  WakeConditionVariable(&q->cond);
  LeaveCriticalSection(&q->mtx);
#endif
  return true;
}

/* Drain up to cap entries under a single lock acquisition.
 * Same return contract as ring_queue_pop_batch. */
#if LOG_FEATURE_ASYNC
static int queue_pop_batch(log_queue *q, log_queue_entry **out, size_t cap, int timeout_ms) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&q->mtx);
  while (!q->closed && q->size == 0) {
    if (timeout_ms >= 0) {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += timeout_ms / 1000;
      deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
      }
      if (pthread_cond_timedwait(&q->cond, &q->mtx, &deadline) == ETIMEDOUT) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
      }
    } else {
      pthread_cond_wait(&q->cond, &q->mtx);
    }
  }
  if (q->size == 0) {
    pthread_mutex_unlock(&q->mtx);
    return 0;
  }
  size_t n = 0;
  while (q->head && n < cap) {
    out[n++] = q->head;
    q->head = q->head->next;
    if (!q->head) q->tail = NULL;
  }
  q->size -= n;
  pthread_mutex_unlock(&q->mtx);
  for (size_t k = 0; k < n; k++) {
    pthread_cond_signal(&q->space_cond);
  }
  return (int)n;
#else
  EnterCriticalSection(&q->mtx);
  while (!q->closed && q->size == 0) {
    DWORD wait_ms = timeout_ms >= 0 ? (DWORD)timeout_ms : INFINITE;
    if (!SleepConditionVariableCS(&q->cond, &q->mtx, wait_ms)) {
      LeaveCriticalSection(&q->mtx);
      return -1;
    }
  }
  if (q->size == 0) {
    LeaveCriticalSection(&q->mtx);
    return 0;
  }
  size_t n = 0;
  while (q->head && n < cap) {
    out[n++] = q->head;
    q->head = q->head->next;
    if (!q->head) q->tail = NULL;
  }
  q->size -= n;
  LeaveCriticalSection(&q->mtx);
  for (size_t k = 0; k < n; k++) {
    WakeConditionVariable(&q->space_cond);
  }
  return (int)n;
#endif
}
#endif /* LOG_FEATURE_ASYNC */  /* queue_pop_batch */

#endif /* LOG_FEATURE_ASYNC */  /* queue_push .. queue_pop_batch */

static void queue_shutdown(log_queue *q) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&q->mtx);
  q->closed = true;
  pthread_cond_broadcast(&q->cond);
  pthread_cond_broadcast(&q->space_cond);
  pthread_mutex_unlock(&q->mtx);
#else
  EnterCriticalSection(&q->mtx);
  q->closed = true;
  WakeAllConditionVariable(&q->cond);
  WakeAllConditionVariable(&q->space_cond);
  LeaveCriticalSection(&q->mtx);
#endif
}

#if LOG_FEATURE_ASYNC
/* Reopen a queue that was shut down (used when async is re-enabled). */
static void queue_reopen(log_queue *q) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&q->mtx);
  q->closed = false;
  pthread_mutex_unlock(&q->mtx);
#else
  EnterCriticalSection(&q->mtx);
  q->closed = false;
  LeaveCriticalSection(&q->mtx);
#endif
}
#endif /* LOG_FEATURE_ASYNC */

static void queue_destroy(log_queue *q) {
  if (!q) return;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&q->mtx);
#else
  EnterCriticalSection(&q->mtx);
#endif
  log_queue_entry *cur = q->head;
  while (cur) {
    log_queue_entry *next = cur->next;
    free(cur->msg);
    free(cur->file);
#if LOG_FEATURE_KV
    free(cur->kv);
#endif
    free(cur);
    cur = next;
  }
  q->head = NULL;
  q->tail = NULL;
  q->size = 0;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&q->mtx);
  pthread_mutex_destroy(&q->mtx);
  pthread_cond_destroy(&q->cond);
  pthread_cond_destroy(&q->space_cond);
#else
  LeaveCriticalSection(&q->mtx);
  DeleteCriticalSection(&q->mtx);
#endif
}

/* ==================== Ring Buffer Implementation ==================== */

#if LOG_FEATURE_RING_QUEUE

static void ring_queue_init_with_storage(log_ring_queue *rq, size_t capacity,
                                         log_ring_entry *storage) {
  size_t cap = 1;
  while (cap < capacity) cap <<= 1;
  rq->capacity = cap;
  rq->mask = cap - 1;
  if (storage) {
    rq->buffer = storage;
    rq->storage_owned = false;
    memset(storage, 0, cap * sizeof(log_ring_entry));
  } else {
    rq->buffer = calloc(cap, sizeof(log_ring_entry));
    rq->storage_owned = true;
  }
  atomic_store(&rq->head, 0);
  atomic_store(&rq->tail, 0);
  rq->closed = false;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&rq->mtx, NULL);
  pthread_cond_init(&rq->cond, NULL);
  pthread_cond_init(&rq->space_cond, NULL);
#else
  InitializeCriticalSection(&rq->mtx);
  InitializeConditionVariable(&rq->cond);
  InitializeConditionVariable(&rq->space_cond);
#endif
}

#if !LOG_FEATURE_STATIC_ALLOC
static void ring_queue_init(log_ring_queue *rq, size_t capacity) {
  ring_queue_init_with_storage(rq, capacity, NULL);
}
#endif

static void ring_queue_destroy(log_ring_queue *rq) {
  if (!rq->buffer) return;
  if (rq->storage_owned) {
    free(rq->buffer);
  }
  rq->buffer = NULL;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_destroy(&rq->mtx);
  pthread_cond_destroy(&rq->cond);
  pthread_cond_destroy(&rq->space_cond);
#else
  DeleteCriticalSection(&rq->mtx);
#endif
}

#if LOG_FEATURE_ASYNC
/* Format directly into ring buffer slot - eliminates malloc + memcpy for small messages */
static bool ring_queue_push_vfmt(log_ring_queue *rq, const char *fmt, va_list ap,
                                 const char *file, int level, int line,
                                 double timestamp, bool blocking) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&rq->mtx);
#else
  EnterCriticalSection(&rq->mtx);
#endif
  if (rq->closed) {
#if defined(LOG_PLATFORM_POSIX)
    pthread_mutex_unlock(&rq->mtx);
#else
    LeaveCriticalSection(&rq->mtx);
#endif
    return false;
  }
  size_t head = atomic_load(&rq->head);
  size_t tail = atomic_load(&rq->tail);
  while (tail - head >= rq->capacity) {
    if (!blocking) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    STAT_INC(queue_blocked);
#if defined(LOG_PLATFORM_POSIX)
    pthread_cond_wait(&rq->space_cond, &rq->mtx);
#else
    SleepConditionVariableCS(&rq->space_cond, &rq->mtx, INFINITE);
#endif
    if (rq->closed) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    head = atomic_load(&rq->head);
    tail = atomic_load(&rq->tail);
  }
  log_ring_entry *entry = &rq->buffer[tail & rq->mask];
#if LOG_FEATURE_KV
  entry->has_kv = false;
#endif

  va_list ap_copy;
  va_copy(ap_copy, ap);
  int msg_len = vsnprintf(entry->msg, sizeof(entry->msg), fmt, ap_copy);
  va_end(ap_copy);

  if (msg_len < 0) {
    entry->has_large_msg = false;
    entry->msg[0] = '\0';
  } else if ((size_t)msg_len >= sizeof(entry->msg)) {
#if LOG_FEATURE_STATIC_ALLOC
    /* Static mode: keep the vsnprintf-truncated text, no heap allocation. */
    entry->has_large_msg = false;
    STAT_INC(truncated_count);
#else
    entry->has_large_msg = true;
    char *large = malloc((size_t)msg_len + 1);
    if (!large) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    va_list ap_copy2;
    va_copy(ap_copy2, ap);
    vsnprintf(large, (size_t)msg_len + 1, fmt, ap_copy2);
    va_end(ap_copy2);
    *(char**)entry->msg = large;
#endif
  } else {
    entry->has_large_msg = false;
  }

  size_t file_len = file ? strlen(file) : 0;
  if (file_len >= sizeof(entry->file)) {
#if LOG_FEATURE_STATIC_ALLOC
    /* Static mode: truncate the file path instead of heap-allocating. */
    entry->has_large_file = false;
    if (file) {
      size_t copy = sizeof(entry->file) - 1;
      memcpy(entry->file, file, copy);
      entry->file[copy] = '\0';
    } else {
      entry->file[0] = '\0';
    }
    STAT_INC(truncated_count);
#else
    entry->has_large_file = true;
    char *large = malloc(file_len + 1);
    if (!large) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    if (file) memcpy(large, file, file_len + 1);
    else large[0] = '\0';
    *(char**)entry->file = large;
#endif
  } else {
    entry->has_large_file = false;
    if (file) memcpy(entry->file, file, file_len + 1);
    else entry->file[0] = '\0';
  }
  entry->level = level;
  entry->line = line;
  entry->timestamp = timestamp;
  atomic_store(&rq->tail, tail + 1);
#if defined(LOG_PLATFORM_POSIX)
  pthread_cond_signal(&rq->cond);
  pthread_mutex_unlock(&rq->mtx);
#else
  WakeConditionVariable(&rq->cond);
  LeaveCriticalSection(&rq->mtx);
#endif
  return true;
}

#if LOG_FEATURE_KV || LOG_FEATURE_FILTER
/* Push a literal, already-rendered message (plus an optional encoded kv blob).
 * Non-static: a small body with no kv stays inline in the slot; a large body
 * or any kv goes into one heap block ("body\0blob") tracked by has_large_msg
 * so the existing ownership transfer / free path releases it.
 * Static: body and blob are copied into the slot (truncated if too long; the
 * overflow is counted). */
static bool ring_queue_push_body(log_ring_queue *rq, const char *body, const char *kv,
                                 const char *file, int level, int line,
                                 double timestamp, bool blocking) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&rq->mtx);
#else
  EnterCriticalSection(&rq->mtx);
#endif
  if (rq->closed) {
#if defined(LOG_PLATFORM_POSIX)
    pthread_mutex_unlock(&rq->mtx);
#else
    LeaveCriticalSection(&rq->mtx);
#endif
    return false;
  }
  size_t head = atomic_load(&rq->head);
  size_t tail = atomic_load(&rq->tail);
  while (tail - head >= rq->capacity) {
    if (!blocking) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    STAT_INC(queue_blocked);
#if defined(LOG_PLATFORM_POSIX)
    pthread_cond_wait(&rq->space_cond, &rq->mtx);
#else
    SleepConditionVariableCS(&rq->space_cond, &rq->mtx, INFINITE);
#endif
    if (rq->closed) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    head = atomic_load(&rq->head);
    tail = atomic_load(&rq->tail);
  }
  log_ring_entry *entry = &rq->buffer[tail & rq->mask];

  /* File path first: on the rare large-path OOM the body block is not yet
   * allocated, so unwinding stays leak-free. */
  size_t file_len = file ? strlen(file) : 0;
  if (file_len >= sizeof(entry->file)) {
#if LOG_FEATURE_STATIC_ALLOC
    entry->has_large_file = false;
    if (file) {
      size_t copy = sizeof(entry->file) - 1;
      memcpy(entry->file, file, copy);
      entry->file[copy] = '\0';
    } else {
      entry->file[0] = '\0';
    }
    STAT_INC(truncated_count);
#else
    char *large_file = malloc(file_len + 1);
    if (!large_file) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    memcpy(large_file, file, file_len + 1);
    entry->has_large_file = true;
    *(char**)entry->file = large_file;
#endif
  } else {
    entry->has_large_file = false;
    if (file) memcpy(entry->file, file, file_len + 1);
    else entry->file[0] = '\0';
  }

  size_t body_len = body ? strlen(body) : 0;
#if LOG_FEATURE_KV
  size_t kv_len = kv_blob_size(kv);   /* self-describing; may contain NULs */
#else
  size_t kv_len = 0;
  (void)kv;
#endif
#if LOG_FEATURE_STATIC_ALLOC
  entry->has_large_msg = false;
  if (body_len >= sizeof(entry->msg)) {
    body_len = sizeof(entry->msg) - 1;
    STAT_INC(truncated_count);
  }
  if (body) memcpy(entry->msg, body, body_len);
  entry->msg[body_len] = '\0';
#if LOG_FEATURE_KV
  if (kv_len > sizeof(entry->kv_storage)) {
    /* The blob is self-describing and cannot be split: drop it whole. */
    entry->has_kv = false;
    STAT_INC(truncated_count);
  } else {
    entry->has_kv = kv_len > 0;
    if (kv_len) memcpy(entry->kv_storage, kv, kv_len);
  }
#endif
#else
  if (kv_len > 0 || body_len >= sizeof(entry->msg)) {
    char *combined = malloc(body_len + 1 + kv_len + 1);
    if (!combined) {
      if (entry->has_large_file) {
        free(*(char**)entry->file);
        entry->has_large_file = false;
      }
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    if (body_len) memcpy(combined, body, body_len);
    combined[body_len] = '\0';
    if (kv_len) memcpy(combined + body_len + 1, kv, kv_len);
    combined[body_len + 1 + kv_len] = '\0';
    entry->has_large_msg = true;
#if LOG_FEATURE_KV
    entry->has_kv = kv_len > 0;
#endif
    *(char**)entry->msg = combined;
  } else {
    entry->has_large_msg = false;
    if (body) memcpy(entry->msg, body, body_len);
    entry->msg[body_len] = '\0';
#if LOG_FEATURE_KV
    entry->has_kv = false;
#endif
  }
#endif

  entry->level = level;
  entry->line = line;
  entry->timestamp = timestamp;
  atomic_store(&rq->tail, tail + 1);
#if defined(LOG_PLATFORM_POSIX)
  pthread_cond_signal(&rq->cond);
  pthread_mutex_unlock(&rq->mtx);
#else
  WakeConditionVariable(&rq->cond);
  LeaveCriticalSection(&rq->mtx);
#endif
  return true;
}
#endif /* LOG_FEATURE_KV || LOG_FEATURE_FILTER */

#if LOG_FEATURE_KV
/* Backwards-compatible name for the kv path. */
static bool ring_queue_push_kv(log_ring_queue *rq, const char *body, const char *kv,
                               const char *file, int level, int line,
                               double timestamp, bool blocking) {
  return ring_queue_push_body(rq, body, kv, file, level, line, timestamp, blocking);
}
#endif /* LOG_FEATURE_KV */
#endif /* LOG_FEATURE_ASYNC */

/* Batch drain state for the async writer thread (on its stack). */
typedef struct {
  log_ring_entry entries[LOG_DRAIN_BATCH];
  char *large_msg[LOG_DRAIN_BATCH];
  char *large_file[LOG_DRAIN_BATCH];
  int count;
} log_drain_batch;

/* Wait for at least one entry, then drain up to LOG_DRAIN_BATCH entries in a
 * single lock acquisition. Returns:
 *   >0  number of entries filled into out (ownership of large-message
 *       pointers transfers to out->large_msg / out->large_file)
 *    0  queue closed and empty (writer thread should exit)
 *   -1  timeout_ms elapsed with no entry (flush INTERVAL handlers and retry)
 * The per-entry heap strdup of the old single-pop is gone: inline buffers
 * are copied into the batch instead, and one wakeup serves a whole batch. */
#if LOG_FEATURE_ASYNC
static int ring_queue_pop_batch(log_ring_queue *rq, log_drain_batch *out, int timeout_ms) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&rq->mtx);
  size_t head = atomic_load(&rq->head);
  while (head == atomic_load(&rq->tail)) {
    if (rq->closed) {
      pthread_mutex_unlock(&rq->mtx);
      return 0;
    }
    if (timeout_ms >= 0) {
      struct timespec deadline;
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += timeout_ms / 1000;
      deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
      if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
      }
      if (pthread_cond_timedwait(&rq->cond, &rq->mtx, &deadline) == ETIMEDOUT) {
        pthread_mutex_unlock(&rq->mtx);
        return -1;
      }
    } else {
      pthread_cond_wait(&rq->cond, &rq->mtx);
    }
    head = atomic_load(&rq->head);
  }
  size_t tail = atomic_load(&rq->tail);
  size_t avail = tail - head;
  if (avail > (size_t)LOG_DRAIN_BATCH) avail = (size_t)LOG_DRAIN_BATCH;
  for (size_t k = 0; k < avail; k++) {
    log_ring_entry *e = &rq->buffer[(head + k) & rq->mask];
    out->entries[k] = *e;
    if (e->has_large_msg) {
      out->large_msg[k] = *(char**)e->msg;
      e->has_large_msg = false;
    } else {
      out->large_msg[k] = NULL;
    }
    if (e->has_large_file) {
      out->large_file[k] = *(char**)e->file;
      e->has_large_file = false;
    } else {
      out->large_file[k] = NULL;
    }
  }
  atomic_store(&rq->head, head + avail);
  pthread_mutex_unlock(&rq->mtx);
  for (size_t k = 0; k < avail; k++) {
    pthread_cond_signal(&rq->space_cond);
  }
  out->count = (int)avail;
  return (int)avail;
#else
  EnterCriticalSection(&rq->mtx);
  size_t head = atomic_load(&rq->head);
  while (head == atomic_load(&rq->tail)) {
    if (rq->closed) {
      LeaveCriticalSection(&rq->mtx);
      return 0;
    }
    DWORD wait_ms = timeout_ms >= 0 ? (DWORD)timeout_ms : INFINITE;
    if (!SleepConditionVariableCS(&rq->cond, &rq->mtx, wait_ms)) {
      LeaveCriticalSection(&rq->mtx);
      return -1;
    }
    head = atomic_load(&rq->head);
  }
  size_t tail = atomic_load(&rq->tail);
  size_t avail = tail - head;
  if (avail > (size_t)LOG_DRAIN_BATCH) avail = (size_t)LOG_DRAIN_BATCH;
  for (size_t k = 0; k < avail; k++) {
    log_ring_entry *e = &rq->buffer[(head + k) & rq->mask];
    out->entries[k] = *e;
    if (e->has_large_msg) {
      out->large_msg[k] = *(char**)e->msg;
      e->has_large_msg = false;
    } else {
      out->large_msg[k] = NULL;
    }
    if (e->has_large_file) {
      out->large_file[k] = *(char**)e->file;
      e->has_large_file = false;
    } else {
      out->large_file[k] = NULL;
    }
  }
  atomic_store(&rq->head, head + avail);
  LeaveCriticalSection(&rq->mtx);
  for (size_t k = 0; k < avail; k++) {
    WakeConditionVariable(&rq->space_cond);
  }
  out->count = (int)avail;
  return (int)avail;
#endif
}
#endif /* LOG_FEATURE_ASYNC */

static void ring_queue_shutdown(log_ring_queue *rq) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&rq->mtx);
  rq->closed = true;
  pthread_cond_broadcast(&rq->cond);
  pthread_cond_broadcast(&rq->space_cond);
  pthread_mutex_unlock(&rq->mtx);
#else
  EnterCriticalSection(&rq->mtx);
  rq->closed = true;
  WakeAllConditionVariable(&rq->cond);
  WakeAllConditionVariable(&rq->space_cond);
  LeaveCriticalSection(&rq->mtx);
#endif
}

#endif /* LOG_FEATURE_RING_QUEUE */

/* ==================== Formatting helpers ==================== */

/* Format helpers: the message is formatted exactly once, then shared by all handlers.
 * Sets *heap_owned when the caller must free the returned pointer; otherwise the
 * buffer belongs to the event (thread-local storage) and must not be freed. */
static char* format_message(log_event *ev, bool *heap_owned) {
  *heap_owned = false;
  if (ev->raw_msg) {
    return (char*)ev->raw_msg;
  }
  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int len = vsnprintf(NULL, 0, ev->fmt, args_copy);
  va_end(args_copy);
  if (len < 0) return NULL;
  char *msg = malloc((size_t)len + 1);
  if (!msg) return NULL;
  vsnprintf(msg, (size_t)len + 1, ev->fmt, ev->ap);
  *heap_owned = true;
  return msg;
}

/* Clamp a caller-supplied level into the valid range so that out-of-range
 * values (buggy callers, hand-built events) can never index the
 * level_strings/level_colors arrays. Negative -> TRACE, >= LOG_LEVELS -> FATAL. */
static int clamp_level(int level) {
  if (level < LOG_TRACE) return LOG_TRACE;
  if (level > LOG_FATAL) return LOG_FATAL;
  return level;
}

/* Build the line prefix (time, level, optional thread id, file:line, custom formatter).
 * handler_fmt is the per-handler formatter (NULL = fall back to ctx->format_fn).
 * Returns the number of characters written into buf (excluding NUL). */
static int format_prefix(log_handle *ctx, log_event *ev, log_FormatFn handler_fmt,
                         char *buf, size_t buf_size, bool show_tid, bool use_color) {
  log_FormatFn fmt = handler_fmt ? handler_fmt : (ctx ? ctx->format_fn : NULL);
  if (fmt) {
    int n = fmt(ctx, ev, buf, buf_size);
    return n < 0 ? 0 : n;
  }
  char time_buf[64];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf),
                   ctx && ctx->enable_ts_cache);
  int lvl = clamp_level(ev->level);
#ifdef LOG_USE_COLOR
  if (use_color) {
    if (show_tid) {
      return snprintf(buf, buf_size, "%s %s%-5s\x1b[0m \x1b[90m[%lu] %s:%d:\x1b[0m ",
                      time_buf, level_colors[lvl], level_strings[lvl],
                      LOG_GET_THREAD_ID(), ev->file ? ev->file : "", ev->line);
    }
    return snprintf(buf, buf_size, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
                    time_buf, level_colors[lvl], level_strings[lvl],
                    ev->file ? ev->file : "", ev->line);
  }
#else
  (void)use_color;
#endif
  if (show_tid) {
    return snprintf(buf, buf_size, "%s %-5s [%lu] %s:%d: ",
                    time_buf, level_strings[lvl], LOG_GET_THREAD_ID(),
                    ev->file ? ev->file : "", ev->line);
  }
  return snprintf(buf, buf_size, "%s %-5s %s:%d: ",
                  time_buf, level_strings[lvl],
                  ev->file ? ev->file : "", ev->line);
}

/* ==================== Typed key-value metadata (B2) ==================== */

#if LOG_FEATURE_KV
/* Encoded blob layout: a 4-byte big-endian record-stream length, then records
 *   [type:1][key_len:1][key][val_len:2 big-endian][val]
 * INT/DOUBLE/BOOL carry an ASCII decimal/bool value; STR carries raw bytes.
 * The stream length makes the blob self-describing: it may contain NUL bytes
 * (the value-length high byte), so it is never treated as a C string. */
static size_t kv_encode(const log_kv *kvs, int count, char *dst, size_t dst_size,
                        bool *truncated) {
  size_t used = 4;   /* reserve the length header */
  int pairs = 0;
  for (int i = 0; i < count; i++) {
    const char *key = kvs[i].key;
    if (!key) continue;                 /* NULL key skips the pair */
    if (pairs >= LOG_KV_MAX_PAIRS) { *truncated = true; break; }

    size_t klen = strlen(key);
    if (klen > 255) { klen = 255; *truncated = true; }

    char vbuf[64];
    const char *val;
    size_t vlen;
    switch (kvs[i].type) {
      case LOG_KV_T_INT:
        snprintf(vbuf, sizeof(vbuf), "%lld", kvs[i].i);
        val = vbuf; vlen = strlen(vbuf);
        break;
      case LOG_KV_T_DOUBLE:
        snprintf(vbuf, sizeof(vbuf), "%.17g", kvs[i].d);
        val = vbuf; vlen = strlen(vbuf);
        break;
      case LOG_KV_T_BOOL:
        val = kvs[i].i ? "true" : "false"; vlen = strlen(val);
        break;
      default:  /* LOG_KV_T_STR */
        val = kvs[i].s ? kvs[i].s : ""; vlen = strlen(val);
        break;
    }
    if (vlen > 65535) { vlen = 65535; *truncated = true; }

    size_t need = 1 + 1 + klen + 2 + vlen;
    if (used + need > dst_size) { *truncated = true; break; }

    size_t p = used;
    dst[p++] = (char)kvs[i].type;
    dst[p++] = (char)klen;
    memcpy(dst + p, key, klen); p += klen;
    dst[p++] = (char)((vlen >> 8) & 0xFF);
    dst[p++] = (char)(vlen & 0xFF);
    memcpy(dst + p, val, vlen); p += vlen;
    used = p;
    pairs++;
  }
  if (pairs == 0) return 0;            /* no pairs: signal "no kv" to caller */
  size_t stream = used - 4;
  dst[0] = (char)((stream >> 24) & 0xFF);
  dst[1] = (char)((stream >> 16) & 0xFF);
  dst[2] = (char)((stream >> 8) & 0xFF);
  dst[3] = (char)(stream & 0xFF);
  return used;
}

/* Total byte size of a self-describing blob (header + records). */
static size_t kv_blob_size(const char *blob) {
  if (!blob) return 0;
  return 4 + (((size_t)(unsigned char)blob[0] << 24) |
              ((size_t)(unsigned char)blob[1] << 16) |
              ((size_t)(unsigned char)blob[2] << 8) |
              (size_t)(unsigned char)blob[3]);
}

/* Bounded string sink: len always reports the required length (snprintf
 * semantics), while at most cap-1 bytes plus NUL are stored. */
typedef struct {
  char *dst;
  size_t cap;
  size_t len;
} kv_sink;

static void kv_sink_putc(kv_sink *s, char c) {
  if (s->dst && s->len + 1 < s->cap) s->dst[s->len] = c;
  s->len++;
}

static void kv_sink_write(kv_sink *s, const char *p, size_t n) {
  if (s->dst && s->len < s->cap) {
    size_t room = (s->cap - 1) - s->len;
    size_t w = n < room ? n : room;
    if (w) memcpy(s->dst + s->len, p, w);
  }
  s->len += n;
}

static void kv_sink_finish(kv_sink *s) {
  if (s->dst && s->cap) s->dst[s->len < s->cap ? s->len : s->cap - 1] = '\0';
}

#if LOG_FEATURE_JSON
static void kv_sink_json_str(kv_sink *s, const char *p, size_t n) {
  kv_sink_putc(s, '"');
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    switch (c) {
      case '"':  kv_sink_putc(s, '\\'); kv_sink_putc(s, '"');  break;
      case '\\': kv_sink_putc(s, '\\'); kv_sink_putc(s, '\\'); break;
      case '\n': kv_sink_putc(s, '\\'); kv_sink_putc(s, 'n');  break;
      case '\r': kv_sink_putc(s, '\\'); kv_sink_putc(s, 'r');  break;
      case '\t': kv_sink_putc(s, '\\'); kv_sink_putc(s, 't');  break;
      case '\b': kv_sink_putc(s, '\\'); kv_sink_putc(s, 'b');  break;
      case '\f': kv_sink_putc(s, '\\'); kv_sink_putc(s, 'f');  break;
      default:
        if (c < 0x20) {
          char u[8];
          snprintf(u, sizeof(u), "\\u%04x", c);
          kv_sink_write(s, u, 6);
        } else {
          kv_sink_putc(s, (char)c);
        }
        break;
    }
  }
  kv_sink_putc(s, '"');
}
#endif /* LOG_FEATURE_JSON */

/* Decode one record; returns the pointer past it, or NULL on malformed input. */
static const char* kv_next(const char *p, const char *end, int *type,
                           const char **key, size_t *klen,
                           const char **val, size_t *vlen) {
  if (p + 2 > end) return NULL;
  *type = (unsigned char)p[0];
  *klen = (unsigned char)p[1];
  p += 2;
  if (p + *klen + 2 > end) return NULL;
  *key = p; p += *klen;
  *vlen = ((size_t)(unsigned char)p[0] << 8) | (size_t)(unsigned char)p[1];
  p += 2;
  if (p + *vlen > end) return NULL;
  *val = p; p += *vlen;
  return p;
}

/* Render " key=value ..." (text form). Returns needed length (excluding NUL). */
static size_t kv_render_text(const char *blob, char *dst, size_t cap) {
  kv_sink s = { dst, cap, 0 };
  if (blob) {
    size_t total = kv_blob_size(blob);
    const char *p = blob + 4;
    const char *end = blob + total;
    int type; const char *key, *val; size_t klen, vlen;
    while ((p = kv_next(p, end, &type, &key, &klen, &val, &vlen)) != NULL) {
      kv_sink_putc(&s, ' ');
      kv_sink_write(&s, key, klen);
      kv_sink_putc(&s, '=');
      kv_sink_write(&s, val, vlen);
    }
  }
  kv_sink_finish(&s);
  return s.len;
}

#if LOG_FEATURE_JSON
/* Render ', "key": value, ...' (JSON object fragment). Needed length excl NUL. */
static size_t kv_render_json(const char *blob, char *dst, size_t cap) {
  kv_sink s = { dst, cap, 0 };
  if (blob) {
    size_t total = kv_blob_size(blob);
    const char *p = blob + 4;
    const char *end = blob + total;
    int type; const char *key, *val; size_t klen, vlen;
    while ((p = kv_next(p, end, &type, &key, &klen, &val, &vlen)) != NULL) {
      kv_sink_write(&s, ", ", 2);
      kv_sink_json_str(&s, key, klen);
      kv_sink_write(&s, ": ", 2);
      if (type == LOG_KV_T_INT || type == LOG_KV_T_DOUBLE || type == LOG_KV_T_BOOL) {
        kv_sink_write(&s, val, vlen);
      } else {
        kv_sink_json_str(&s, val, vlen);
      }
    }
  }
  kv_sink_finish(&s);
  return s.len;
}
#endif /* LOG_FEATURE_JSON */
#endif /* LOG_FEATURE_KV */

/* ==================== Stream buffering & durability ==================== */

#if LOG_FEATURE_FILE_OPS
/* Fully buffer files opened by the library: fewer write() syscalls per
 * message. Only valid before the first I/O on the stream, so it is used
 * exclusively for files we open ourselves (log_add_file, rotation
 * reopen). Skipped in static mode (setvbuf would heap-allocate). */
static void log_setup_stream_buffer(FILE *fp) {
#if !LOG_FEATURE_STATIC_ALLOC
  setvbuf(fp, NULL, _IOFBF, LOG_FILE_BUF_SIZE);
#else
  (void)fp;
#endif
}
#endif /* LOG_FEATURE_FILE_OPS */

/* fflush + durable sync of a stdio stream (fsync on POSIX, _commit on Windows). */
static void log_flush_fsync(FILE *fp) {
  fflush(fp);
#if defined(LOG_PLATFORM_POSIX)
  fsync(fileno(fp));
#else
  _commit(_fileno(fp));
#endif
}

static double monotonic_seconds(void) {
  return get_timestamp_with_clock(LOG_CLOCK_MONOTONIC);
}

/* Apply the per-handler flush policy after one message was written.
 * INTERVAL bookkeeping races benignly between writer threads: worst case
 * is a duplicate flush or one interval of extra delay. The first write
 * only starts the interval clock; the flush itself happens interval_ms
 * later (on a later write, or via the async writer timer). */
static void log_apply_flush(log_handle *ctx, int handler_idx) {
  log_handler *h = &ctx->handlers[handler_idx];
  FILE *fp = h->fp;
  if (!fp) return;
  if (h->flush_policy == LOG_FLUSH_EVERY) {
    fflush(fp);
    if (h->flush_fsync) log_flush_fsync(fp);
  } else if (h->flush_policy == LOG_FLUSH_INTERVAL && h->flush_interval_ms > 0) {
    double now = monotonic_seconds();
    if (h->last_flush == 0.0) {
      h->last_flush = now;   /* baseline only: nothing withheld yet */
    } else if (now - h->last_flush >= (double)h->flush_interval_ms / 1000.0) {
      h->last_flush = now;
      fflush(fp);
      if (h->flush_fsync) log_flush_fsync(fp);
    }
  }
}

#if LOG_FEATURE_ASYNC
/* Flush handlers whose INTERVAL deadline has passed (async writer timer). */
static void log_flush_due_intervals(log_handle *ctx) {
  double now = monotonic_seconds();
  for (int i = 0; i < ctx->handler_count; i++) {
    log_handler *h = &ctx->handlers[i];
    if (!h->fp || h->flush_policy != LOG_FLUSH_INTERVAL || h->flush_interval_ms == 0) continue;
    if (h->last_flush == 0.0 ||
        now - h->last_flush >= (double)h->flush_interval_ms / 1000.0) {
      h->last_flush = now;
      fflush(h->fp);
      if (h->flush_fsync) log_flush_fsync(h->fp);
    }
  }
}

/* Smallest pending INTERVAL in ms, or -1 when no handler needs timed flushes
 * (the async writer then waits indefinitely). */
static int log_next_flush_timeout_ms(log_handle *ctx) {
  int best = -1;
  for (int i = 0; i < ctx->handler_count; i++) {
    log_handler *h = &ctx->handlers[i];
    if (h->active && h->fp && h->flush_policy == LOG_FLUSH_INTERVAL &&
        h->flush_interval_ms > 0) {
      int ms = (int)h->flush_interval_ms;
      if (best < 0 || ms < best) best = ms;
    }
  }
  return best;
}
#endif /* LOG_FEATURE_ASYNC */

/* ==================== Crash-safe mode ==================== */
#if LOG_FEATURE_CRASH_MODE

#define LOG_CRASH_FD_MAX 16
static struct {
  int fds[LOG_CRASH_FD_MAX];
  int count;
} crash_targets;

/* (Re)collect the raw fds of all active file-backed handlers of ctx.
 * fds are written first and count afterwards, so a fatal signal arriving
 * mid-update observes a consistent target set (old or new). */
static void crash_refresh_targets(log_handle *ctx) {
  int n = 0;
  for (int i = 0; i < ctx->handler_count && n < LOG_CRASH_FD_MAX; i++) {
    FILE *fp = ctx->handlers[i].fp;
    if (!fp || !ctx->handlers[i].active) continue;
    int fd = fileno(fp);
    if (fd < 0) continue;
    bool dup = false;
    for (int j = 0; j < n; j++) {
      if (crash_targets.fds[j] == fd) { dup = true; break; }
    }
    if (!dup) crash_targets.fds[n++] = fd;
  }
  crash_targets.count = n;
}

#if LOG_FEATURE_MEMORY_HANDLER
/* Registry of flight-recorder stores to dump from the fatal-signal handler.
 * Pointer written before count, so a signal arriving mid-registration sees
 * either the old or the new set. Bounded; stores beyond the cap are simply
 * not dumped on crash. */
#define LOG_CRASH_MEM_MAX 8
static struct {
  const log_memory_store *stores[LOG_CRASH_MEM_MAX];
  int count;
} crash_memory;

static void crash_memory_register(const log_memory_store *st) {
  if (crash_memory.count >= LOG_CRASH_MEM_MAX) return;
  crash_memory.stores[crash_memory.count] = st;
  crash_memory.count++;
}

static void crash_memory_unregister(const log_memory_store *st) {
  for (int i = 0; i < crash_memory.count; i++) {
    if (crash_memory.stores[i] == st) {
      crash_memory.stores[i] = crash_memory.stores[crash_memory.count - 1];
      crash_memory.count--;
      return;
    }
  }
}

#if defined(LOG_PLATFORM_POSIX)
/* Async-signal-safe tail dump: walk each registered ring and write(2) the
 * retained lines to the crash target fds. No locks, no allocation; a store
 * concurrently mid-write may expose at most one inconsistent entry, which is
 * acceptable for a dying process. */
static void crash_memory_dump(void) {
  static const char header[] = "==== log: flight recorder tail ====\n";
  for (int s = 0; s < crash_memory.count; s++) {
    const log_memory_store *st = crash_memory.stores[s];
    if (!st || !st->entries) continue;
    size_t cap = st->capacity;
    if (cap == 0) continue;
    size_t count = st->count;
    if (count > cap) count = cap;
    size_t start = (count < cap) ? 0 : st->head;   /* oldest entry */
    for (int i = 0; i < crash_targets.count; i++) {
      ssize_t r = write(crash_targets.fds[i], header, sizeof(header) - 1);
      (void)r;
    }
    for (size_t k = 0; k < count; k++) {
      const log_memory_entry *e = &st->entries[(start + k) % cap];
      int len = e->len;
      if (len <= 0) continue;
      if ((size_t)len > LOG_MEMORY_LINE_MAX) len = (int)LOG_MEMORY_LINE_MAX;
      for (int i = 0; i < crash_targets.count; i++) {
        ssize_t r = write(crash_targets.fds[i], e->line, (size_t)len);
        (void)r;
      }
    }
  }
}
#endif /* LOG_PLATFORM_POSIX */
#endif /* LOG_FEATURE_MEMORY_HANDLER */

#if defined(LOG_PLATFORM_POSIX)
/* Async-signal-safe marker line: constant text plus a hand-rolled integer
 * conversion. Only write(2)/memcpy/stack are used here. */
static void crash_marker_write(int sig) {
  char buf[80];
  size_t n = 0;
  static const char prefix[] = "==== log: fatal signal ";
  static const char suffix[] = " caught, process dying ====\n";
  memcpy(buf, prefix, sizeof(prefix) - 1);
  n += sizeof(prefix) - 1;
  char digits[12];
  size_t d = 0;
  int v = sig < 0 ? -sig : sig;
  do { digits[d++] = (char)('0' + (v % 10)); v /= 10; } while (v);
  while (d) buf[n++] = digits[--d];
  memcpy(buf + n, suffix, sizeof(suffix) - 1);
  n += sizeof(suffix) - 1;
  for (int i = 0; i < crash_targets.count; i++) {
    ssize_t r = write(crash_targets.fds[i], buf, n);
    (void)r;
  }
}

/* Installed with SA_RESETHAND: the disposition is already back to SIG_DFL
 * while this handler runs, so a nested fault kills the process instead of
 * recursing. */
static void crash_signal_handler(int sig) {
  crash_marker_write(sig);
#if LOG_FEATURE_MEMORY_HANDLER
  crash_memory_dump();
#endif
  kill(getpid(), sig);
}
#endif /* LOG_PLATFORM_POSIX */

#endif /* LOG_FEATURE_CRASH_MODE */

/* File rotation */
#if LOG_FEATURE_FILE_OPS
static void rotate_file(log_handle *ctx, const char *filename) {
  if (!ctx->file_prefix) return;
  
  char old_path[512];
  snprintf(old_path, sizeof(old_path), "%s.%d", ctx->file_prefix, LOG_MAX_ROTATION_FILES - 1);
  remove(old_path);
  
  for (int i = LOG_MAX_ROTATION_FILES - 2; i >= 1; i--) {
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s.%d", ctx->file_prefix, i);
    snprintf(dst, sizeof(dst), "%s.%d", ctx->file_prefix, i + 1);
    rename(src, dst);
  }
  
  char new_path[512];
  snprintf(new_path, sizeof(new_path), "%s.1", ctx->file_prefix);
  rename(filename, new_path);

  STAT_INC(rotation_count);
}
#endif /* LOG_FEATURE_FILE_OPS */

/* Output handlers */
static void stdout_handler(log_handle *ctx, log_event *ev) {
  bool heap_msg = false;
  char *msg = format_message(ev, &heap_msg);
  if (!msg) return;

  bool show_tid = false;
  log_FormatFn handler_fmt = NULL;
  if (ctx) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (!ctx->handlers[i].active || ctx->handlers[i].udata != ev->udata) continue;
      if (ctx->handlers[i].show_thread_id) show_tid = true;
      if (ctx->handlers[i].format_fn) handler_fmt = ctx->handlers[i].format_fn;
      if (show_tid && handler_fmt) break;
    }
  }

  char prefix[512];
  format_prefix(ctx, ev, handler_fmt, prefix, sizeof(prefix), show_tid,
#ifdef LOG_USE_COLOR
                true
#else
                false
#endif
               );
#if LOG_FEATURE_KV
  char kv_buf[LOG_KV_TEXT_RENDER_MAX];
  const char *kv_text = "";
  if (ev->kv) {
    kv_render_text(ev->kv, kv_buf, sizeof(kv_buf));
    kv_text = kv_buf;
  }
  fprintf(ev->udata, "%s%s%s\n", prefix, msg, kv_text);
#else
  fprintf(ev->udata, "%s%s\n", prefix, msg);
#endif
  if (heap_msg) free(msg);
}

static void file_handler_internal(log_handle *ctx, log_event *ev, int handler_idx) {
  bool heap_msg = false;
  char *msg = format_message(ev, &heap_msg);
  if (!msg) return;

  log_handler *h = &ctx->handlers[handler_idx];
  char prefix[512];
  int prefix_len = format_prefix(ctx, ev, h->format_fn, prefix, sizeof(prefix),
                                 h->show_thread_id, false);
  if (prefix_len < 0) { if (heap_msg) free(msg); return; }
  if ((size_t)prefix_len >= sizeof(prefix)) prefix_len = (int)sizeof(prefix) - 1;

  size_t msg_len = strlen(msg);
#if LOG_FEATURE_KV
  char kv_buf[LOG_KV_TEXT_RENDER_MAX];
  const char *kv_text = "";
  size_t kv_len = 0;
  if (ev->kv) {
    kv_len = kv_render_text(ev->kv, kv_buf, sizeof(kv_buf));
    kv_text = kv_buf;
  }
#else
  const char *kv_text = "";
  size_t kv_len = 0;
#endif
  FILE *fp = h->fp;
  /* Assemble prefix+msg+kv+'\n' and write with a SINGLE fwrite: stdio holds
   * the stream lock for the whole line, so concurrent writers cannot tear
   * lines apart. The common case assembles into thread-local storage (no
   * heap); oversized lines fall back to an exact-size malloc. */
  size_t total = (size_t)prefix_len + msg_len + kv_len + 1;
  static LOG_THREAD_LOCAL char tl_line_buf[512 + LOG_MSG_BUF_SIZE +
#if LOG_FEATURE_KV
                                           LOG_KV_TEXT_RENDER_MAX +
#endif
                                           2];
  size_t written;
  if (total <= sizeof(tl_line_buf)) {
    memcpy(tl_line_buf, prefix, (size_t)prefix_len);
    memcpy(tl_line_buf + prefix_len, msg, msg_len);
    if (kv_len) memcpy(tl_line_buf + prefix_len + msg_len, kv_text, kv_len);
    tl_line_buf[prefix_len + msg_len + kv_len] = '\n';
    written = fwrite(tl_line_buf, 1, total, fp);
  } else {
    char *buf = malloc(total);
    if (!buf) { if (heap_msg) free(msg); return; }
    memcpy(buf, prefix, (size_t)prefix_len);
    memcpy(buf + prefix_len, msg, msg_len);
    if (kv_len) memcpy(buf + prefix_len + msg_len, kv_text, kv_len);
    buf[prefix_len + msg_len + kv_len] = '\n';
    written = fwrite(buf, 1, total, fp);
    free(buf);
  }

#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->file_mtx);
#else
  EnterCriticalSection(&ctx->file_mtx);
#endif
  ctx->handlers[handler_idx].file_size += written;

#if LOG_FEATURE_FILE_OPS
  if (ctx->handlers[handler_idx].file_size >= ctx->max_file_size) {
    if (ctx->handlers[handler_idx].owns_file && ctx->file_prefix) {
      fflush(fp);
      if (ctx->handlers[handler_idx].flush_fsync) log_flush_fsync(fp);
      if (fp != stderr && fp != stdout) {
        fclose(fp);
      }
      /* Clear both references before reopening: if fopen() fails the handler
       * must not keep a pointer to the now-closed FILE. */
      ctx->handlers[handler_idx].fp = NULL;
      ctx->handlers[handler_idx].udata = NULL;
      rotate_file(ctx, ctx->file_prefix);
      FILE *reopened = fopen(ctx->file_prefix, "a");
      if (reopened) {
        log_setup_stream_buffer(reopened);
        ctx->handlers[handler_idx].fp = reopened;
        ctx->handlers[handler_idx].udata = reopened;
        ctx->handlers[handler_idx].file_size = 0;
#if LOG_FEATURE_CRASH_MODE
        crash_refresh_targets(ctx);
#endif
      }
    } else {
      ctx->handlers[handler_idx].file_size = 0;
    }
  }
#endif /* LOG_FEATURE_FILE_OPS */
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->file_mtx);
#else
  LeaveCriticalSection(&ctx->file_mtx);
#endif

  if (heap_msg) free(msg);
}

static void file_handler_wrapper(log_handle *ctx, log_event *ev) {
  FILE *target_fp = ev->udata;
  /* A file handler whose reopen failed clears udata: never write to a
   * closed/freed FILE. */
  if (!target_fp) return;

  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].udata == target_fp && ctx->handlers[i].fp) {
      file_handler_internal(ctx, ev, i);
      return;
    }
  }

  bool heap_fb = false;
  char *msg = format_message(ev, &heap_fb);
  if (msg) {
    fprintf(target_fp, "%s\n", msg);
    if (heap_fb) free(msg);
  }
}

#if LOG_FEATURE_MEMORY_HANDLER
/* In-memory flight recorder (B1). Render the complete line (prefix + body +
 * kv suffix + '\n') once and append it to the handler's ring. The store lock
 * serializes concurrent producers: the ctx rwlock is only a read lock during
 * dispatch, so handlers must guard their own mutable state. */
static void memory_handler(log_handle *ctx, log_event *ev) {
  log_memory_store *st = (log_memory_store*)ev->udata;
  if (!st || !st->entries || st->capacity == 0) return;

  bool heap_msg = false;
  char *msg = format_message(ev, &heap_msg);
  if (!msg) return;

  log_FormatFn handler_fmt = NULL;
  bool show_tid = false;
  if (ctx) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].kind == HANDLER_MEMORY && ctx->handlers[i].udata == st) {
        handler_fmt = ctx->handlers[i].format_fn;
        show_tid = ctx->handlers[i].show_thread_id;
        break;
      }
    }
  }

  char prefix[512];
  int plen = format_prefix(ctx, ev, handler_fmt, prefix, sizeof(prefix), show_tid, false);
  if (plen < 0) plen = 0;
  if ((size_t)plen >= sizeof(prefix)) plen = (int)sizeof(prefix) - 1;

#if LOG_FEATURE_KV
  char kv_buf[LOG_KV_TEXT_RENDER_MAX];
  const char *kv_text = "";
  if (ev->kv) {
    kv_render_text(ev->kv, kv_buf, sizeof(kv_buf));
    kv_text = kv_buf;
  }
#else
  const char *kv_text = "";
#endif

  char line[LOG_MEMORY_LINE_MAX];
  int n = snprintf(line, sizeof(line), "%s%s%s\n", prefix, msg, kv_text);
  if (heap_msg) free(msg);
  if (n < 0) return;

  size_t len = (size_t)n;
  if (len >= sizeof(line)) {
    /* Truncated: force a terminating '\n' so every dumped record is still a
     * complete, newline-terminated line (and crash write(2) emits whole
     * lines). The final byte is therefore always '\n' when len > 0. */
    len = sizeof(line) - 1;
    line[len - 1] = '\n';
    line[len] = '\0';
    STAT_INC(truncated_count);
  }

#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&st->mtx);
#else
  EnterCriticalSection(&st->mtx);
#endif
  log_memory_entry *e = &st->entries[st->head];
  memcpy(e->line, line, len);
  e->line[len] = '\0';
  e->len = (int)len;
  e->level = ev->level;
  st->head = (st->head + 1) % st->capacity;
  if (st->count < st->capacity) st->count++;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&st->mtx);
#else
  LeaveCriticalSection(&st->mtx);
#endif
}
#endif /* LOG_FEATURE_MEMORY_HANDLER */

#if LOG_FEATURE_JSON
/* Escape a NUL-terminated string for use inside a JSON string literal.
 * Handles the two mandatory escapes plus all control characters
 * (U+0000..U+001F) as \uXXXX, so arbitrary log payloads stay valid JSON.
 * Returns a malloc'd buffer, or NULL on allocation failure. */
static char* json_escape(const char *src) {
  if (!src) src = "";
  size_t n = strlen(src);
  char *out = malloc(n * 6 + 1);  /* worst case: 6 bytes per input byte */
  if (!out) return NULL;
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)src[i];
    switch (c) {
      case '"':  out[j++] = '\\'; out[j++] = '"';  break;
      case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
      case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
      case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
      case '\t': out[j++] = '\\'; out[j++] = 't';  break;
      case '\b': out[j++] = '\\'; out[j++] = 'b';  break;
      case '\f': out[j++] = '\\'; out[j++] = 'f';  break;
      default:
        if (c < 0x20) {
          j += (size_t)snprintf(out + j, 7, "\\u%04x", c);
        } else {
          out[j++] = (char)c;
        }
        break;
    }
  }
  out[j] = '\0';
  return out;
}

static void json_handler(log_handle *ctx, log_event *ev) {
  /* Per-handler formatter wins; ctx-level formatter is fallback. */
  if (ctx && ev->udata) {
    for (int i = 0; i < ctx->handler_count; i++) {
      log_handler *h = &ctx->handlers[i];
      if (h->active && h->udata == ev->udata && h->format_fn) {
        char prefix[512];
        int n = h->format_fn(ctx, ev, prefix, sizeof(prefix));
        if (n > 0) {
          fprintf(ev->udata, "%s\n", prefix);
          return;
        }
        break;
      }
    }
  }
  if (ctx && ctx->format_fn) {
    char prefix[512];
    int n = ctx->format_fn(ctx, ev, prefix, sizeof(prefix));
    if (n > 0) {
      fprintf(ev->udata, "%s\n", prefix);
      return;
    }
  }

  bool heap_msg = false;
  char *msg = format_message(ev, &heap_msg);
  if (!msg) return;

  char time_buf[64];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf),
                   ctx && ctx->enable_ts_cache);

  char *escaped_msg = json_escape(msg);
  if (heap_msg) free(msg);
  if (!escaped_msg) return;

  char *escaped_file = json_escape(ev->file ? ev->file : "");
  if (!escaped_file) { free(escaped_msg); return; }

  bool show_tid = false;
  if (ctx) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active && ctx->handlers[i].udata == ev->udata) {
        show_tid = true;
        break;
      }
    }
  }

  const char *lvl_str = level_strings[clamp_level(ev->level)];
  int line_val = ev->line;

#if LOG_FEATURE_KV
  size_t kv_len = ev->kv ? kv_render_json(ev->kv, NULL, 0) : 0;
#endif

  size_t head;
  if (show_tid) {
    head = snprintf(NULL, 0,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"thread_id\": %lu, \"message\": \"%s\"",
      time_buf, lvl_str, escaped_file, line_val, LOG_GET_THREAD_ID(), escaped_msg);
  } else {
    head = snprintf(NULL, 0,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"",
      time_buf, lvl_str, escaped_file, line_val, escaped_msg);
  }

#if LOG_FEATURE_KV
  size_t needed = head + kv_len;
#else
  size_t needed = head;
#endif

  char *buf = malloc(needed + 2);   /* +1 closing brace, +1 NUL */
  if (!buf) { free(escaped_msg); free(escaped_file); return; }
  if (show_tid) {
    snprintf(buf, head + 1,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"thread_id\": %lu, \"message\": \"%s\"",
      time_buf, lvl_str, escaped_file, line_val, LOG_GET_THREAD_ID(), escaped_msg);
  } else {
    snprintf(buf, head + 1,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"",
      time_buf, lvl_str, escaped_file, line_val, escaped_msg);
  }
#if LOG_FEATURE_KV
  if (kv_len) kv_render_json(ev->kv, buf + head, kv_len + 1);
#endif
  buf[needed] = '}';
  buf[needed + 1] = '\0';

  fprintf(ev->udata, "%s\n", buf);
  free(escaped_msg);
  free(escaped_file);
  free(buf);
}
#endif /* LOG_FEATURE_JSON */

/* Async writer thread */
#if LOG_FEATURE_ASYNC
#if defined(LOG_PLATFORM_POSIX)
static void* async_writer_thread(void *arg) {
#else
static DWORD WINAPI async_writer_thread(LPVOID arg) {
#endif
  log_handle *ctx = (log_handle*)arg;

  /* The writer thread uses a dedicated context-owned slot and never takes
   * ctx->mutex here: log_enable_mpool()/ts_cache() hold that mutex while
   * joining this thread, so acquiring it during startup would deadlock. */
#if LOG_FEATURE_STATS
  tl_stats = &ctx->async_writer_stats;
#endif

#if LOG_FEATURE_RING_QUEUE
  if (ctx->use_ring_queue) {
    log_drain_batch batch;
    while (true) {
      int timeout_ms = log_next_flush_timeout_ms(ctx);
      int n = ring_queue_pop_batch(&ctx->ring_queue, &batch, timeout_ms);
      if (n == 0) break;                 /* closed and drained */
      if (n < 0) {                       /* interval timer fired */
        log_flush_due_intervals(ctx);
        continue;
      }
#if LOG_FEATURE_STATS
      double now_ts = get_timestamp_with_clock(ctx->clock_source);
#endif
      rwlock_read_lock(&ctx->rwlock);
      for (int k = 0; k < n; k++) {
        log_ring_entry *re = &batch.entries[k];
        log_event ev = {0};
        ev.level = re->level;
        ev.file = batch.large_file[k] ? batch.large_file[k] : re->file;
        ev.line = re->line;
        ev.timestamp = re->timestamp;
        ev.raw_msg = batch.large_msg[k] ? batch.large_msg[k] : re->msg;
#if LOG_FEATURE_KV
        if (re->has_kv) {
#if LOG_FEATURE_STATIC_ALLOC
          ev.kv = re->kv_storage;
#else
          ev.kv = ev.raw_msg + strlen(ev.raw_msg) + 1;
#endif
        }
#endif
        for (int i = 0; i < ctx->handler_count; i++) {
          if (ctx->handlers[i].active && ctx->handlers[i].fn && re->level >= ctx->handlers[i].level) {
            ev.udata = ctx->handlers[i].udata;
            ctx->handlers[i].fn(ctx, &ev);
            log_apply_flush(ctx, i);
          }
        }
#if LOG_FEATURE_STATS
        double lat = now_ts - re->timestamp;
        if (lat < 0.0) lat = 0.0;   /* guard against clock steps */
        STAT_ADD(queue_latency_total_ms, lat * 1000.0);
        STAT_INC(queue_latency_count);
#endif
      }
      rwlock_read_unlock(&ctx->rwlock);
      for (int k = 0; k < n; k++) {
        free(batch.large_msg[k]);
        free(batch.large_file[k]);
      }
    }
  } else
#endif /* LOG_FEATURE_RING_QUEUE */
  {
    log_queue_entry *entries[LOG_DRAIN_BATCH];
    while (true) {
      int timeout_ms = log_next_flush_timeout_ms(ctx);
      int n = queue_pop_batch(&ctx->queue, entries, LOG_DRAIN_BATCH, timeout_ms);
      if (n == 0) break;                 /* closed and drained */
      if (n < 0) {                       /* interval timer fired */
        log_flush_due_intervals(ctx);
        continue;
      }
#if LOG_FEATURE_STATS
      double now_ts = get_timestamp_with_clock(ctx->clock_source);
#endif
      rwlock_read_lock(&ctx->rwlock);
      for (int k = 0; k < n; k++) {
        log_queue_entry *entry = entries[k];
        log_event ev = {0};
        ev.level = entry->level;
        ev.file = entry->file;
        ev.line = entry->line;
        ev.timestamp = entry->timestamp;
        ev.raw_msg = entry->msg;
#if LOG_FEATURE_KV
        ev.kv = entry->kv;
#endif
        for (int i = 0; i < ctx->handler_count; i++) {
          if (ctx->handlers[i].active && ctx->handlers[i].fn && entry->level >= ctx->handlers[i].level) {
            ev.udata = ctx->handlers[i].udata;
            ctx->handlers[i].fn(ctx, &ev);
            log_apply_flush(ctx, i);
          }
        }
#if LOG_FEATURE_STATS
        double lat = now_ts - entry->timestamp;
        if (lat < 0.0) lat = 0.0;   /* guard against clock steps */
        STAT_ADD(queue_latency_total_ms, lat * 1000.0);
        STAT_INC(queue_latency_count);
#endif
      }
      rwlock_read_unlock(&ctx->rwlock);
      for (int k = 0; k < n; k++) {
        log_queue_entry *entry = entries[k];
        /* NULL before returning to the pool: pooled entries reuse the
         * msg/file slots and must not hold dangling pointers. */
        free(entry->msg);
        free(entry->file);
        entry->msg = NULL;
        entry->file = NULL;
#if LOG_FEATURE_KV
        free(entry->kv);
        entry->kv = NULL;
#endif
        queue_entry_destroy(ctx, entry);
      }
    }
  }

#if defined(LOG_PLATFORM_POSIX)
  return NULL;
#else
  return 0;
#endif
}
#endif /* LOG_FEATURE_ASYNC */

/* API Implementation */

/* Shared context initialization for log_create() and log_create_static().
 * Returns false on allocation failure (impossible in static mode). */
static bool init_context(log_handle *ctx) {
  /* Pre-warm the timezone state once so that concurrent log calls never
   * race inside glibc's lazily-initialized tzset_internal. */
  tzset();

  rwlock_init(&ctx->rwlock);
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&ctx->mutex, NULL);
  pthread_mutex_init(&ctx->file_mtx, NULL);
#else
  InitializeCriticalSection(&ctx->mutex);
  InitializeCriticalSection(&ctx->file_mtx);
#endif

  ctx->level = LOG_TRACE;
  ctx->quiet = false;
  ctx->max_file_size = LOG_DEFAULT_MAX_SIZE;
  ctx->async_enabled = false;
  ctx->queue_policy = LOG_QUEUE_FALLBACK_SYNC;
  ctx->crash_safe = false;

#if LOG_FEATURE_FILTER
  memset(&ctx->filter, 0, sizeof(ctx->filter));
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&ctx->filter.mtx, NULL);
#else
  InitializeCriticalSection(&ctx->filter.mtx);
#endif
#endif

  queue_init(&ctx->queue, DEFAULT_QUEUE_SIZE);

  ctx->handler_capacity = MAX_HANDLERS;
#if LOG_FEATURE_STATIC_ALLOC
  ctx->handlers = ctx->handlers_storage;
  memset(ctx->handlers_storage, 0, sizeof(ctx->handlers_storage));
#else
  ctx->handlers = calloc(MAX_HANDLERS, sizeof(log_handler));
  if (!ctx->handlers) return false;
#endif
  ctx->handler_count = 0;

  ctx->format_fn = NULL;

  ctx->syslog_ident = NULL;
  ctx->syslog_facility = LOG_USER;
  ctx->syslog_enabled_global = false;

#if LOG_FEATURE_MPOOL
  mpool_init(&ctx->mpool, LOG_MPOOL_MAX_CHUNKS * LOG_MPOOL_CHUNK_SIZE);
#if LOG_FEATURE_STATIC_ALLOC
  ctx->enable_mpool = false;   /* pool chunks would heap-allocate */
#else
  ctx->enable_mpool = true;
#endif
#else
  ctx->enable_mpool = false;
#endif
#if LOG_FEATURE_TS_CACHE
  ctx->enable_ts_cache = true;
#else
  ctx->enable_ts_cache = false;
#endif

#if LOG_FEATURE_RING_QUEUE
  ctx->use_ring_queue = true;
#endif
#if LOG_FEATURE_STATIC_ALLOC && LOG_FEATURE_RING_QUEUE
  ring_queue_init_with_storage(&ctx->ring_queue, LOG_RING_CAPACITY, ctx->ring_storage);
#elif LOG_FEATURE_RING_QUEUE
  ring_queue_init(&ctx->ring_queue, DEFAULT_QUEUE_SIZE);
#else
  ctx->use_ring_queue = false;
#endif
  ctx->clock_source = LOG_CLOCK_REALTIME_COARSE;

  stats_init_registry(ctx);

  log_add_handler(ctx, stdout_handler, stderr, LOG_TRACE);
  if (ctx->handler_count > 0) {
    ctx->handlers[0].kind = HANDLER_STDOUT;
    ctx->handlers[0].owns_file = false;
  }

  return true;
}

log_handle* log_create(void) {
  log_handle *ctx = calloc(1, sizeof(log_handle));
  if (!ctx) return NULL;
  if (!init_context(ctx)) {
    free(ctx);
    return NULL;
  }
  return ctx;
}

#if LOG_FEATURE_STATIC_ALLOC
size_t log_static_ctx_size(void) {
  return sizeof(log_handle);
}

log_handle* log_create_static(void *buf, size_t buf_size) {
  if (!buf || buf_size < sizeof(log_handle)) return NULL;
  /* buf must be aligned like log_static_storage_t (see log.h); that type
   * is the documented way to declare the storage. */
  log_handle *ctx = (log_handle*)buf;
  memset(ctx, 0, sizeof(*ctx));
  if (!init_context(ctx)) {
    return NULL;
  }
  return ctx;
}
#endif /* LOG_FEATURE_STATIC_ALLOC */

/* ==================== Process lifecycle safety (B6) ==================== */
/* log_destroy() drops the context from these registries so an exit or fork
 * after teardown never touches freed memory. */
#if LOG_FEATURE_LIFECYCLE && defined(LOG_PLATFORM_POSIX)

#define LOG_LIFECYCLE_MAX_CTXS 8

typedef struct { log_handle *ctx; bool atfork; bool atexit; } log_lifecycle_entry;

static log_lifecycle_entry g_lifecycle[LOG_LIFECYCLE_MAX_CTXS];
static int g_lifecycle_count = 0;
static pthread_mutex_t g_lifecycle_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool g_atfork_installed = false;
static bool g_atexit_installed = false;

static int lifecycle_find(log_handle *ctx) {
  for (int i = 0; i < g_lifecycle_count; i++) {
    if (g_lifecycle[i].ctx == ctx) return i;
  }
  return -1;
}

/* Quiesce a context: hold every lock a logging thread could hold, in the
 * same order the library acquires them (mutex -> rwlock -> file_mtx ->
 * queue/ring -> mpool), so fork() happens at a consistent point. */
static void lifecycle_lock(log_handle *ctx) {
  pthread_mutex_lock(&ctx->mutex);
  rwlock_write_lock(&ctx->rwlock);
#if LOG_FEATURE_FILTER
  pthread_mutex_lock(&ctx->filter.mtx);
#endif
  pthread_mutex_lock(&ctx->file_mtx);
  pthread_mutex_lock(&ctx->queue.mtx);
#if LOG_FEATURE_RING_QUEUE
  pthread_mutex_lock(&ctx->ring_queue.mtx);
#endif
#if LOG_FEATURE_MPOOL
  pthread_mutex_lock(&ctx->mpool.mtx);
#endif
}

static void lifecycle_unlock(log_handle *ctx) {
#if LOG_FEATURE_MPOOL
  pthread_mutex_unlock(&ctx->mpool.mtx);
#endif
#if LOG_FEATURE_RING_QUEUE
  pthread_mutex_unlock(&ctx->ring_queue.mtx);
#endif
  pthread_mutex_unlock(&ctx->queue.mtx);
  pthread_mutex_unlock(&ctx->file_mtx);
#if LOG_FEATURE_FILTER
  pthread_mutex_unlock(&ctx->filter.mtx);
#endif
  rwlock_write_unlock(&ctx->rwlock);
  pthread_mutex_unlock(&ctx->mutex);
}

/* Child side: the parent's threads do not exist, so reinitialize the locks
 * and downgrade async to synchronous. Anything still queued is abandoned
 * (writing it would duplicate the parent's pending output). */
static void lifecycle_reinit(log_handle *ctx) {
  rwlock_init(&ctx->rwlock);
  pthread_mutex_init(&ctx->mutex, NULL);
  pthread_mutex_init(&ctx->file_mtx, NULL);
#if LOG_FEATURE_FILTER
  pthread_mutex_init(&ctx->filter.mtx, NULL);
#endif
  pthread_mutex_init(&ctx->queue.mtx, NULL);
  pthread_cond_init(&ctx->queue.cond, NULL);
  pthread_cond_init(&ctx->queue.space_cond, NULL);
  ctx->queue.head = NULL;
  ctx->queue.tail = NULL;
  ctx->queue.size = 0;
  ctx->queue.closed = false;
#if LOG_FEATURE_RING_QUEUE
  pthread_mutex_init(&ctx->ring_queue.mtx, NULL);
  pthread_cond_init(&ctx->ring_queue.cond, NULL);
  pthread_cond_init(&ctx->ring_queue.space_cond, NULL);
  atomic_store(&ctx->ring_queue.head, 0);
  atomic_store(&ctx->ring_queue.tail, 0);
  ctx->ring_queue.closed = false;
#endif
#if LOG_FEATURE_MPOOL
  pthread_mutex_init(&ctx->mpool.mtx, NULL);
#endif
  ctx->async_enabled = false;   /* no writer thread exists in the child */
}

static void lifecycle_atfork_prepare(void) {
  pthread_mutex_lock(&g_lifecycle_mtx);
  for (int i = 0; i < g_lifecycle_count; i++) {
    if (g_lifecycle[i].atfork && g_lifecycle[i].ctx) lifecycle_lock(g_lifecycle[i].ctx);
  }
  pthread_mutex_lock(&default_log_mutex);
}

static void lifecycle_atfork_parent(void) {
  pthread_mutex_unlock(&default_log_mutex);
  for (int i = g_lifecycle_count - 1; i >= 0; i--) {
    if (g_lifecycle[i].atfork && g_lifecycle[i].ctx) lifecycle_unlock(g_lifecycle[i].ctx);
  }
  pthread_mutex_unlock(&g_lifecycle_mtx);
}

static void lifecycle_atfork_child(void) {
  pthread_mutex_init(&g_lifecycle_mtx, NULL);
  pthread_mutex_init(&default_log_mutex, NULL);
  for (int i = 0; i < g_lifecycle_count; i++) {
    if (g_lifecycle[i].atfork && g_lifecycle[i].ctx) lifecycle_reinit(g_lifecycle[i].ctx);
  }
}

static void lifecycle_atexit_handler(void) {
  pthread_mutex_lock(&g_lifecycle_mtx);
  for (int i = 0; i < g_lifecycle_count; i++) {
    log_handle *ctx = g_lifecycle[i].ctx;
    if (!g_lifecycle[i].atexit || !ctx) continue;
#if LOG_FEATURE_ASYNC
    if (ctx->async_enabled) {
      log_set_async(ctx, false);   /* drains the queue and joins the writer */
    }
#endif
  }
  pthread_mutex_unlock(&g_lifecycle_mtx);
}

int log_install_atfork(log_handle *ctx) {
  if (!ctx) return -1;
  pthread_mutex_lock(&g_lifecycle_mtx);
  int idx = lifecycle_find(ctx);
  if (idx < 0) {
    if (g_lifecycle_count >= LOG_LIFECYCLE_MAX_CTXS) {
      pthread_mutex_unlock(&g_lifecycle_mtx);
      return -1;
    }
    idx = g_lifecycle_count++;
    g_lifecycle[idx].ctx = ctx;
    g_lifecycle[idx].atfork = false;
    g_lifecycle[idx].atexit = false;
  }
  g_lifecycle[idx].atfork = true;
  int rc = 0;
  if (!g_atfork_installed) {
    if (pthread_atfork(lifecycle_atfork_prepare, lifecycle_atfork_parent,
                       lifecycle_atfork_child) != 0) {
      rc = -1;
    } else {
      g_atfork_installed = true;
    }
  }
  pthread_mutex_unlock(&g_lifecycle_mtx);
  return rc;
}

int log_install_atexit(log_handle *ctx) {
  if (!ctx) return -1;
  pthread_mutex_lock(&g_lifecycle_mtx);
  int idx = lifecycle_find(ctx);
  if (idx < 0) {
    if (g_lifecycle_count >= LOG_LIFECYCLE_MAX_CTXS) {
      pthread_mutex_unlock(&g_lifecycle_mtx);
      return -1;
    }
    idx = g_lifecycle_count++;
    g_lifecycle[idx].ctx = ctx;
    g_lifecycle[idx].atfork = false;
    g_lifecycle[idx].atexit = false;
  }
  g_lifecycle[idx].atexit = true;
  int rc = 0;
  if (!g_atexit_installed) {
    if (atexit(lifecycle_atexit_handler) != 0) rc = -1;
    else g_atexit_installed = true;
  }
  pthread_mutex_unlock(&g_lifecycle_mtx);
  return rc;
}

static void lifecycle_unregister(log_handle *ctx) {
  pthread_mutex_lock(&g_lifecycle_mtx);
  int idx = lifecycle_find(ctx);
  if (idx >= 0) {
    g_lifecycle[idx] = g_lifecycle[g_lifecycle_count - 1];
    g_lifecycle_count--;
  }
  pthread_mutex_unlock(&g_lifecycle_mtx);
}

#else  /* !(LOG_FEATURE_LIFECYCLE && POSIX) */

static void lifecycle_unregister(log_handle *ctx) { (void)ctx; }
#if LOG_FEATURE_LIFECYCLE
int log_install_atfork(log_handle *ctx) { (void)ctx; return -1; }  /* POSIX only */
int log_install_atexit(log_handle *ctx) { (void)ctx; return -1; }  /* POSIX only */
#endif

#endif /* LOG_FEATURE_LIFECYCLE && POSIX */

/* ==================== Named loggers (B4) ==================== */
#if LOG_FEATURE_NAMED

/* Build an alias handle that shares `def`'s handlers and owns only its own
 * level/quiet. Caller holds def->mutex. */
static log_handle* named_alias_create(log_handle *def, const char *name) {
  log_handle *h = calloc(1, sizeof(log_handle));
  if (!h) return NULL;
  rwlock_init(&h->rwlock);
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&h->mutex, NULL);
  pthread_mutex_init(&h->file_mtx, NULL);
#else
  InitializeCriticalSection(&h->mutex);
  InitializeCriticalSection(&h->file_mtx);
#endif
#if LOG_FEATURE_FILTER
  memset(&h->filter, 0, sizeof(h->filter));
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&h->filter.mtx, NULL);
#else
  InitializeCriticalSection(&h->filter.mtx);
#endif
#endif
  h->base = def;
  h->level = def->level;
  h->quiet = false;
  h->async_enabled = false;

  size_t n = strlen(name);
  memcpy(def->named[def->named_count].name, name, n);
  def->named[def->named_count].name[n] = '\0';
  def->named[def->named_count].handle = h;
  def->named_count++;
  return h;
}

static void named_alias_free(log_handle *h) {
  if (!h) return;
  rwlock_destroy(&h->rwlock);
#if LOG_FEATURE_FILTER
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_destroy(&h->filter.mtx);
#else
  DeleteCriticalSection(&h->filter.mtx);
#endif
#endif
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_destroy(&h->mutex);
  pthread_mutex_destroy(&h->file_mtx);
#else
  DeleteCriticalSection(&h->mutex);
  DeleteCriticalSection(&h->file_mtx);
#endif
  free(h);
}

log_handle* log_get(const char *name) {
  log_handle *def = log_default();
  if (!def) return NULL;
  if (!name || name[0] == '\0' || strlen(name) >= LOG_NAMED_NAME_MAX) {
    log_warn("log_get: invalid name, using default context");
    return def;
  }

#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&def->mutex);
#else
  EnterCriticalSection(&def->mutex);
#endif
  log_handle *found = NULL;
  for (int i = 0; i < def->named_count; i++) {
    if (strcmp(def->named[i].name, name) == 0) {
      found = def->named[i].handle;
      break;
    }
  }
  bool full = false;
  if (!found) {
    if (def->named_count >= LOG_NAMED_MAX) {
      full = true;
    } else {
      found = named_alias_create(def, name);
    }
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&def->mutex);
#else
  LeaveCriticalSection(&def->mutex);
#endif

  if (full) {
    log_warn("log_get: named logger registry full, using default context");
    return def;
  }
  if (!found) {
    log_warn("log_get: allocation failed, using default context");
    return def;
  }
  return found;
}

void log_named_set_level(const char *name, int level) {
  if (!name || name[0] == '\0' || strlen(name) >= LOG_NAMED_NAME_MAX) return;
  log_handle *def = log_default();
  if (!def) return;

  log_handle *target = NULL;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&def->mutex);
#else
  EnterCriticalSection(&def->mutex);
#endif
  for (int i = 0; i < def->named_count; i++) {
    if (strcmp(def->named[i].name, name) == 0) {
      target = def->named[i].handle;
      break;
    }
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&def->mutex);
#else
  LeaveCriticalSection(&def->mutex);
#endif

  if (!target) target = log_get(name);
  if (target && target != def) log_set_level(target, level);
}

#endif /* LOG_FEATURE_NAMED */

/* ==================== Public API: context lifecycle & configuration ==================== */

void log_destroy(log_handle *ctx) {
  if (!ctx) return;

#if LOG_FEATURE_NAMED
  /* Named loggers are owned by the default context; destroying one is a
   * no-op (it is freed when its owner is destroyed). */
  if (ctx->base) return;
#endif

  lifecycle_unregister(ctx);

#if LOG_FEATURE_FILTER
  /* Emit any pending "last message repeated N times" summaries before the
   * queue is drained/closed and the handlers go away. */
  log_flush_suppressed(ctx);
#endif

  if (ctx->async_enabled) {
#if LOG_FEATURE_RING_QUEUE
    if (ctx->use_ring_queue) {
      ring_queue_shutdown(&ctx->ring_queue);
    } else
#endif
    {
      queue_shutdown(&ctx->queue);
    }
    LOG_THREAD_JOIN(ctx->async_thread);
  }

  queue_destroy(&ctx->queue);
#if LOG_FEATURE_RING_QUEUE
  ring_queue_destroy(&ctx->ring_queue);
#endif

#if LOG_HAVE_SYSLOG && LOG_FEATURE_SYSLOG
  if (ctx->syslog_enabled_global) {
    closelog();
  }
#endif

  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].owns_file && ctx->handlers[i].fp) {
      fflush(ctx->handlers[i].fp);
      if (ctx->handlers[i].fp != stderr && ctx->handlers[i].fp != stdout) {
        fclose(ctx->handlers[i].fp);
      }
    }
    free(ctx->handlers[i].filename);
#if LOG_FEATURE_MEMORY_HANDLER
    if (ctx->handlers[i].kind == HANDLER_MEMORY && ctx->handlers[i].udata) {
      log_memory_store *st = (log_memory_store*)ctx->handlers[i].udata;
#if LOG_FEATURE_CRASH_MODE
      crash_memory_unregister(st);
#endif
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_destroy(&st->mtx);
#else
      DeleteCriticalSection(&st->mtx);
#endif
      free(st->entries);
      free(st);
      ctx->handlers[i].udata = NULL;
    }
#endif
  }
#if LOG_FEATURE_STATIC_ALLOC
  /* handlers storage is embedded in the caller-provided context */
#else
  free(ctx->handlers);
#endif

  free(ctx->file_prefix);
  free(ctx->syslog_ident);
#if LOG_FEATURE_FILTER
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_destroy(&ctx->filter.mtx);
#else
  DeleteCriticalSection(&ctx->filter.mtx);
#endif
#endif
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_destroy(&ctx->mutex);
  pthread_mutex_destroy(&ctx->file_mtx);
#else
  DeleteCriticalSection(&ctx->mutex);
  DeleteCriticalSection(&ctx->file_mtx);
#endif

  rwlock_destroy(&ctx->rwlock);
#if LOG_FEATURE_MPOOL
  mpool_destroy(&ctx->mpool);
#endif
#if LOG_FEATURE_NAMED
  /* Named aliases are owned by this context; free them with it. */
  for (int i = 0; i < ctx->named_count; i++) {
    named_alias_free(ctx->named[i].handle);
    ctx->named[i].handle = NULL;
  }
  ctx->named_count = 0;
#endif
#if LOG_FEATURE_STATIC_ALLOC
  /* context memory belongs to the caller's storage block */
#else
  free(ctx);
#endif
}

log_handle* log_default(void) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&default_log_mutex);
#else
  if (!default_log_mutex_initialized) {
    InitializeCriticalSection(&default_log_mutex);
    default_log_mutex_initialized = true;
  }
  EnterCriticalSection(&default_log_mutex);
#endif
  if (!DEFAULT_LOG) {
    DEFAULT_LOG = log_create();
  }
  log_handle *result = DEFAULT_LOG;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&default_log_mutex);
#else
  LeaveCriticalSection(&default_log_mutex);
#endif
  return result;
}

const char* log_level_string(int level) {
  if (level < 0 || level >= LOG_LEVELS) {
    return "UNKNOWN";
  }
  return level_strings[level];
}

void log_set_level(log_handle *ctx, int level) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->level = level;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_quiet(log_handle *ctx, bool enable) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->quiet = enable;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_format(log_handle *ctx, log_FormatFn fn) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->format_fn = fn;
  rwlock_write_unlock(&ctx->rwlock);
}

#if LOG_FEATURE_ASYNC
int log_set_async(log_handle *ctx, bool enable) {
  if (!ctx) return -1;
#if LOG_FEATURE_CRASH_MODE
  /* Crash-safe mode requires the synchronous path: messages sitting in a
   * queue when a fatal signal hits would be lost. */
  if (enable && ctx->crash_safe) return -1;
#endif
  if (enable && !ctx->async_enabled) {
    queue_reopen(&ctx->queue);
#if LOG_FEATURE_STATS
    memset(&ctx->async_writer_stats, 0, sizeof(ctx->async_writer_stats));
#endif
    ctx->async_enabled = true;
#ifdef LOG_PLATFORM_POSIX
    if (LOG_THREAD_CREATE(ctx->async_thread, async_writer_thread, ctx) != 0) {
      ctx->async_enabled = false;
      return -1;
    }
#elif defined(LOG_PLATFORM_WINDOWS)
    ctx->async_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)async_writer_thread, ctx, 0, NULL);
    if (ctx->async_thread == NULL) {
      ctx->async_enabled = false;
      return -1;
    }
#endif
  } else if (!enable && ctx->async_enabled) {
#if LOG_FEATURE_RING_QUEUE
    if (ctx->use_ring_queue) {
      ring_queue_shutdown(&ctx->ring_queue);
    } else
#endif
    {
      queue_shutdown(&ctx->queue);
    }
    LOG_THREAD_JOIN(ctx->async_thread);
    ctx->async_enabled = false;
  }
  return 0;
}
#endif /* LOG_FEATURE_ASYNC */

#if LOG_FEATURE_FILE_OPS
void log_set_max_file_size(log_handle *ctx, size_t size) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->max_file_size = size;
  rwlock_write_unlock(&ctx->rwlock);
}

/**
 * Check if the path contains path traversal sequences.
 * Returns 1 if the path is safe (no traversal), 0 if unsafe.
 * Absolute paths are allowed on Unix systems.
 */
static int is_path_safe(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }

#ifdef LOG_PLATFORM_WINDOWS
  /* Reject absolute paths on Windows */
  if (path[0] == '\\') {
    return 0;
  }
  if (path[1] == ':') {
    return 0;
  }
#endif

  /* Check for ".." path traversal sequences */
  const char *p = path;
  while (*p) {
    /* Check for ".." at start or after a separator */
    if (p[0] == '.' && p[1] == '.') {
      /* ".." at end of string or followed by separator */
      if (p[2] == '\0' || p[2] == '/' || p[2] == '\\') {
        return 0;
      }
    }
    p++;
  }

  return 1;
}

void log_set_file_prefix(log_handle *ctx, const char *prefix) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);

  const char *safe_prefix = prefix ? prefix : "log";

  if (!is_path_safe(safe_prefix)) {
    rwlock_write_unlock(&ctx->rwlock);
    return;
  }

  free(ctx->file_prefix);
  ctx->file_prefix = strdup(safe_prefix);
  if (!ctx->file_prefix) {
    rwlock_write_unlock(&ctx->rwlock);
    return;
  }
  rwlock_write_unlock(&ctx->rwlock);
}
#endif /* LOG_FEATURE_FILE_OPS */

#if LOG_FEATURE_MPOOL
void log_enable_mpool(log_handle *ctx, bool enable) {
  if (!ctx) return;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->mutex);
#else
  EnterCriticalSection(&ctx->mutex);
#endif
  bool was_async_enabled = ctx->async_enabled;
  if (was_async_enabled) {
    log_set_async(ctx, false);
  }
  rwlock_write_lock(&ctx->rwlock);
  if (!enable && ctx->enable_mpool) {
    mpool_destroy(&ctx->mpool);
    mpool_init(&ctx->mpool, LOG_MPOOL_MAX_CHUNKS * LOG_MPOOL_CHUNK_SIZE);
  }
  ctx->enable_mpool = enable;
  rwlock_write_unlock(&ctx->rwlock);
  if (was_async_enabled) {
    log_set_async(ctx, true);
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->mutex);
#else
  LeaveCriticalSection(&ctx->mutex);
#endif
}
#endif /* LOG_FEATURE_MPOOL */

#if LOG_FEATURE_TS_CACHE
void log_enable_ts_cache(log_handle *ctx, bool enable) {
  if (!ctx) return;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->mutex);
#else
  EnterCriticalSection(&ctx->mutex);
#endif
  bool was_async_enabled = ctx->async_enabled;
  if (was_async_enabled) {
    log_set_async(ctx, false);
  }
  rwlock_write_lock(&ctx->rwlock);
  ctx->enable_ts_cache = enable;
  rwlock_write_unlock(&ctx->rwlock);
  if (was_async_enabled) {
    log_set_async(ctx, true);
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->mutex);
#else
  LeaveCriticalSection(&ctx->mutex);
#endif
}
#endif /* LOG_FEATURE_TS_CACHE */

void log_set_queue_policy(log_handle *ctx, int policy) {
  if (!ctx || policy < LOG_QUEUE_FALLBACK_SYNC || policy > LOG_QUEUE_BLOCK) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->queue_policy = policy;
  rwlock_write_unlock(&ctx->rwlock);
}

#if LOG_FEATURE_RING_QUEUE
void log_enable_ring_queue(log_handle *ctx, bool enable) {
  if (!ctx) return;
#if LOG_FEATURE_STATIC_ALLOC
  (void)enable;  /* static mode always uses the embedded ring storage */
#else
  rwlock_write_lock(&ctx->rwlock);
  ctx->use_ring_queue = enable;
  rwlock_write_unlock(&ctx->rwlock);
#endif
}
#endif /* LOG_FEATURE_RING_QUEUE */

void log_set_clock_source(log_handle *ctx, int clock_source) {
  if (!ctx || clock_source < LOG_CLOCK_REALTIME || clock_source > LOG_CLOCK_MONOTONIC_COARSE) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->clock_source = clock_source;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_queue_size(log_handle *ctx, size_t size) {
  if (!ctx || size < 2) return;
#if LOG_FEATURE_STATIC_ALLOC
  (void)size;  /* ring storage is fixed at compile time */
  return;
#else
  rwlock_write_lock(&ctx->rwlock);
#if LOG_FEATURE_RING_QUEUE
  ring_queue_destroy(&ctx->ring_queue);
  ring_queue_init(&ctx->ring_queue, size);
#endif
  ctx->queue.max_size = size;
  rwlock_write_unlock(&ctx->rwlock);
#endif
}

/* Snapshot aggregate stats across every registered thread block. Guarded by
 * ctx->mutex so the slot list cannot change mid-read; each block is read
 * without its own lock (plain counters, best-effort monitoring). */
static void stats_snapshot(log_handle *ctx, log_stats *stats) {
#if LOG_FEATURE_STATS
  if (!ctx || !stats) return;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->mutex);
#else
  EnterCriticalSection(&ctx->mutex);
#endif
  log_stats acc = {0};
  uint64_t lat_count = 0;
  double lat_total = 0.0;
  for (int i = 0; i < ctx->stats_registry.count; i++) {
    log_thread_stats *s = &ctx->stats_registry.slots[i];
    acc.total_count += s->total_count;
    for (int l = 0; l < LOG_LEVELS; l++) acc.level_counts[l] += s->level_counts[l];
    acc.queue_drops += s->queue_drops;
    acc.queue_blocked += s->queue_blocked;
    acc.rotation_count += s->rotation_count;
    acc.async_writes += s->async_writes;
    acc.sync_writes += s->sync_writes;
    acc.truncated_count += s->truncated_count;
    acc.suppressed_count += s->suppressed_count;
    lat_count += s->queue_latency_count;
    lat_total += s->queue_latency_total_ms;
  }
  {
    /* Dedicated async writer slot (rotations + queue-latency samples). */
    log_thread_stats *s = &ctx->async_writer_stats;
    acc.total_count += s->total_count;
    for (int l = 0; l < LOG_LEVELS; l++) acc.level_counts[l] += s->level_counts[l];
    acc.queue_drops += s->queue_drops;
    acc.queue_blocked += s->queue_blocked;
    acc.rotation_count += s->rotation_count;
    acc.async_writes += s->async_writes;
    acc.sync_writes += s->sync_writes;
    acc.truncated_count += s->truncated_count;
    acc.suppressed_count += s->suppressed_count;
    lat_count += s->queue_latency_count;
    lat_total += s->queue_latency_total_ms;
  }
  acc.avg_queue_latency_ms = lat_count ? lat_total / (double)lat_count : 0.0;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->mutex);
#else
  LeaveCriticalSection(&ctx->mutex);
#endif
  *stats = acc;
#else
  (void)ctx;
  if (stats) memset(stats, 0, sizeof(*stats));
#endif
}

#if LOG_FEATURE_STATS
void log_get_perf_stats(log_handle *ctx, log_stats *stats) {
  if (!ctx || !stats) return;
  stats_snapshot(ctx, stats);
}
#endif /* LOG_FEATURE_STATS */

/* ==================== Handler management ==================== */

int log_add_handler(log_handle *ctx, log_LogFn fn, void *udata, int level) {
  if (!fn || !ctx || ctx->handler_count >= ctx->handler_capacity) {
    return -1;
  }

  rwlock_write_lock(&ctx->rwlock);

  log_handler *h = &ctx->handlers[ctx->handler_count++];
  h->fn = fn;
  h->udata = udata;
  h->level = level;
  h->active = true;
  h->fp = NULL;
  h->filename = NULL;
  h->file_size = 0;
  h->syslog_enabled = false;
  h->syslog_facility = LOG_USER;
  h->show_thread_id = false;
  h->kind = HANDLER_CUSTOM;
  h->flush_policy = LOG_FLUSH_NEVER;
  h->flush_interval_ms = 0;
  h->flush_fsync = false;
  h->last_flush = 0.0;
  h->format_fn = NULL;

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}

int log_add_fp(log_handle *ctx, FILE *fp, int level) {
  if (!ctx || !fp) return -1;
  if (ctx->handler_count >= ctx->handler_capacity) return -1;

  rwlock_write_lock(&ctx->rwlock);

  log_handler *h = &ctx->handlers[ctx->handler_count++];
  h->fn = file_handler_wrapper;
  h->udata = fp;
  h->fp = fp;
  h->level = level;
  h->active = true;
  h->filename = NULL;
  h->file_size = 0;
  h->syslog_enabled = false;
  h->syslog_facility = LOG_USER;
  h->show_thread_id = false;
  h->kind = HANDLER_FILE;
  h->owns_file = false;
  h->flush_policy = LOG_FLUSH_NEVER;
  h->flush_interval_ms = 0;
  h->flush_fsync = false;
  h->last_flush = 0.0;
  h->format_fn = NULL;

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}

#if LOG_FEATURE_MEMORY_HANDLER
int log_add_memory_handler(log_handle *ctx, int lines, int level) {
  if (!ctx || lines <= 0) return -1;
  if (ctx->handler_count >= ctx->handler_capacity) return -1;
  if (lines > LOG_MEMORY_MAX_LINES) lines = LOG_MEMORY_MAX_LINES;

  log_memory_store *st = calloc(1, sizeof(*st));
  if (!st) return -1;
  st->entries = calloc((size_t)lines, sizeof(log_memory_entry));
  if (!st->entries) { free(st); return -1; }
  st->capacity = (size_t)lines;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_init(&st->mtx, NULL);
#else
  InitializeCriticalSection(&st->mtx);
#endif

  rwlock_write_lock(&ctx->rwlock);
  /* Re-check under the write lock: a concurrent add may have filled the
   * table between the hint check above and here. */
  if (ctx->handler_count >= ctx->handler_capacity) {
    rwlock_write_unlock(&ctx->rwlock);
#if defined(LOG_PLATFORM_POSIX)
    pthread_mutex_destroy(&st->mtx);
#else
    DeleteCriticalSection(&st->mtx);
#endif
    free(st->entries);
    free(st);
    return -1;
  }

  log_handler *h = &ctx->handlers[ctx->handler_count++];
  h->fn = memory_handler;
  h->udata = st;
  h->level = level;
  h->active = true;
  h->fp = NULL;
  h->filename = NULL;
  h->file_size = 0;
  h->syslog_enabled = false;
  h->syslog_facility = LOG_USER;
  h->show_thread_id = false;
  h->kind = HANDLER_MEMORY;
  h->owns_file = false;
  h->flush_policy = LOG_FLUSH_NEVER;
  h->flush_interval_ms = 0;
  h->flush_fsync = false;
  h->last_flush = 0.0;
  h->format_fn = NULL;

  rwlock_write_unlock(&ctx->rwlock);
#if LOG_FEATURE_CRASH_MODE
  crash_memory_register(st);
#endif
  return ctx->handler_count - 1;
}

void log_dump_memory_handler(log_handle *ctx, int handler_idx, FILE *out) {
  if (!ctx || !out) return;
  if (handler_idx < 0 || handler_idx >= ctx->handler_count) return;

  rwlock_read_lock(&ctx->rwlock);
  log_handler *h = &ctx->handlers[handler_idx];
  if (h->kind != HANDLER_MEMORY || !h->udata) {
    rwlock_read_unlock(&ctx->rwlock);
    return;
  }
  log_memory_store *st = (log_memory_store*)h->udata;
  /* Hold the store lock for the whole dump so the sequence is a consistent
   * snapshot: producers block briefly, but no entry is torn or reordered. */
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&st->mtx);
#else
  EnterCriticalSection(&st->mtx);
#endif
  size_t cap = st->capacity;
  size_t count = st->count;
  size_t start = (count < cap) ? 0 : st->head;   /* oldest entry */
  for (size_t k = 0; k < count; k++) {
    log_memory_entry *e = &st->entries[(start + k) % cap];
    if (e->len > 0) fwrite(e->line, 1, (size_t)e->len, out);
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&st->mtx);
#else
  LeaveCriticalSection(&st->mtx);
#endif
  rwlock_read_unlock(&ctx->rwlock);
}
#endif /* LOG_FEATURE_MEMORY_HANDLER */

#if LOG_FEATURE_FILE_OPS
int log_add_file(log_handle *ctx, const char *filename, int level) {
  if (!ctx || !filename) return -1;
  if (ctx->handler_count >= ctx->handler_capacity) return -1;

  FILE *fp = fopen(filename, "a");
  if (!fp) return -1;
  log_setup_stream_buffer(fp);

  rwlock_write_lock(&ctx->rwlock);

  log_handler *h = &ctx->handlers[ctx->handler_count++];
  h->fn = file_handler_wrapper;
  h->udata = fp;
  h->fp = fp;
  h->level = level;
  h->active = true;
  h->filename = strdup(filename);
  h->file_size = 0;
  h->syslog_enabled = false;
  h->syslog_facility = LOG_USER;
  h->show_thread_id = false;
  h->kind = HANDLER_FILE;
  h->owns_file = true;
  h->flush_policy = LOG_FLUSH_NEVER;
  h->flush_interval_ms = 0;
  h->flush_fsync = false;
  h->last_flush = 0.0;
  h->format_fn = NULL;

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}
#endif /* LOG_FEATURE_FILE_OPS */

void log_remove_handler(log_handle *ctx, int idx) {
  if (!ctx || idx < 0 || idx >= ctx->handler_count) return;
  
  rwlock_write_lock(&ctx->rwlock);
#if LOG_FEATURE_MEMORY_HANDLER && LOG_FEATURE_CRASH_MODE
  if (ctx->handlers[idx].kind == HANDLER_MEMORY && ctx->handlers[idx].udata) {
    crash_memory_unregister((const log_memory_store*)ctx->handlers[idx].udata);
  }
#endif
  ctx->handlers[idx].active = false;
  rwlock_write_unlock(&ctx->rwlock);
}

/* ==================== Formatting & synchronous dispatch ==================== */

/* Format the message body once (thread-local buffer; exact-size heap
 * fallback for oversized messages, truncation in static mode), then
 * dispatch synchronously to every active handler. Shared by the sync path
 * and the async FALLBACK_SYNC path. Caller holds the ctx read lock and has
 * already va_start()ed ev->ap (consumed here). */
static void sync_format_and_dispatch(log_handle *ctx, log_event *ev) {
  char *big = NULL;
  /* A caller may have supplied a pre-rendered body (structured kv logging
   * passes the literal message this way); then no printf work is needed. */
  if (!ev->raw_msg) {
    va_list probe;
    va_copy(probe, ev->ap);
    int pflen = vsnprintf(NULL, 0, ev->fmt, probe);
    va_end(probe);
    if (pflen < 0) {
      return;
    }
    if ((size_t)pflen < sizeof(tl_msg_buf)) {
      vsnprintf(tl_msg_buf, sizeof(tl_msg_buf), ev->fmt, ev->ap);
      ev->raw_msg = tl_msg_buf;
    } else {
#if LOG_FEATURE_STATIC_ALLOC
      /* Static mode: keep the truncated text, no heap allocation. */
      vsnprintf(tl_msg_buf, sizeof(tl_msg_buf), ev->fmt, ev->ap);
      ev->raw_msg = tl_msg_buf;
      STAT_INC(truncated_count);
#else
      big = malloc((size_t)pflen + 1);
      if (big) {
        vsnprintf(big, (size_t)pflen + 1, ev->fmt, ev->ap);
        ev->raw_msg = big;
      } else {
        ev->raw_msg = "";   /* OOM: handlers print an empty body */
      }
#endif
    }
  }

  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].active && ctx->handlers[i].fn && ev->level >= ctx->handlers[i].level) {
      ev->udata = ctx->handlers[i].udata;
      ctx->handlers[i].fn(ctx, ev);
      log_apply_flush(ctx, i);
    }
  }
  free(big);
}

/* ==================== Log flood control (B3) ==================== */
#if LOG_FEATURE_FILTER

/* FNV-1a 64-bit over `len` bytes, continuing from seed `h`. */
static uint64_t fnv1a_64(const void *data, size_t len, uint64_t h) {
  const unsigned char *p = (const unsigned char*)data;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

typedef struct log_filter_result {
  bool suppress;          /* current message must not be emitted */
  bool emit_summary;      /* a pending dedupe summary must be emitted first */
  int summary_level;
  unsigned summary_count;
} log_filter_result;

/* Render a printf-style body for the filter path. Returns tl_msg_buf when the
 * body fits, or an exact-size heap copy (tracked in *owned) otherwise; static
 * builds always truncate into tl_msg_buf. */
static const char* filter_render_body(const char *fmt, va_list ap, char **owned) {
  *owned = NULL;
  va_list probe;
  va_copy(probe, ap);
  int len = vsnprintf(NULL, 0, fmt, probe);
  va_end(probe);
  if (len < 0) return "";
  if ((size_t)len < sizeof(tl_msg_buf)) {
    vsnprintf(tl_msg_buf, sizeof(tl_msg_buf), fmt, ap);
    return tl_msg_buf;
  }
#if LOG_FEATURE_STATIC_ALLOC
  vsnprintf(tl_msg_buf, sizeof(tl_msg_buf), fmt, ap);
  return tl_msg_buf;
#else
  char *big = malloc((size_t)len + 1);
  if (!big) return "";
  vsnprintf(big, (size_t)len + 1, fmt, ap);
  *owned = big;
  return big;
#endif
}

/* Decide whether a rendered message should be emitted. `body` is the rendered
 * text; `kv`/`kv_len` are the optional encoded kv bytes (hashed with the body
 * so distinct structured events are not collapsed). All state is per level. */
static void filter_check(log_handle *ctx, int level, const char *body,
                         const void *kv, size_t kv_len, double now,
                         log_filter_result *res) {
  level = clamp_level(level);   /* caller may pass an out-of-range level */
  res->suppress = false;
  res->emit_summary = false;
  res->summary_level = level;
  res->summary_count = 0;
  log_filter *f = &ctx->filter;

#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&f->mtx);
#else
  EnterCriticalSection(&f->mtx);
#endif

  /* Rate limit: at most max_per_sec messages per one-second window. */
  log_rate_limit *rl = &f->rate[level];
  if (rl->max_per_sec > 0) {
    if (rl->window_start == 0.0 || now - rl->window_start >= 1.0) {
      rl->window_start = now;
      rl->count = 0;
    }
    if (rl->count >= rl->max_per_sec) {
      res->suppress = true;
      STAT_INC(suppressed_count);
      goto done;
    }
    rl->count++;
  }

  /* Dedupe: one active group per level. A different (or post-window) message
   * closes the current group; if it had suppressed repeats, ask the caller to
   * emit the summary line first. */
  {
    unsigned win = f->dedupe_window_ms[level];
    log_dedupe_group *g = &f->dedupe[level];
    uint64_t h = 1469598103934665603ULL;
    if (win > 0) {
      h = fnv1a_64(body, body ? strlen(body) : 0, h);
      if (kv && kv_len) h = fnv1a_64(kv, kv_len, h);
    }
    if (g->active) {
      bool expired = (win == 0) || (now - g->window_start >= (double)win / 1000.0);
      bool different = (win == 0) || (h != g->hash);
      if (g->suppressed > 0 && (expired || different)) {
        res->emit_summary = true;
        res->summary_count = g->suppressed;
      }
      if (expired || different) {
        g->active = false;
        g->suppressed = 0;
      }
    }
    if (win > 0) {
      if (g->active && h == g->hash) {
        g->suppressed++;
        res->suppress = true;
        STAT_INC(suppressed_count);
      } else {
        g->active = true;
        g->hash = h;
        g->window_start = now;
        g->suppressed = 0;
      }
    }
  }

done:
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&f->mtx);
#else
  LeaveCriticalSection(&f->mtx);
#endif
}

/* Dispatch a fully rendered line to the handlers. Caller holds ctx->rwlock
 * for reading (sync path). */
static void filter_dispatch_raw(log_handle *ctx, int level, const char *file,
                                int line, double timestamp, const char *body) {
  log_event ev = {0};
  ev.fmt = body;
  ev.raw_msg = body;
  ev.file = file;
  ev.line = line;
  ev.level = level;
  ev.timestamp = timestamp;
  sync_format_and_dispatch(ctx, &ev);
}

static bool filter_is_async(log_handle *ctx) {
#if LOG_FEATURE_CRASH_MODE
  return ctx->async_enabled && !ctx->crash_safe;
#else
  return ctx->async_enabled;
#endif
}

#if LOG_FEATURE_ASYNC
/* Enqueue a fully rendered line. Returns 1 enqueued, 0 dropped (DROP policy),
 * -1 caller must write synchronously (FALLBACK_SYNC / BLOCK). */
static int filter_enqueue(log_handle *ctx, int level, const char *file, int line,
                          double timestamp, const char *body, const char *kv,
                          int queue_policy) {
#if LOG_FEATURE_RING_QUEUE
  if (ctx->use_ring_queue) {
    if (ring_queue_push_body(&ctx->ring_queue, body, kv, file, level, line,
                             timestamp, queue_policy == LOG_QUEUE_BLOCK)) {
      return 1;
    }
  } else
#endif
  {
    log_queue_entry *entry = queue_entry_create_body(ctx, body, kv, file, level,
                                                     line, timestamp);
    bool ok = entry && queue_push(ctx, entry, queue_policy == LOG_QUEUE_BLOCK);
    if (!ok && entry) queue_entry_destroy(ctx, entry);
    if (ok) return 1;
  }
  if (queue_policy == LOG_QUEUE_DROP) return 0;
  return -1;
}
#endif /* LOG_FEATURE_ASYNC */

/* Emit a pending dedupe summary. lock_held says whether the caller already
 * holds ctx->rwlock for reading (sync dispatch inline); otherwise the async
 * path enqueues it, falling back to a synchronous write if needed. */
static void filter_emit_summary(log_handle *ctx, int level, unsigned count,
                                bool lock_held) {
  char buf[80];
  double ts = get_timestamp_with_clock(ctx->clock_source);
  snprintf(buf, sizeof(buf), "last message repeated %u times", count);

  if (!filter_is_async(ctx)) {
    if (!lock_held) rwlock_read_lock(&ctx->rwlock);
    filter_dispatch_raw(ctx, level, "log_filter", 0, ts, buf);
    if (!lock_held) rwlock_read_unlock(&ctx->rwlock);
    return;
  }
#if LOG_FEATURE_ASYNC
  {
    int policy = ctx->queue_policy;
    int r = filter_enqueue(ctx, level, "log_filter", 0, ts, buf, NULL, policy);
    if (r == -1) {
      rwlock_read_lock(&ctx->rwlock);
      filter_dispatch_raw(ctx, level, "log_filter", 0, ts, buf);
      rwlock_read_unlock(&ctx->rwlock);
    }
  }
#else
  (void)lock_held;
#endif
}

/* Public B3 API ----------------------------------------------------------- */

void log_set_rate_limit(log_handle *ctx, int level, unsigned max_per_sec) {
  if (!ctx || level < 0 || level >= LOG_LEVELS) return;
  rwlock_write_lock(&ctx->rwlock);
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->filter.mtx);
#else
  EnterCriticalSection(&ctx->filter.mtx);
#endif
  ctx->filter.rate[level].max_per_sec = max_per_sec;
  ctx->filter.rate[level].count = 0;
  ctx->filter.rate[level].window_start = 0.0;
  if (max_per_sec) ctx->filter.enabled = true;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->filter.mtx);
#else
  LeaveCriticalSection(&ctx->filter.mtx);
#endif
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_dedupe(log_handle *ctx, int level, unsigned window_ms) {
  if (!ctx || level < 0 || level >= LOG_LEVELS) return;
  rwlock_write_lock(&ctx->rwlock);
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->filter.mtx);
#else
  EnterCriticalSection(&ctx->filter.mtx);
#endif
  ctx->filter.dedupe_window_ms[level] = window_ms;
  ctx->filter.dedupe[level].active = false;
  ctx->filter.dedupe[level].suppressed = 0;
  if (window_ms) ctx->filter.enabled = true;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->filter.mtx);
#else
  LeaveCriticalSection(&ctx->filter.mtx);
#endif
  rwlock_write_unlock(&ctx->rwlock);
}

void log_flush_suppressed(log_handle *ctx) {
  if (!ctx) return;
  stats_ensure_registered(ctx);

  int levels[LOG_LEVELS];
  unsigned counts[LOG_LEVELS];
  int n = 0;

#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->filter.mtx);
#else
  EnterCriticalSection(&ctx->filter.mtx);
#endif
  for (int l = 0; l < LOG_LEVELS; l++) {
    log_dedupe_group *g = &ctx->filter.dedupe[l];
    if (g->active && g->suppressed > 0) {
      levels[n] = l;
      counts[n] = g->suppressed;
      n++;
      g->suppressed = 0;
    }
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->filter.mtx);
#else
  LeaveCriticalSection(&ctx->filter.mtx);
#endif

  /* Emit outside the filter lock: emission takes the rwlock / queue locks. */
  for (int i = 0; i < n; i++) {
    filter_emit_summary(ctx, levels[i], counts[i], false);
  }
}

#endif /* LOG_FEATURE_FILTER */

/* ==================== Public API: logging entry points ==================== */

void log_log(log_handle *ctx, int level, const char *file, int line, const char *fmt, ...) {
  if (!ctx || !fmt) return;

  /* Named logger (B4): its own level/quiet gate, then emit through the shared
   * default context. The default's level must not filter it again, otherwise
   * a named logger could never be more verbose than the default. */
  bool apply_ctx_level = true;
#if LOG_FEATURE_NAMED
  if (ctx->base) {
    if (ctx->quiet || level < ctx->level) return;
    ctx = ctx->base;
    apply_ctx_level = false;
  }
#endif

  /* Register before taking the rwlock: log_enable_mpool()/ts_cache() hold
   * ctx->mutex while acquiring the rwlock, so taking ctx->mutex under the
   * rwlock would invert the lock order and deadlock. */
  stats_ensure_registered(ctx);

  rwlock_read_lock(&ctx->rwlock);

  if (apply_ctx_level && (ctx->quiet || level < ctx->level)) {
    rwlock_read_unlock(&ctx->rwlock);
    return;
  }

  STAT_INC(total_count);
  if (level >= 0 && level < LOG_LEVELS) {
    STAT_INC(level_counts[level]);
  }

  log_event ev = {0};
  ev.fmt = fmt;
  ev.file = file;
  ev.line = line;
  ev.level = level;
  ev.timestamp = get_timestamp_with_clock(ctx->clock_source);

#if LOG_FEATURE_CRASH_MODE
  /* Crash-safe mode never queues: messages must be in the kernel before
   * log_log returns. */
  bool async_path = ctx->async_enabled && !ctx->crash_safe;
#else
  bool async_path = ctx->async_enabled;
#endif

#if LOG_FEATURE_FILTER
  if (ctx->filter.enabled) {
    char *owned = NULL;
    va_start(ev.ap, fmt);
    const char *body = filter_render_body(fmt, ev.ap, &owned);
    va_end(ev.ap);

    log_filter_result fr;
    filter_check(ctx, level, body, NULL, 0, monotonic_seconds(), &fr);

    if (!async_path) {
      if (fr.emit_summary) {
        filter_emit_summary(ctx, fr.summary_level, fr.summary_count, true);
      }
      if (!fr.suppress) {
        STAT_INC(sync_writes);
        filter_dispatch_raw(ctx, level, file, line, ev.timestamp, body);
      }
      free(owned);
      rwlock_read_unlock(&ctx->rwlock);
      return;
    }

#if LOG_FEATURE_ASYNC
    {
      int queue_policy = ctx->queue_policy;
      rwlock_read_unlock(&ctx->rwlock);
      if (fr.emit_summary) {
        filter_emit_summary(ctx, fr.summary_level, fr.summary_count, false);
      }
      if (!fr.suppress) {
        int r = filter_enqueue(ctx, level, file, line, ev.timestamp, body, NULL,
                               queue_policy);
        if (r == 1) {
          STAT_INC(async_writes);
        } else if (r == 0) {
          STAT_INC(queue_drops);
        } else {
          rwlock_read_lock(&ctx->rwlock);
          STAT_INC(sync_writes);
          filter_dispatch_raw(ctx, level, file, line, ev.timestamp, body);
          rwlock_read_unlock(&ctx->rwlock);
        }
      }
      free(owned);
      return;
    }
#else
    free(owned);
    rwlock_read_unlock(&ctx->rwlock);
    return;
#endif
  }
#endif /* LOG_FEATURE_FILTER */

  if (!async_path) {
    STAT_INC(sync_writes);
    va_start(ev.ap, fmt);
    sync_format_and_dispatch(ctx, &ev);
    va_end(ev.ap);
    rwlock_read_unlock(&ctx->rwlock);
    return;
  }

#if LOG_FEATURE_ASYNC
  /* Enqueue without holding the ctx rwlock. Holding the read lock across a
   * blocking queue_push (LOG_QUEUE_BLOCK) deadlocks against a concurrent
   * configuration change: the blocked producer holds the read lock forever,
   * the writer thread's read lock is queued behind the pending write lock,
   * so the queue is never drained and space_cond is never signalled. */
  int queue_policy = ctx->queue_policy;
#if LOG_FEATURE_RING_QUEUE
  bool use_ring_queue = ctx->use_ring_queue;
#endif
  bool pushed = false;
  rwlock_read_unlock(&ctx->rwlock);

#if LOG_FEATURE_RING_QUEUE
  if (use_ring_queue) {
    va_start(ev.ap, fmt);
    pushed = ring_queue_push_vfmt(&ctx->ring_queue, fmt, ev.ap, file, level, line,
                                  ev.timestamp, queue_policy == LOG_QUEUE_BLOCK);
    va_end(ev.ap);
  } else
#endif /* LOG_FEATURE_RING_QUEUE */
  {
    va_start(ev.ap, fmt);
    log_queue_entry *entry = queue_entry_create(ctx, &ev);
    va_end(ev.ap);
    pushed = entry && queue_push(ctx, entry, queue_policy == LOG_QUEUE_BLOCK);
    if (!pushed && entry) queue_entry_destroy(ctx, entry);
  }

  if (pushed) {
    STAT_INC(async_writes);
  } else if (queue_policy == LOG_QUEUE_DROP) {
    STAT_INC(queue_drops);
  } else {
    /* FALLBACK_SYNC or BLOCK: write synchronously, no drop. Re-acquire the
     * read lock: dispatch touches handler state guarded by the rwlock. */
    rwlock_read_lock(&ctx->rwlock);
    va_start(ev.ap, fmt);
    sync_format_and_dispatch(ctx, &ev);
    va_end(ev.ap);
    STAT_INC(sync_writes);
    rwlock_read_unlock(&ctx->rwlock);
  }
#else
  (void)0;
  rwlock_read_unlock(&ctx->rwlock);
#endif /* LOG_FEATURE_ASYNC */
}

#if LOG_FEATURE_KV
void log_log_kv(log_handle *ctx, int level, const char *file, int line,
                const log_kv *kvs, int kv_count, const char *msg) {
  if (!ctx) return;

  bool apply_ctx_level = true;
#if LOG_FEATURE_NAMED
  if (ctx->base) {
    if (ctx->quiet || level < ctx->level) return;
    ctx = ctx->base;
    apply_ctx_level = false;
  }
#endif

  const char *body = msg ? msg : "";

  /* Encode the typed pairs into our own stack buffer before touching any
   * lock: pure CPU, and valid for the whole call (handlers read it inline on
   * the sync path; the async push copies it before returning). */
  char kv_buf[LOG_KV_ENCODE_MAX];
  bool kv_truncated = false;
  size_t kv_len = 0;
  if (kvs && kv_count > 0) {
    kv_len = kv_encode(kvs, kv_count, kv_buf, sizeof(kv_buf), &kv_truncated);
  }
  const char *kv_blob = kv_len ? kv_buf : NULL;

  /* Register before taking the rwlock (see log_log for the lock-order note). */
  stats_ensure_registered(ctx);

  rwlock_read_lock(&ctx->rwlock);

  if (apply_ctx_level && (ctx->quiet || level < ctx->level)) {
    rwlock_read_unlock(&ctx->rwlock);
    return;
  }

  STAT_INC(total_count);
  if (level >= 0 && level < LOG_LEVELS) {
    STAT_INC(level_counts[level]);
  }
  if (kv_truncated) STAT_INC(truncated_count);

  log_event ev = {0};
  ev.fmt = body;        /* literal: raw_msg short-circuits printf formatting */
  ev.raw_msg = body;
  ev.file = file;
  ev.line = line;
  ev.level = level;
  ev.timestamp = get_timestamp_with_clock(ctx->clock_source);
  ev.kv = kv_blob;

#if LOG_FEATURE_CRASH_MODE
  bool async_path = ctx->async_enabled && !ctx->crash_safe;
#else
  bool async_path = ctx->async_enabled;
#endif

#if LOG_FEATURE_FILTER
  if (ctx->filter.enabled) {
    log_filter_result fr;
    filter_check(ctx, level, body, kv_blob, kv_len, monotonic_seconds(), &fr);

    if (!async_path) {
      if (fr.emit_summary) {
        filter_emit_summary(ctx, fr.summary_level, fr.summary_count, true);
      }
      if (!fr.suppress) {
        STAT_INC(sync_writes);
        sync_format_and_dispatch(ctx, &ev);
      }
      rwlock_read_unlock(&ctx->rwlock);
      return;
    }

#if LOG_FEATURE_ASYNC
    {
      int queue_policy = ctx->queue_policy;
      rwlock_read_unlock(&ctx->rwlock);
      if (fr.emit_summary) {
        filter_emit_summary(ctx, fr.summary_level, fr.summary_count, false);
      }
      if (!fr.suppress) {
        int r = filter_enqueue(ctx, level, file, line, ev.timestamp, body, kv_blob,
                               queue_policy);
        if (r == 1) {
          STAT_INC(async_writes);
        } else if (r == 0) {
          STAT_INC(queue_drops);
        } else {
          rwlock_read_lock(&ctx->rwlock);
          STAT_INC(sync_writes);
          sync_format_and_dispatch(ctx, &ev);
          rwlock_read_unlock(&ctx->rwlock);
        }
      }
      return;
    }
#else
    rwlock_read_unlock(&ctx->rwlock);
    return;
#endif
  }
#endif /* LOG_FEATURE_FILTER */

  if (!async_path) {
    STAT_INC(sync_writes);
    sync_format_and_dispatch(ctx, &ev);
    rwlock_read_unlock(&ctx->rwlock);
    return;
  }

#if LOG_FEATURE_ASYNC
  int queue_policy = ctx->queue_policy;
#if LOG_FEATURE_RING_QUEUE
  bool use_ring_queue = ctx->use_ring_queue;
#endif
  bool pushed = false;
  rwlock_read_unlock(&ctx->rwlock);

#if LOG_FEATURE_RING_QUEUE
  if (use_ring_queue) {
    pushed = ring_queue_push_kv(&ctx->ring_queue, body, kv_blob, file, level, line,
                                ev.timestamp, queue_policy == LOG_QUEUE_BLOCK);
  } else
#endif /* LOG_FEATURE_RING_QUEUE */
  {
    log_queue_entry *entry = queue_entry_create_body(ctx, body, kv_blob, file,
                                                     level, line, ev.timestamp);
    pushed = entry && queue_push(ctx, entry, queue_policy == LOG_QUEUE_BLOCK);
    if (!pushed && entry) queue_entry_destroy(ctx, entry);
  }

  if (pushed) {
    STAT_INC(async_writes);
  } else if (queue_policy == LOG_QUEUE_DROP) {
    STAT_INC(queue_drops);
  } else {
    rwlock_read_lock(&ctx->rwlock);
    sync_format_and_dispatch(ctx, &ev);
    STAT_INC(sync_writes);
    rwlock_read_unlock(&ctx->rwlock);
  }
#else
  rwlock_read_unlock(&ctx->rwlock);
#endif /* LOG_FEATURE_ASYNC */
}
#endif /* LOG_FEATURE_KV */

/* ==================== Extended API: rotation, stats, format & crash ==================== */

#if LOG_FEATURE_FILE_OPS
void log_rotate(log_handle *ctx) {
  if (!ctx || !ctx->file_prefix) return;

  /* A caller that never logged still triggers rotation_count; make sure its
   * counters have a slot (no-op once registered). */
  stats_ensure_registered(ctx);

  rwlock_write_lock(&ctx->rwlock);

  /* On Windows, rename fails on open files; flush and close before rotating. */
  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].kind != HANDLER_FILE) continue;
    if (!ctx->handlers[i].fp) continue;
    if (ctx->handlers[i].fp == stderr || ctx->handlers[i].fp == stdout) continue;
    if (ctx->handlers[i].flush_fsync) {
      log_flush_fsync(ctx->handlers[i].fp);
    } else {
      fflush(ctx->handlers[i].fp);
    }
    if (ctx->handlers[i].owns_file) {
      fclose(ctx->handlers[i].fp);
      ctx->handlers[i].fp = NULL;
      ctx->handlers[i].udata = NULL;
    }
  }

  rotate_file(ctx, ctx->file_prefix);

  /* Reopen file handlers at the new path. */
  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].kind != HANDLER_FILE) continue;
    /* Skip stdout/stderr handlers (they have no real file) */
    if (ctx->handlers[i].fp == stdout || ctx->handlers[i].fp == stderr) continue;

    FILE *reopened = fopen(ctx->file_prefix, "a");
    if (reopened) {
      log_setup_stream_buffer(reopened);
      ctx->handlers[i].fp = reopened;
      ctx->handlers[i].udata = reopened;
      ctx->handlers[i].file_size = 0;
      /* Library opened the new file, so it now owns it */
      ctx->handlers[i].owns_file = true;
    } else {
      /* Never leave a handler pointing at a closed FILE. */
      ctx->handlers[i].fp = NULL;
      ctx->handlers[i].udata = NULL;
    }
  }
#if LOG_FEATURE_CRASH_MODE
  crash_refresh_targets(ctx);
#endif
  rwlock_write_unlock(&ctx->rwlock);
}
#endif /* LOG_FEATURE_FILE_OPS */

int log_get_stats(log_handle *ctx, log_stats *stats) {
  if (!ctx || !stats) return -1;
  stats_snapshot(ctx, stats);
  return 0;
}

#if LOG_FEATURE_JSON
int log_format_json(log_handle *ctx, log_event *ev, char *buf, size_t buf_size) {
  bool heap_msg = false;
  char *msg = format_message(ev, &heap_msg);
  if (!msg) return 0;

  char time_buf[64];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf),
                   ctx && ctx->enable_ts_cache);

  char *escaped_msg = json_escape(msg);
  if (heap_msg) free(msg);
  if (!escaped_msg) return 0;

  char *escaped_file = json_escape(ev->file ? ev->file : "");
  if (!escaped_file) { free(escaped_msg); return 0; }

  (void)ctx;
#if LOG_FEATURE_KV
  int kv_len = ev->kv ? (int)kv_render_json(ev->kv, NULL, 0) : 0;
#else
  int kv_len = 0;
#endif
  int head = snprintf(buf, buf_size,
    "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"",
    time_buf, level_strings[clamp_level(ev->level)],
    escaped_file, ev->line, escaped_msg);
  free(escaped_msg);
  free(escaped_file);
#if LOG_FEATURE_KV
  if (ev->kv && buf && buf_size > 0 && head > 0 && (size_t)head < buf_size) {
    kv_render_json(ev->kv, buf + head, buf_size - (size_t)head);
  }
#endif
  if (buf && buf_size > 0) {
    size_t brace = (head > 0 ? (size_t)head : 0) + (size_t)kv_len;
    if (brace + 1 < buf_size) {
      buf[brace] = '}';
      buf[brace + 1] = '\0';
    } else {
      buf[buf_size - 1] = '\0';
    }
  }
  return head + kv_len + 1;
}
#endif /* LOG_FEATURE_JSON */

void log_handler_set_level(log_handle *ctx, int handler_idx, int new_level) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return;
  
  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].level = new_level;
  rwlock_write_unlock(&ctx->rwlock);
}

int log_handler_set_flush(log_handle *ctx, int handler_idx, int policy, unsigned interval_ms) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return -1;
  if (policy != LOG_FLUSH_NEVER && policy != LOG_FLUSH_EVERY && policy != LOG_FLUSH_INTERVAL) {
    return -1;
  }
  if (policy == LOG_FLUSH_INTERVAL && interval_ms == 0) return -1;

  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].flush_policy = policy;
  ctx->handlers[handler_idx].flush_interval_ms = interval_ms;
  ctx->handlers[handler_idx].last_flush = 0.0;
  rwlock_write_unlock(&ctx->rwlock);
  return 0;
}

int log_handler_set_fsync(log_handle *ctx, int handler_idx, bool enable) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return -1;

  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].flush_fsync = enable;
  rwlock_write_unlock(&ctx->rwlock);
  return 0;
}

#if LOG_FEATURE_CRASH_MODE
int log_set_crash_safe(log_handle *ctx, bool enable) {
  if (!ctx) return -1;
  if (!enable) {
    /* Only clears the flag: flush policies stay whatever they are now. */
    rwlock_write_lock(&ctx->rwlock);
    ctx->crash_safe = false;
    rwlock_write_unlock(&ctx->rwlock);
    return 0;
  }
  /* Crash-safe mode requires the synchronous path: drain a running writer. */
  if (ctx->async_enabled) {
    if (log_set_async(ctx, false) != 0) return -1;
  }
  rwlock_write_lock(&ctx->rwlock);
  ctx->crash_safe = true;
  /* Per-line flush on every file-backed handler: everything written before
   * a fatal signal has already reached the kernel. */
  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].fp) {
      ctx->handlers[i].flush_policy = LOG_FLUSH_EVERY;
    }
  }
  rwlock_write_unlock(&ctx->rwlock);
  return 0;
}

int log_install_crash_handler(log_handle *ctx) {
#if defined(LOG_PLATFORM_POSIX)
  if (!ctx) return -1;
  crash_refresh_targets(ctx);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = crash_signal_handler;
  sigemptyset(&sa.sa_mask);
  /* SA_RESETHAND: the disposition is back to SIG_DFL while the handler
   * runs, so a nested fault kills the process instead of recursing. */
  sa.sa_flags = SA_RESETHAND;
  static const int crash_sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
  bool ok = true;
  for (size_t i = 0; i < sizeof(crash_sigs) / sizeof(crash_sigs[0]); i++) {
    if (sigaction(crash_sigs[i], &sa, NULL) != 0) {
      ok = false;
    }
  }
  return ok ? 0 : -1;
#else
  (void)ctx;
  return -1;  /* fatal-signal markers are POSIX-only */
#endif
}
#endif /* LOG_FEATURE_CRASH_MODE */

void log_handler_set_formatter(log_handle *ctx, int handler_idx, log_FormatFn new_fn) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  if (handler_idx >= 0 && handler_idx < ctx->handler_count) {
    ctx->handlers[handler_idx].format_fn = new_fn;
  }
  rwlock_write_unlock(&ctx->rwlock);
}

void log_configure_pipeline(log_handle* ctx, log_stage_function* stages, int stage_count) {
  rwlock_write_lock(&ctx->rwlock);

  for (int i = 0; i < stage_count && i < ctx->handler_count; i++) {
    if (stages[i].transform) {
      ctx->format_fn = stages[i].transform;
    }
    if (stages[i].output) {
      ctx->handlers[i].fn = stages[i].output;
      ctx->handlers[i].kind = HANDLER_CUSTOM;
    }
  }

  rwlock_write_unlock(&ctx->rwlock);
}

void log_enable_text_format(log_handle* ctx) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].kind == HANDLER_STDOUT) {
      ctx->handlers[i].fn = stdout_handler;
    } else if (ctx->handlers[i].kind == HANDLER_FILE) {
      ctx->handlers[i].fn = file_handler_wrapper;
    }
  }
  rwlock_write_unlock(&ctx->rwlock);
}

#if LOG_FEATURE_JSON
void log_enable_json_format(log_handle* ctx) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].kind == HANDLER_STDOUT || ctx->handlers[i].kind == HANDLER_FILE) {
      ctx->handlers[i].fn = json_handler;
    }
  }
  rwlock_write_unlock(&ctx->rwlock);
}
#endif /* LOG_FEATURE_JSON */

/* Thread ID support implementation */
#if LOG_FEATURE_THREAD_ID
void log_enable_thread_id(log_handle *ctx, int handler_idx, bool enable) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return;

  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].show_thread_id = enable;
  rwlock_write_unlock(&ctx->rwlock);
}
#endif /* LOG_FEATURE_THREAD_ID */

/* ==================== Syslog integration ==================== */

/* Syslog support implementation */
#if LOG_FEATURE_SYSLOG
#if LOG_HAVE_SYSLOG
int log_level_to_syslog(int level) {
  switch (clamp_level(level)) {
    case LOG_TRACE: return 7; /* LOG_DEBUG */
    case LOG_DEBUG: return 7; /* LOG_DEBUG */
    case LOG_INFO:  return 6; /* LOG_INFO */
    case LOG_WARN:  return 4; /* LOG_WARNING */
    case LOG_ERROR: return 3; /* LOG_ERR */
    case LOG_FATAL: return 2; /* LOG_CRIT */
    default:        return 6; /* LOG_INFO */
  }
}

static void syslog_handler(log_handle *ctx, log_event *ev) {
  if (!ctx) return;

  int priority = LOG_USER | log_level_to_syslog(ev->level);

  bool heap_msg = false;
  char *msg = format_message(ev, &heap_msg);
  if (!msg) return;

  if (ctx->handlers && ctx->handler_count > 0) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active) {
        char full_msg[5120];
        snprintf(full_msg, sizeof(full_msg), "[%lu] %s:%d: %s",
                LOG_GET_THREAD_ID(),
                ev->file ? ev->file : "", ev->line, msg);
        syslog(priority, "%s", full_msg);
        if (heap_msg) free(msg);
        return;
      }
    }
  }

  syslog(priority, "%s:%d: %s", ev->file ? ev->file : "", ev->line, msg);
  if (heap_msg) free(msg);
}

int log_add_syslog_handler(log_handle *ctx, const char *ident, int facility, int level) {
  if (!ctx) return -1;

  rwlock_write_lock(&ctx->rwlock);

  if (ctx->handler_count >= ctx->handler_capacity) {
    rwlock_write_unlock(&ctx->rwlock);
    return -1;
  }

  bool need_to_open_syslog = !ctx->syslog_enabled_global && ident != NULL;
  if (need_to_open_syslog) {
    free(ctx->syslog_ident);
    ctx->syslog_ident = strdup(ident);
    if (!ctx->syslog_ident) {
      rwlock_write_unlock(&ctx->rwlock);
      return -1;
    }
    ctx->syslog_facility = facility;
  }

  log_handler *h = &ctx->handlers[ctx->handler_count++];
  h->fn = syslog_handler;
  h->udata = NULL;
  h->level = level;
  h->active = true;
  h->fp = NULL;
  h->filename = NULL;
  h->file_size = 0;
  h->syslog_enabled = need_to_open_syslog;
  h->syslog_facility = facility;
  h->show_thread_id = false;
  h->kind = HANDLER_SYSLOG;
  h->format_fn = NULL;
  h->flush_policy = LOG_FLUSH_NEVER;
  h->flush_interval_ms = 0;
  h->flush_fsync = false;
  h->last_flush = 0.0;
  h->owns_file = false;

  if (need_to_open_syslog) {
    openlog(ctx->syslog_ident, LOG_PID | LOG_NDELAY, facility);
    ctx->syslog_enabled_global = true;
  }

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}
#else /* !LOG_HAVE_SYSLOG: Windows stubs (feature enabled, no syslog API) */
int log_level_to_syslog(int level) {
  (void)level;
  return 0;
}

int log_add_syslog_handler(log_handle *ctx, const char *ident, int facility, int level) {
  (void)ctx; (void)ident; (void)facility; (void)level;
  return -1;
}
#endif /* LOG_HAVE_SYSLOG */

#if LOG_HAVE_SYSLOG
void log_handler_enable_syslog(log_handle *ctx, int handler_idx, bool enable) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return;

  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].syslog_enabled = enable;

  if (enable && !ctx->syslog_enabled_global) {
    openlog(ctx->syslog_ident ? ctx->syslog_ident : "log",
            LOG_PID | LOG_NDELAY, ctx->handlers[handler_idx].syslog_facility);
    ctx->syslog_enabled_global = true;
  }

  rwlock_write_unlock(&ctx->rwlock);
}
#else /* !LOG_HAVE_SYSLOG */
void log_handler_enable_syslog(log_handle *ctx, int handler_idx, bool enable) {
  (void)ctx; (void)handler_idx; (void)enable;
}
#endif /* LOG_HAVE_SYSLOG */
#endif /* LOG_FEATURE_SYSLOG */
