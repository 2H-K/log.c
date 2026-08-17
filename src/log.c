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
#endif

#ifdef LOG_PLATFORM_WINDOWS
#include <windows.h>
#include <sys/stat.h>
#endif

#ifdef __STDC_VERSION__
#if __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(int) >= 4, "int must be at least 32 bits");
#endif
#endif

/* ==================== Platform-specific helpers ==================== */

#ifdef LOG_PLATFORM_POSIX
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
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER uli;
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  /* Convert from 100ns intervals since 1601 to seconds since 1970 */
  return (double)(uli.QuadPart - 116444736000000000LL) / 10000000.0;
#endif
}

static double get_timestamp(void) {
  return get_timestamp_with_clock(LOG_CLOCK_REALTIME);
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

#define MAX_HANDLERS 32
#define DEFAULT_QUEUE_SIZE LOG_MAX_QUEUE_SIZE

#define LOG_MPOOL_CHUNK_SIZE 64
#define LOG_MPOOL_MAX_CHUNKS 64

#define ARENA_BLOCK_SIZE (64 * 1024)

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

static void aggregate_stats(log *ctx) {
#if LOG_FEATURE_STATS
  ctx->stats.total_count = tl_stats.total_count;
  for (int i = 0; i < LOG_LEVELS; i++) {
    ctx->stats.level_counts[i] = tl_stats.level_counts[i];
  }
  ctx->stats.queue_drops = tl_stats.queue_drops;
  ctx->stats.sync_writes = tl_stats.sync_writes;
#endif
}

static log *DEFAULT_LOG = NULL;
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

#endif /* LOG_FEATURE_MPOOL */

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

static LOG_THREAD_LOCAL log_arena *tl_arena = NULL;
#if defined(LOG_PLATFORM_POSIX)
static pthread_key_t arena_key;
static pthread_once_t arena_key_once = PTHREAD_ONCE_INIT;
#else
static DWORD arena_fls_key = 0;
static bool arena_fls_initialized = false;
#endif

static void arena_destroy(void);
#if defined(LOG_PLATFORM_POSIX)
static void arena_destroy_wrapper(void *val) {
  (void)val;
  arena_destroy();
}
static void arena_key_create(void) {
  pthread_key_create(&arena_key, arena_destroy_wrapper);
}
#else
static void NTAPI arena_fls_callback(PVOID p) {
  (void)p;
  arena_destroy();
}
#endif

static void* arena_alloc(size_t size) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_once(&arena_key_once, arena_key_create);
  if (!tl_arena) {
    pthread_setspecific(arena_key, (void*)1);
  }
#else
  if (!arena_fls_initialized) {
    DWORD fls = FlsAlloc(arena_fls_callback);
    if (fls == FLS_OUT_OF_INDEXES) return NULL;
    arena_fls_key = fls;
    arena_fls_initialized = true;
  }
  if (!tl_arena) {
    FlsSetValue(arena_fls_key, (PVOID)1);
  }
#endif
  if (!tl_arena || tl_arena->offset + size > tl_arena->capacity) {
    size_t cap = ARENA_BLOCK_SIZE;
    if (size > cap) cap = size * 2;
    log_arena *a = malloc(sizeof(log_arena));
    if (!a) return NULL;
    a->buffer = malloc(cap);
    if (!a->buffer) { free(a); return NULL; }
    a->offset = 0;
    a->capacity = cap;
    a->next = tl_arena;
    tl_arena = a;
  }
  char *ptr = tl_arena->buffer + tl_arena->offset;
  tl_arena->offset += size;
  return ptr;
}

static void arena_reset(void) {
  log_arena *a = tl_arena;
  while (a) {
    a->offset = 0;
    a = a->next;
  }
}

static void arena_destroy(void) {
  log_arena *a = tl_arena;
  while (a) {
    log_arena *next = a->next;
    free(a->buffer);
    free(a);
    a = next;
  }
  tl_arena = NULL;
}

