/*
 * Enhanced C11 Log Library with async support, rotation, and performance stats
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

static LOG_THREAD_LOCAL log_thread_stats tl_stats = {0};

#if LOG_FEATURE_STATS
#define STAT_INC(f) (tl_stats.f++)
#define STAT_LOAD(f) (tl_stats.f)
#define STAT_STORE(f, v) (tl_stats.f = (v))
#else
#define STAT_INC(f) ((void)0)
#define STAT_LOAD(f) (0)
#define STAT_STORE(f, v) ((void)(v))
#endif

static void reset_thread_stats(void) {
#if LOG_FEATURE_STATS
  memset(&tl_stats, 0, sizeof(tl_stats));
#endif
}

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

/* Lock-free Queue Implementation */
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
    tl_stats.queue_blocked++;
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

/* Build the line prefix (time, level, optional thread id, file:line, custom formatter).
 * Returns the number of characters written into buf (excluding NUL). */
static int format_prefix(log_handle *ctx, log_event *ev, char *buf, size_t buf_size,
                         bool show_tid, bool use_color) {
  if (ctx && ctx->format_fn) {
    int n = ctx->format_fn(ctx, ev, buf, buf_size);
    return n < 0 ? 0 : n;
  }
  char time_buf[64];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf),
                   ctx && ctx->enable_ts_cache);
#ifdef LOG_USE_COLOR
  if (use_color) {
    if (show_tid) {
      return snprintf(buf, buf_size, "%s %s%-5s\x1b[0m \x1b[90m[%lu] %s:%d:\x1b[0m ",
                      time_buf, level_colors[ev->level], level_strings[ev->level],
                      LOG_GET_THREAD_ID(), ev->file ? ev->file : "", ev->line);
    }
    return snprintf(buf, buf_size, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
                    time_buf, level_colors[ev->level], level_strings[ev->level],
                    ev->file ? ev->file : "", ev->line);
  }
#else
  (void)use_color;
#endif
  if (show_tid) {
    return snprintf(buf, buf_size, "%s %-5s [%lu] %s:%d: ",
                    time_buf, level_strings[ev->level], LOG_GET_THREAD_ID(),
                    ev->file ? ev->file : "", ev->line);
  }
  return snprintf(buf, buf_size, "%s %-5s %s:%d: ",
                  time_buf, level_strings[ev->level],
                  ev->file ? ev->file : "", ev->line);
}

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
  if (ctx) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active && ctx->handlers[i].udata == ev->udata) {
        show_tid = true;
        break;
      }
    }
  }

  char prefix[512];
  format_prefix(ctx, ev, prefix, sizeof(prefix), show_tid,
#ifdef LOG_USE_COLOR
                true
#else
                false
#endif
               );
  fprintf(ev->udata, "%s%s\n", prefix, msg);
  if (heap_msg) free(msg);
}