static LOG_THREAD_LOCAL log_ts_cache ts_cache_local;

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
static log_queue_entry* queue_entry_create(log *ctx, log_event *ev) {
  log_mpool *mp = &ctx->mpool;
  bool use_mpool = ctx->enable_mpool;
  log_queue_entry *entry = use_mpool ? mpool_alloc(mp) : malloc(sizeof(log_queue_entry));
  if (!entry) return NULL;

  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int len = vsnprintf(NULL, 0, ev->fmt, args_copy);
  va_end(args_copy);

  if (len < 0) goto fail;

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
  } else {
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
  if (use_mpool) {
    mpool_free(mp, entry);
  } else {
    free(entry->msg);
    free(entry->file);
    free(entry);
  }
  return NULL;
}

static void queue_entry_destroy(log *ctx, log_queue_entry *entry) {
  if (!entry) return;
  if (ctx->enable_mpool) {
    mpool_free(&ctx->mpool, entry);
  } else {
    free(entry->msg);
    free(entry->file);
    free(entry);
  }
}

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

static bool queue_push(log *ctx, log_queue_entry *entry, bool blocking) {
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

static log_queue_entry* queue_pop(log_queue *q) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&q->mtx);
  while (!q->closed && q->size == 0) {
    pthread_cond_wait(&q->cond, &q->mtx);
  }
  if (q->size == 0) {
    pthread_mutex_unlock(&q->mtx);
    return NULL;
  }
  log_queue_entry *entry = q->head;
  q->head = entry->next;
  if (!q->head) {
    q->tail = NULL;
  }
  q->size--;
  pthread_cond_signal(&q->space_cond);
  pthread_mutex_unlock(&q->mtx);
  return entry;
#else
  EnterCriticalSection(&q->mtx);
  while (!q->closed && q->size == 0) {
    SleepConditionVariableCS(&q->cond, &q->mtx, INFINITE);
  }
  if (q->size == 0) {
    LeaveCriticalSection(&q->mtx);
    return NULL;
  }
  log_queue_entry *entry = q->head;
  q->head = entry->next;
  if (!q->head) {
    q->tail = NULL;
  }
  q->size--;
  WakeConditionVariable(&q->space_cond);
  LeaveCriticalSection(&q->mtx);
  return entry;
#endif
}

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