static void file_handler_internal(log_handle *ctx, log_event *ev, int handler_idx) {
  bool heap_msg = false;
  char *msg = format_message(ev, &heap_msg);
  if (!msg) return;

  char prefix[512];
  int prefix_len = format_prefix(ctx, ev, prefix, sizeof(prefix),
                                 ctx->handlers[handler_idx].show_thread_id, false);
  if (prefix_len < 0) { if (heap_msg) free(msg); return; }
  if ((size_t)prefix_len >= sizeof(prefix)) prefix_len = (int)sizeof(prefix) - 1;

  size_t msg_len = strlen(msg);
  FILE *fp = ctx->handlers[handler_idx].fp;
  /* Assemble prefix+msg+'\n' and write with a SINGLE fwrite: stdio holds
   * the stream lock for the whole line, so concurrent writers cannot tear
   * lines apart. The common case assembles into thread-local storage (no
   * heap); oversized lines fall back to an exact-size malloc. */
  size_t total = (size_t)prefix_len + msg_len + 1;
  static LOG_THREAD_LOCAL char tl_line_buf[512 + LOG_MSG_BUF_SIZE + 2];
  size_t written;
  if (total <= sizeof(tl_line_buf)) {
    memcpy(tl_line_buf, prefix, (size_t)prefix_len);
    memcpy(tl_line_buf + prefix_len, msg, msg_len);
    tl_line_buf[prefix_len + msg_len] = '\n';
    written = fwrite(tl_line_buf, 1, total, fp);
  } else {
    char *buf = malloc(total);
    if (!buf) { if (heap_msg) free(msg); return; }
    memcpy(buf, prefix, (size_t)prefix_len);
    memcpy(buf + prefix_len, msg, msg_len);
    buf[prefix_len + msg_len] = '\n';
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

  const char *lvl_str = level_strings[ev->level];
  int line_val = ev->line;

  size_t needed = 0;
  if (show_tid) {
    needed = snprintf(NULL, 0,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"thread_id\": %lu, \"message\": \"%s\"}",
      time_buf, lvl_str, escaped_file, line_val, LOG_GET_THREAD_ID(), escaped_msg);
  } else {
    needed = snprintf(NULL, 0,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
      time_buf, lvl_str, escaped_file, line_val, escaped_msg);
  }

  char *buf = malloc(needed + 2);
  if (!buf) { free(escaped_msg); free(escaped_file); return; }
  if (show_tid) {
    snprintf(buf, needed + 1,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"thread_id\": %lu, \"message\": \"%s\"}",
      time_buf, lvl_str, escaped_file, line_val, LOG_GET_THREAD_ID(), escaped_msg);
  } else {
    snprintf(buf, needed + 1,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
      time_buf, lvl_str, escaped_file, line_val, escaped_msg);
  }

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
      rwlock_read_lock(&ctx->rwlock);
      for (int k = 0; k < n; k++) {
        log_ring_entry *re = &batch.entries[k];
        log_event ev = {0};
        ev.level = re->level;
        ev.file = batch.large_file[k] ? batch.large_file[k] : re->file;
        ev.line = re->line;
        ev.timestamp = re->timestamp;
        ev.raw_msg = batch.large_msg[k] ? batch.large_msg[k] : re->msg;
        for (int i = 0; i < ctx->handler_count; i++) {
          if (ctx->handlers[i].active && ctx->handlers[i].fn && re->level >= ctx->handlers[i].level) {
            ev.udata = ctx->handlers[i].udata;
            ctx->handlers[i].fn(ctx, &ev);
            log_apply_flush(ctx, i);
          }
        }
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
      rwlock_read_lock(&ctx->rwlock);
      for (int k = 0; k < n; k++) {
        log_queue_entry *entry = entries[k];
        log_event ev = {0};
        ev.level = entry->level;
        ev.file = entry->file;
        ev.line = entry->line;
        ev.timestamp = entry->timestamp;
        ev.raw_msg = entry->msg;
        for (int i = 0; i < ctx->handler_count; i++) {
          if (ctx->handlers[i].active && ctx->handlers[i].fn && entry->level >= ctx->handlers[i].level) {
            ev.udata = ctx->handlers[i].udata;
            ctx->handlers[i].fn(ctx, &ev);
            log_apply_flush(ctx, i);
          }
        }
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
  reset_thread_stats();

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

void log_destroy(log_handle *ctx) {
  if (!ctx) return;

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
  }
#if LOG_FEATURE_STATIC_ALLOC
  /* handlers storage is embedded in the caller-provided context */
#else
  free(ctx->handlers);
#endif

  free(ctx->file_prefix);
  free(ctx->syslog_ident);
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

/* Snapshot stats from thread-local storage into a plain struct */
static void stats_snapshot(log_handle *ctx, log_stats *stats) {
  (void)ctx;
  stats->total_count = tl_stats.total_count;
  for (int i = 0; i < LOG_LEVELS; i++) {
    stats->level_counts[i] = tl_stats.level_counts[i];
  }
  stats->queue_drops = tl_stats.queue_drops;
  stats->queue_blocked = tl_stats.queue_blocked;
  stats->rotation_count = tl_stats.rotation_count;
  stats->async_writes = tl_stats.async_writes;
  stats->sync_writes = tl_stats.sync_writes;
  stats->truncated_count = tl_stats.truncated_count;
}

#if LOG_FEATURE_STATS
void log_get_perf_stats(log_handle *ctx, log_stats *stats) {
  if (!ctx || !stats) return;
  stats_snapshot(ctx, stats);
}
#endif /* LOG_FEATURE_STATS */

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

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}

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

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}
#endif /* LOG_FEATURE_FILE_OPS */

void log_remove_handler(log_handle *ctx, int idx) {
  if (!ctx || idx < 0 || idx >= ctx->handler_count) return;
  
  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[idx].active = false;
  rwlock_write_unlock(&ctx->rwlock);
}

/* Format the message body once (thread-local buffer; exact-size heap
 * fallback for oversized messages, truncation in static mode), then
 * dispatch synchronously to every active handler. Shared by the sync path
 * and the async FALLBACK_SYNC path. Caller holds the ctx read lock and has
 * already va_start()ed ev->ap (consumed here). */
static void sync_format_and_dispatch(log_handle *ctx, log_event *ev) {
  char *big = NULL;
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

  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].active && ctx->handlers[i].fn && ev->level >= ctx->handlers[i].level) {
      ev->udata = ctx->handlers[i].udata;
      ctx->handlers[i].fn(ctx, ev);
      log_apply_flush(ctx, i);
    }
  }
  free(big);
}

void log_log(log_handle *ctx, int level, const char *file, int line, const char *fmt, ...) {
  if (!ctx || !fmt) return;

  rwlock_read_lock(&ctx->rwlock);

  if (ctx->quiet || level < ctx->level) {
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

#if LOG_FEATURE_FILE_OPS
void log_rotate(log_handle *ctx) {
  if (!ctx || !ctx->file_prefix) return;

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
  int n = snprintf(buf, buf_size,
    "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
    time_buf, level_strings[ev->level],
    escaped_file, ev->line, escaped_msg);
  free(escaped_msg);
  free(escaped_file);
  return n;
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
  rwlock_write_lock(&ctx->rwlock);
  if (handler_idx >= 0 && handler_idx < ctx->handler_count) {
    ctx->handlers[handler_idx].fn = NULL; /* mark for kind-based resolution */
  }
  ctx->format_fn = new_fn;
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

/* Syslog support implementation */
#if LOG_FEATURE_SYSLOG
#if LOG_HAVE_SYSLOG
int log_level_to_syslog(int level) {
  switch (level) {
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