static void ring_queue_init(log_ring_queue *rq, size_t capacity) {
  size_t cap = 1;
  while (cap < capacity) cap <<= 1;
  rq->capacity = cap;
  rq->mask = cap - 1;
  rq->buffer = calloc(cap, sizeof(log_ring_entry));
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

static void ring_queue_destroy(log_ring_queue *rq) {
  if (!rq->buffer) return;
  free(rq->buffer);
  rq->buffer = NULL;
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_destroy(&rq->mtx);
  pthread_cond_destroy(&rq->cond);
  pthread_cond_destroy(&rq->space_cond);
#else
  DeleteCriticalSection(&rq->mtx);
#endif
}

static bool ring_queue_push(log_ring_queue *rq, const char *msg, const char *file,
                            int level, int line, double timestamp, bool blocking) {
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
  size_t msg_len = strlen(msg);
  if (msg_len >= sizeof(entry->msg)) {
    entry->has_large_msg = true;
    char *large = malloc(msg_len + 1);
    if (!large) {
#if defined(LOG_PLATFORM_POSIX)
      pthread_mutex_unlock(&rq->mtx);
#else
      LeaveCriticalSection(&rq->mtx);
#endif
      return false;
    }
    memcpy(large, msg, msg_len + 1);
    *(char**)entry->msg = large;
  } else {
    entry->has_large_msg = false;
    memcpy(entry->msg, msg, msg_len + 1);
  }
  size_t file_len = file ? strlen(file) : 0;
  if (file_len >= sizeof(entry->file)) {
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
  } else {
    entry->has_large_msg = false;
  }

  size_t file_len = file ? strlen(file) : 0;
  if (file_len >= sizeof(entry->file)) {
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

static bool ring_queue_pop(log_ring_queue *rq, log_ring_entry *entry, char **msg_ptr, char **file_ptr) {
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&rq->mtx);
#else
  EnterCriticalSection(&rq->mtx);
#endif
  size_t head = atomic_load(&rq->head);
  while (head == atomic_load(&rq->tail) && !rq->closed) {
#if defined(LOG_PLATFORM_POSIX)
    pthread_cond_wait(&rq->cond, &rq->mtx);
#else
    SleepConditionVariableCS(&rq->cond, &rq->mtx, INFINITE);
#endif
  }
  if (head == atomic_load(&rq->tail)) {
#if defined(LOG_PLATFORM_POSIX)
    pthread_mutex_unlock(&rq->mtx);
#else
    LeaveCriticalSection(&rq->mtx);
#endif
    return false;
  }
  log_ring_entry *e = &rq->buffer[head & rq->mask];
  *entry = *e;
  if (e->has_large_msg) {
    *msg_ptr = *(char**)e->msg;
    e->has_large_msg = false;
  } else {
    *msg_ptr = strdup(e->msg);
  }
  if (e->has_large_file) {
    *file_ptr = *(char**)e->file;
    e->has_large_file = false;
  } else {
    *file_ptr = strdup(e->file);
  }
  atomic_store(&rq->head, head + 1);
#if defined(LOG_PLATFORM_POSIX)
  pthread_cond_signal(&rq->space_cond);
  pthread_mutex_unlock(&rq->mtx);
#else
  WakeConditionVariable(&rq->space_cond);
  LeaveCriticalSection(&rq->mtx);
#endif
  return true;
}

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

/* Format helpers: the message is formatted exactly once, then shared by all handlers. */
static char* format_message(log_event *ev) {
  if (ev->raw_msg) {
    return strdup(ev->raw_msg);
  }
  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int len = vsnprintf(NULL, 0, ev->fmt, args_copy);
  va_end(args_copy);
  if (len < 0) return NULL;
  char *msg = malloc((size_t)len + 1);
  if (!msg) return NULL;
  vsnprintf(msg, (size_t)len + 1, ev->fmt, ev->ap);
  return msg;
}

/* Build the line prefix (time, level, optional thread id, file:line, custom formatter).
 * Returns the number of characters written into buf (excluding NUL). */
static int format_prefix(log *ctx, log_event *ev, char *buf, size_t buf_size,
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

/* File rotation */
static void rotate_file(log *ctx, const char *filename) {
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

/* Output handlers */
static void stdout_handler(log *ctx, log_event *ev) {
  char *msg = format_message(ev);
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
  free(msg);
}

static void file_handler_internal(log *ctx, log_event *ev, int handler_idx) {
  char *msg = format_message(ev);
  if (!msg) return;

  char prefix[512];
  int prefix_len = format_prefix(ctx, ev, prefix, sizeof(prefix),
                                 ctx->handlers[handler_idx].show_thread_id, false);
  if (prefix_len < 0) { free(msg); return; }
  if ((size_t)prefix_len >= sizeof(prefix)) prefix_len = (int)sizeof(prefix) - 1;

  size_t msg_len = strlen(msg);
  size_t total = (size_t)prefix_len + msg_len + 2;
  char *buf = malloc(total);
  if (!buf) { free(msg); return; }
  memcpy(buf, prefix, prefix_len);
  memcpy(buf + prefix_len, msg, msg_len);
  buf[prefix_len + msg_len] = '\n';
  buf[prefix_len + msg_len + 1] = '\0';

  FILE *fp = ctx->handlers[handler_idx].fp;
  size_t written = fwrite(buf, 1, total - 1, fp);
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_lock(&ctx->file_mtx);
#else
  EnterCriticalSection(&ctx->file_mtx);
#endif
  ctx->handlers[handler_idx].file_size += written;

  if (ctx->handlers[handler_idx].file_size >= ctx->max_file_size) {
    if (ctx->handlers[handler_idx].owns_file) {
      fflush(fp);
      if (fp != stderr && fp != stdout) {
        fclose(fp);
      }
      rotate_file(ctx, ctx->file_prefix);

      ctx->handlers[handler_idx].fp = fopen(ctx->file_prefix, "a");
      if (ctx->handlers[handler_idx].fp) {
        ctx->handlers[handler_idx].file_size = 0;
        /* Update udata to match new fp so handler can be found */
        ctx->handlers[handler_idx].udata = ctx->handlers[handler_idx].fp;
      }
    } else {
      ctx->handlers[handler_idx].file_size = 0;
    }
  }
#if defined(LOG_PLATFORM_POSIX)
  pthread_mutex_unlock(&ctx->file_mtx);
#else
  LeaveCriticalSection(&ctx->file_mtx);
#endif

  free(buf);
  free(msg);
}

static void file_handler_wrapper(log *ctx, log_event *ev) {
  FILE *target_fp = ev->udata;

  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].udata == target_fp && ctx->handlers[i].fp) {
      file_handler_internal(ctx, ev, i);
      return;
    }
  }

  char *msg = format_message(ev);
  if (msg) {
    fprintf(ev->udata, "%s\n", msg);
    free(msg);
  }
}

#if LOG_FEATURE_JSON
static void json_handler(log *ctx, log_event *ev) {
  char *msg = format_message(ev);
  if (!msg) return;

  char time_buf[64];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf),
                   ctx && ctx->enable_ts_cache);

  size_t msg_len = strlen(msg);
  size_t esc_len = 0;
  for (size_t i = 0; i < msg_len; i++) {
    switch (msg[i]) {
      case '"': case '\\': case '\n': case '\r': case '\t': esc_len += 2; break;
      default: esc_len += 1; break;
    }
  }

  char *escaped_msg = malloc(esc_len + 1);
  if (!escaped_msg) { free(msg); return; }
  size_t j = 0;
  for (size_t i = 0; i < msg_len; i++) {
    switch (msg[i]) {
      case '"':  escaped_msg[j++] = '\\'; escaped_msg[j++] = '"'; break;
      case '\\': escaped_msg[j++] = '\\'; escaped_msg[j++] = '\\'; break;
      case '\n': escaped_msg[j++] = '\\'; escaped_msg[j++] = 'n'; break;
      case '\r': escaped_msg[j++] = '\\'; escaped_msg[j++] = 'r'; break;
      case '\t': escaped_msg[j++] = '\\'; escaped_msg[j++] = 't'; break;
      default:   escaped_msg[j++] = msg[i]; break;
    }
  }
  escaped_msg[j] = '\0';
  free(msg);

  bool show_tid = false;
  if (ctx) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active && ctx->handlers[i].udata == ev->udata) {
        show_tid = true;
        break;
      }
    }
  }

  const char *file_str = ev->file ? ev->file : "";
  const char *lvl_str = level_strings[ev->level];
  int line_val = ev->line;

  size_t needed = 0;
  if (show_tid) {
    needed = snprintf(NULL, 0,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"thread_id\": %lu, \"message\": \"%s\"}",
      time_buf, lvl_str, file_str, line_val, LOG_GET_THREAD_ID(), escaped_msg);
  } else {
    needed = snprintf(NULL, 0,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
      time_buf, lvl_str, file_str, line_val, escaped_msg);
  }

  char *buf = malloc(needed + 2);
  if (!buf) { free(escaped_msg); return; }
  if (show_tid) {
    snprintf(buf, needed + 1,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"thread_id\": %lu, \"message\": \"%s\"}",
      time_buf, lvl_str, file_str, line_val, LOG_GET_THREAD_ID(), escaped_msg);
  } else {
    snprintf(buf, needed + 1,
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
      time_buf, lvl_str, file_str, line_val, escaped_msg);
  }

  fprintf(ev->udata, "%s\n", buf);
  free(escaped_msg);
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
  log *ctx = (log*)arg;

  if (ctx->use_ring_queue) {
    while (true) {
      log_ring_entry ring_entry;
      char *msg = NULL, *file = NULL;
      if (!ring_queue_pop(&ctx->ring_queue, &ring_entry, &msg, &file)) {
        break;
      }
      double queue_latency = (get_timestamp_with_clock(ctx->clock_source) - ring_entry.timestamp) * 1000.0;
      log_event ev = {0};
      ev.level = ring_entry.level;
      ev.file = file;
      ev.line = ring_entry.line;
      ev.timestamp = ring_entry.timestamp;
      ev.raw_msg = msg;
      rwlock_read_lock(&ctx->rwlock);
      for (int i = 0; i < ctx->handler_count; i++) {
        if (ctx->handlers[i].active && ctx->handlers[i].fn && ring_entry.level >= ctx->handlers[i].level) {
          ev.udata = ctx->handlers[i].udata;
          ctx->handlers[i].fn(ctx, &ev);
        }
      }
      rwlock_read_unlock(&ctx->rwlock);
      free(msg);
      free(file);
    }
  } else {
    while (true) {
      log_queue_entry *entry = queue_pop(&ctx->queue);
      if (!entry) {
        break;
      }
      double queue_latency = (get_timestamp_with_clock(ctx->clock_source) - entry->timestamp) * 1000.0;
      log_event ev = {0};
      ev.level = entry->level;
      ev.file = entry->file;
      ev.line = entry->line;
      ev.timestamp = entry->timestamp;
      ev.raw_msg = entry->msg;
      rwlock_read_lock(&ctx->rwlock);
      for (int i = 0; i < ctx->handler_count; i++) {
        if (ctx->handlers[i].active && ctx->handlers[i].fn && entry->level >= ctx->handlers[i].level) {
          ev.udata = ctx->handlers[i].udata;
          ctx->handlers[i].fn(ctx, &ev);
        }
      }
      rwlock_read_unlock(&ctx->rwlock);
      /* format_message returns a strdup, so free the original */
      free(entry->msg);
      free(entry->file);
      entry->msg = NULL;
      entry->file = NULL;
      queue_entry_destroy(ctx, entry);
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
log* log_create(void) {
  log *ctx = calloc(1, sizeof(log));
  reset_thread_stats();
  if (!ctx) return NULL;

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

  queue_init(&ctx->queue, DEFAULT_QUEUE_SIZE);

  ctx->handler_capacity = MAX_HANDLERS;
  ctx->handlers = calloc(MAX_HANDLERS, sizeof(log_handler));
  ctx->handler_count = 0;

  ctx->format_fn = NULL;

  ctx->syslog_ident = NULL;
  ctx->syslog_facility = LOG_USER;
  ctx->syslog_enabled_global = false;

  mpool_init(&ctx->mpool, LOG_MPOOL_MAX_CHUNKS * LOG_MPOOL_CHUNK_SIZE);
  ctx->enable_ts_cache = true;
  ctx->enable_mpool = true;

  ring_queue_init(&ctx->ring_queue, DEFAULT_QUEUE_SIZE);
  ctx->use_ring_queue = true;
  ctx->clock_source = LOG_CLOCK_REALTIME_COARSE;

  log_add_handler(ctx, stdout_handler, stderr, LOG_TRACE);
  if (ctx->handler_count > 0) {
    ctx->handlers[0].kind = HANDLER_STDOUT;
    ctx->handlers[0].owns_file = false;
  }

  return ctx;
}

void log_destroy(log *ctx) {
  if (!ctx) return;

  if (ctx->async_enabled) {
    if (ctx->use_ring_queue) {
      ring_queue_shutdown(&ctx->ring_queue);
    } else {
      queue_shutdown(&ctx->queue);
    }
    LOG_THREAD_JOIN(ctx->async_thread);
  }

  queue_destroy(&ctx->queue);
  ring_queue_destroy(&ctx->ring_queue);

#if LOG_HAVE_SYSLOG
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
  free(ctx->handlers);

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
  mpool_destroy(&ctx->mpool);
  free(ctx);
}

log* log_default(void) {
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
  log *result = DEFAULT_LOG;
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

void log_set_level(log *ctx, int level) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->level = level;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_quiet(log *ctx, bool enable) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->quiet = enable;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_format(log *ctx, log_FormatFn fn) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->format_fn = fn;
  rwlock_write_unlock(&ctx->rwlock);
}

int log_set_async(log *ctx, bool enable) {
  if (!ctx) return -1;
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
    if (ctx->use_ring_queue) {
      ring_queue_shutdown(&ctx->ring_queue);
    } else {
      queue_shutdown(&ctx->queue);
    }
    LOG_THREAD_JOIN(ctx->async_thread);
    ctx->async_enabled = false;
  }
  return 0;
}

void log_set_max_file_size(log *ctx, size_t size) {
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

void log_set_file_prefix(log *ctx, const char *prefix) {
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

void log_enable_mpool(log *ctx, bool enable) {
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

#if LOG_FEATURE_TS_CACHE
void log_enable_ts_cache(log *ctx, bool enable) {
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

void log_set_queue_policy(log *ctx, int policy) {
  if (!ctx || policy < LOG_QUEUE_FALLBACK_SYNC || policy > LOG_QUEUE_BLOCK) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->queue_policy = policy;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_enable_ring_queue(log *ctx, bool enable) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->use_ring_queue = enable;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_clock_source(log *ctx, int clock_source) {
  if (!ctx || clock_source < LOG_CLOCK_REALTIME || clock_source > LOG_CLOCK_MONOTONIC_COARSE) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->clock_source = clock_source;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_queue_size(log *ctx, size_t size) {
  if (!ctx || size < 2) return;
  rwlock_write_lock(&ctx->rwlock);
  ring_queue_destroy(&ctx->ring_queue);
  ring_queue_init(&ctx->ring_queue, size);
  ctx->queue.max_size = size;
  rwlock_write_unlock(&ctx->rwlock);
}

/* Snapshot stats from thread-local storage into a plain struct */
static void stats_snapshot(log *ctx, log_stats *stats) {
  stats->total_count = tl_stats.total_count;
  for (int i = 0; i < LOG_LEVELS; i++) {
    stats->level_counts[i] = tl_stats.level_counts[i];
  }
  stats->queue_drops = tl_stats.queue_drops;
  stats->queue_blocked = tl_stats.queue_blocked;
  stats->rotation_count = tl_stats.rotation_count;
  stats->async_writes = tl_stats.async_writes;
  stats->sync_writes = tl_stats.sync_writes;
}

void log_get_perf_stats(log *ctx, log_stats *stats) {
  if (!ctx || !stats) return;
  stats_snapshot(ctx, stats);
}

int log_add_handler(log *ctx, log_LogFn fn, void *udata, int level) {
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

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}

int log_add_fp(log *ctx, FILE *fp, int level) {
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

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}

int log_add_file(log *ctx, const char *filename, int level) {
  if (!ctx || !filename) return -1;
  if (ctx->handler_count >= ctx->handler_capacity) return -1;

  FILE *fp = fopen(filename, "a");
  if (!fp) return -1;

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

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}

void log_remove_handler(log *ctx, int idx) {
  if (!ctx || idx < 0 || idx >= ctx->handler_count) return;
  
  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[idx].active = false;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_log(log *ctx, int level, const char *file, int line, const char *fmt, ...) {
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

  if (ctx->async_enabled) {
    bool pushed;
    if (ctx->use_ring_queue) {
      va_start(ev.ap, fmt);
      pushed = ring_queue_push_vfmt(&ctx->ring_queue, fmt, ev.ap, file, level, line,
                                    ev.timestamp, ctx->queue_policy == LOG_QUEUE_BLOCK);
      va_end(ev.ap);
      if (!pushed) {
        if (ctx->queue_policy == LOG_QUEUE_DROP) {
          STAT_INC(queue_drops);
        }
        rwlock_read_unlock(&ctx->rwlock);
        return;
      }
    } else {
      va_start(ev.ap, fmt);
      log_queue_entry *entry = queue_entry_create(ctx, &ev);
      va_end(ev.ap);
      pushed = entry && queue_push(ctx, entry, ctx->queue_policy == LOG_QUEUE_BLOCK);
      if (!pushed && entry) queue_entry_destroy(ctx, entry);
    }

    if (pushed) {
      STAT_INC(async_writes);
    } else {
      if (ctx->queue_policy == LOG_QUEUE_DROP) {
        STAT_INC(queue_drops);
      } else {
        /* FALLBACK_SYNC or BLOCK: write synchronously, no drop */
        va_start(ev.ap, fmt);
        char *pre_fmt_fb = NULL;
        va_list args_copy_fb;
        va_copy(args_copy_fb, ev.ap);
        int pflen_fb = vsnprintf(NULL, 0, fmt, args_copy_fb);
        va_end(args_copy_fb);
        if (pflen_fb >= 0) {
          pre_fmt_fb = malloc((size_t)pflen_fb + 1);
          if (pre_fmt_fb) {
            vsnprintf(pre_fmt_fb, (size_t)pflen_fb + 1, fmt, ev.ap);
            ev.raw_msg = pre_fmt_fb;
          }
        }
        for (int i = 0; i < ctx->handler_count; i++) {
          if (ctx->handlers[i].active && ctx->handlers[i].fn && level >= ctx->handlers[i].level) {
            ev.udata = ctx->handlers[i].udata;
            ctx->handlers[i].fn(ctx, &ev);
          }
        }
        free(pre_fmt_fb);
        va_end(ev.ap);
        STAT_INC(sync_writes);
      }
    }
  } else {
    STAT_INC(sync_writes);
    va_start(ev.ap, fmt);
    char *pre_fmt = NULL;
    va_list args_copy;
    va_copy(args_copy, ev.ap);
    int pflen = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (pflen >= 0) {
      pre_fmt = malloc((size_t)pflen + 1);
      if (pre_fmt) {
        vsnprintf(pre_fmt, (size_t)pflen + 1, fmt, ev.ap);
        ev.raw_msg = pre_fmt;
      }
    }
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].active && ctx->handlers[i].fn && level >= ctx->handlers[i].level) {
        ev.udata = ctx->handlers[i].udata;
        ctx->handlers[i].fn(ctx, &ev);
      }
    }
    free(pre_fmt);
    va_end(ev.ap);
  }

  rwlock_read_unlock(&ctx->rwlock);
}

void log_rotate(log *ctx) {
  if (!ctx || !ctx->file_prefix) return;

  rwlock_write_lock(&ctx->rwlock);

  /* On Windows, rename fails on open files; flush and close before rotating. */
  for (int i = 0; i < ctx->handler_count; i++) {
    if (!ctx->handlers[i].fp) continue;
    if (ctx->handlers[i].fp == stderr || ctx->handlers[i].fp == stdout) continue;
    fflush(ctx->handlers[i].fp);
    if (ctx->handlers[i].owns_file) {
      fclose(ctx->handlers[i].fp);
      ctx->handlers[i].fp = NULL;
    }
  }

  rotate_file(ctx, ctx->file_prefix);

  /* Reopen handlers at the new path */
  for (int i = 0; i < ctx->handler_count; i++) {
    if (!ctx->handlers[i].udata) continue;
    /* Skip stdout/stderr handlers (they have no real file) */
    if (ctx->handlers[i].fp == stdout || ctx->handlers[i].fp == stderr) continue;

    ctx->handlers[i].fp = fopen(ctx->file_prefix, "a");
    if (ctx->handlers[i].fp) {
      ctx->handlers[i].udata = ctx->handlers[i].fp;
      ctx->handlers[i].file_size = 0;
      /* Library opened the new file, so it now owns it */
      ctx->handlers[i].owns_file = true;
    }
  }
  rwlock_write_unlock(&ctx->rwlock);
}

int log_get_stats(log *ctx, log_stats *stats) {
  if (!ctx || !stats) return -1;
  stats_snapshot(ctx, stats);
  return 0;
}

#if LOG_FEATURE_JSON
int log_format_json(log *ctx, log_event *ev, char *buf, size_t buf_size) {
  char *msg = format_message(ev);
  if (!msg) return 0;

  char time_buf[64];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf),
                   ctx && ctx->enable_ts_cache);

  size_t msg_len = strlen(msg);
  size_t esc_len = 0;
  for (size_t i = 0; i < msg_len; i++) {
    switch (msg[i]) {
      case '"': case '\\': case '\n': case '\r': case '\t': esc_len += 2; break;
      default: esc_len += 1; break;
    }
  }

  char *escaped_msg = malloc(esc_len + 1);
  if (!escaped_msg) { free(msg); return 0; }
  size_t j = 0;
  for (size_t i = 0; i < msg_len; i++) {
    switch (msg[i]) {
      case '"':  escaped_msg[j++] = '\\'; escaped_msg[j++] = '"'; break;
      case '\\': escaped_msg[j++] = '\\'; escaped_msg[j++] = '\\'; break;
      case '\n': escaped_msg[j++] = '\\'; escaped_msg[j++] = 'n'; break;
      case '\r': escaped_msg[j++] = '\\'; escaped_msg[j++] = 'r'; break;
      case '\t': escaped_msg[j++] = '\\'; escaped_msg[j++] = 't'; break;
      default:   escaped_msg[j++] = msg[i]; break;
    }
  }
  escaped_msg[j] = '\0';
  free(msg);

  (void)ctx;
  int n = snprintf(buf, buf_size,
    "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
    time_buf, level_strings[ev->level],
    ev->file ? ev->file : "", ev->line, escaped_msg);
  free(escaped_msg);
  return n;
}
#endif /* LOG_FEATURE_JSON */

void log_handler_set_level(log *ctx, int handler_idx, int new_level) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return;
  
  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].level = new_level;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_handler_set_formatter(log *ctx, int handler_idx, log_FormatFn new_fn) {
  rwlock_write_lock(&ctx->rwlock);
  if (handler_idx >= 0 && handler_idx < ctx->handler_count) {
    ctx->handlers[handler_idx].fn = NULL; /* mark for kind-based resolution */
  }
  ctx->format_fn = new_fn;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_configure_pipeline(log* ctx, log_stage_function* stages, int stage_count) {
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

void log_enable_text_format(log* ctx) {
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
void log_enable_json_format(log* ctx) {
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
void log_enable_thread_id(log *ctx, int handler_idx, bool enable) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return;

  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].show_thread_id = enable;
  rwlock_write_unlock(&ctx->rwlock);
}
#endif /* LOG_FEATURE_THREAD_ID */

/* Syslog support implementation */
#if LOG_HAVE_SYSLOG && LOG_FEATURE_SYSLOG
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

static void syslog_handler(log *ctx, log_event *ev) {
  if (!ctx) return;

  int priority = LOG_USER | log_level_to_syslog(ev->level);

  char *msg = format_message(ev);
  if (!msg) return;

  if (ctx->handlers && ctx->handler_count > 0) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active) {
        char full_msg[5120];
        snprintf(full_msg, sizeof(full_msg), "[%lu] %s:%d: %s",
                LOG_GET_THREAD_ID(),
                ev->file ? ev->file : "", ev->line, msg);
        syslog(priority, "%s", full_msg);
        free(msg);
        return;
      }
    }
  }

  syslog(priority, "%s:%d: %s", ev->file ? ev->file : "", ev->line, msg);
  free(msg);
}

int log_add_syslog_handler(log *ctx, const char *ident, int facility, int level) {
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
#else
/* Stub implementations for Windows */
int log_level_to_syslog(int level) {
  (void)level;
  return 0;
}

int log_add_syslog_handler(log *ctx, const char *ident, int facility, int level) {
  (void)ctx; (void)ident; (void)facility; (void)level;
  return -1;
}
#endif

#if LOG_HAVE_SYSLOG
void log_handler_enable_syslog(log *ctx, int handler_idx, bool enable) {
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
#else
void log_handler_enable_syslog(log *ctx, int handler_idx, bool enable) {
  (void)ctx; (void)handler_idx; (void)enable;
}
#endif
