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

/* Get high-precision timestamp */
static double get_timestamp(void) {
#ifdef LOG_PLATFORM_POSIX
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
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

/* ==================== Atomic operation wrappers ==================== */

#ifdef LOG_USE_MSVC_ATOMIC

#define atomic_store(p, v) InterlockedExchange((volatile LONG*)(p), (LONG)(v))
#define atomic_load(p) ((volatile LONG)(*(p)))
#define atomic_fetch_add(p, v) InterlockedExchangeAdd((volatile LONG*)(p), (LONG)(v))
#define atomic_fetch_sub(p, v) InterlockedExchangeAdd((volatile LONG*)(p), -(LONG)(v))

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

/* Atomic stats helpers (field must be a member of ctx->stats) */
#ifdef LOG_USE_STDATOMIC
  #define STAT_INC(f)      atomic_fetch_add(&ctx->stats.f, 1)
  #define STAT_LOAD(f)     atomic_load(&ctx->stats.f)
  #define STAT_STORE(f, v) atomic_store(&ctx->stats.f, (v))
#else
  #define STAT_INC(f)      InterlockedExchangeAdd64((volatile LONG64*)&ctx->stats.f, 1)
  #define STAT_LOAD(f)     (ctx->stats.f)
  #define STAT_STORE(f, v) (ctx->stats.f = (v))
#endif

static log *DEFAULT_LOG = NULL;

static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[91m"
};
#endif

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

static LOG_THREAD_LOCAL log_ts_cache ts_cache_local;

/* Format a high-precision timestamp. When use_cache is set, the
 * "%Y-%m-%dT%H:%M:%S" part is cached per thread and reused while the
 * whole-second value does not change (avoids localtime + strftime). */
static void format_timestamp(double ts, char *buf, size_t size, bool use_cache) {
  time_t t = (time_t)ts;
  log_ts_cache *cache = &ts_cache_local;
  struct tm tm_buf;
  if (use_cache) {
    if (cache->cached_string[0] != '\0' && cache->last_timestamp == (double)t) {
      cache->cache_hits++;
      snprintf(buf, size, "%s.%03d", cache->cached_string, (int)((ts - (double)t) * 1000));
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
      snprintf(buf, size, "%s.%03d", cache->cached_string, (int)((ts - (double)t) * 1000));
      return;
    }
    cache->cached_string[0] = '\0';
    buf[0] = '\0';
    return;
  }
#if defined(LOG_PLATFORM_POSIX)
  struct tm *tm_info = localtime_r(&t, &tm_buf);
#else
  struct tm *tm_info = localtime_s(&tm_buf, &t) == 0 ? &tm_buf : NULL;
#endif
  if (tm_info) {
    strftime(buf, size, "%Y-%m-%dT%H:%M:%S", tm_info);
    int ms = (int)((ts - (double)t) * 1000);
    snprintf(buf + strlen(buf), size - strlen(buf), ".%03d", ms);
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
    /* Grow pooled msg buffer only if needed; keep old pointer on failure (Bug 2) */
    if ((size_t)len >= 512) {
      char *new_msg = realloc(entry->msg, (size_t)len + 1);
      if (!new_msg) goto fail;
      entry->msg = new_msg;
    }
    vsnprintf(entry->msg, (size_t)len + 1, ev->fmt, ev->ap);

    size_t file_len = ev->file ? strlen(ev->file) : 0;
    if (file_len >= 128) {
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
  char time_buf[32];
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
  fflush(ev->udata);
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

  fflush(ev->udata);
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
    fflush(ev->udata);
    free(msg);
  }
}

static void json_handler(log *ctx, log_event *ev) {
  char *msg = format_message(ev);
  if (!msg) return;

  char time_buf[32];
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
  fflush(ev->udata);
  free(escaped_msg);
  free(buf);
}

/* Async writer thread */
#if defined(LOG_PLATFORM_POSIX)
static void* async_writer_thread(void *arg) {
#else
static DWORD WINAPI async_writer_thread(LPVOID arg) {
#endif
  log *ctx = (log*)arg;

  while (true) {
    log_queue_entry *entry = queue_pop(&ctx->queue);
    if (!entry) {
      /* Queue was shut down and drained */
      break;
    }

    double queue_latency = (get_timestamp() - entry->timestamp) * 1000.0;

    log_event ev = {0};
    ev.level = entry->level;
    ev.file = entry->file;
    ev.line = entry->line;
    ev.timestamp = entry->timestamp;
    ev.raw_msg = entry->msg;

    uint64_t tc = STAT_LOAD(total_count);
    uint64_t total_latency_ops = tc > 0 ?
      (uint64_t)(STAT_LOAD(avg_queue_latency_ms) * (double)tc) : 0;
    tc = STAT_INC(total_count) + 1;
    if (tc > 0) {
      STAT_STORE(avg_queue_latency_ms,
        (double)(total_latency_ops + (uint64_t)(queue_latency * 1000)) / (double)tc);
    }
    STAT_INC(async_writes);

    rwlock_read_lock(&ctx->rwlock);
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].active && ctx->handlers[i].fn && entry->level >= ctx->handlers[i].level) {
        ev.udata = ctx->handlers[i].udata;
        ctx->handlers[i].fn(ctx, &ev);
      }
    }
    rwlock_read_unlock(&ctx->rwlock);

    queue_entry_destroy(ctx, entry);
  }

#if defined(LOG_PLATFORM_POSIX)
  return NULL;
#else
  return 0;
#endif
}

/* API Implementation */
log* log_create(void) {
  log *ctx = calloc(1, sizeof(log));
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
  ctx->enable_mpool = false;

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
    queue_shutdown(&ctx->queue);
    LOG_THREAD_JOIN(ctx->async_thread);
  }

  queue_destroy(&ctx->queue);

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
  if (!DEFAULT_LOG) {
    DEFAULT_LOG = log_create();
  }
  return DEFAULT_LOG;
}

const char* log_level_string(int level) {
  if (level < 0 || level >= LOG_LEVELS) {
    return "UNKNOWN";
  }
  return level_strings[level];
}

void log_set_level(log *ctx, int level) {
  rwlock_write_lock(&ctx->rwlock);
  ctx->level = level;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_quiet(log *ctx, bool enable) {
  rwlock_write_lock(&ctx->rwlock);
  ctx->quiet = enable;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_set_format(log *ctx, log_FormatFn fn) {
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
    queue_shutdown(&ctx->queue);
    LOG_THREAD_JOIN(ctx->async_thread);
    ctx->async_enabled = false;
  }
  return 0;
}

void log_set_max_file_size(log *ctx, size_t size) {
  rwlock_write_lock(&ctx->rwlock);
  ctx->max_file_size = size;
  rwlock_write_unlock(&ctx->rwlock);
}

/**
 * Check if the path contains path traversal sequences or is an absolute path.
 * Returns 1 if the path is safe (no traversal), 0 if unsafe.
 */
static int is_path_safe(const char *path) {
  if (!path || path[0] == '\0') {
    return 0;
  }

  /* Reject absolute paths:
   * - Unix: starts with '/'
   * - Windows: starts with 'X:\' or 'X:/' or '\\' (UNC)
   */
  if (path[0] == '/' || path[0] == '\\') {
    return 0;
  }
#ifdef LOG_PLATFORM_WINDOWS
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

void log_set_queue_policy(log *ctx, int policy) {
  if (!ctx || policy < LOG_QUEUE_FALLBACK_SYNC || policy > LOG_QUEUE_BLOCK) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->queue_policy = policy;
  rwlock_write_unlock(&ctx->rwlock);
}

/* Snapshot the atomic stats into a plain struct (thread-safe) */
static void stats_snapshot(log *ctx, log_stats *stats) {
#ifdef LOG_USE_STDATOMIC
  stats->total_count = STAT_LOAD(total_count);
  for (int i = 0; i < LOG_LEVELS; i++) {
    stats->level_counts[i] = STAT_LOAD(level_counts[i]);
  }
  stats->queue_drops = STAT_LOAD(queue_drops);
  stats->queue_blocked = STAT_LOAD(queue_blocked);
  stats->rotation_count = STAT_LOAD(rotation_count);
  stats->avg_queue_latency_ms = STAT_LOAD(avg_queue_latency_ms);
  stats->async_writes = STAT_LOAD(async_writes);
  stats->sync_writes = STAT_LOAD(sync_writes);
#else
  (void)ctx;
  *stats = ctx->stats;
#endif
}

void log_get_perf_stats(log *ctx, log_stats *stats) {
  if (!ctx || !stats) return;
  rwlock_read_lock(&ctx->rwlock);
  stats_snapshot(ctx, stats);
  rwlock_read_unlock(&ctx->rwlock);
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
  if (!ctx) return -1;
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
  if (!ctx) return;
  
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
  ev.timestamp = get_timestamp();

  if (ctx->async_enabled) {
    va_start(ev.ap, fmt);
    log_queue_entry *entry = queue_entry_create(ctx, &ev);
    va_end(ev.ap);

    bool pushed = entry &&
      queue_push(ctx, entry, ctx->queue_policy == LOG_QUEUE_BLOCK);
    if (pushed) {
      STAT_INC(async_writes);
    } else {
      STAT_INC(queue_drops);
      if (entry) {
        queue_entry_destroy(ctx, entry);
      }
      if (ctx->queue_policy != LOG_QUEUE_DROP) {
        /* FALLBACK_SYNC: queue full -> write synchronously (zero loss).
         * BLOCK: queue closed while waiting -> degrade to synchronous write. */
        va_start(ev.ap, fmt);
        for (int i = 0; i < ctx->handler_count; i++) {
          if (ctx->handlers[i].active && ctx->handlers[i].fn && level >= ctx->handlers[i].level) {
            ev.udata = ctx->handlers[i].udata;
            ctx->handlers[i].fn(ctx, &ev);
          }
        }
        va_end(ev.ap);
        STAT_INC(sync_writes);
      }
    }
  } else {
    STAT_INC(sync_writes);
    va_start(ev.ap, fmt);
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].active && ctx->handlers[i].fn && level >= ctx->handlers[i].level) {
        ev.udata = ctx->handlers[i].udata;
        ctx->handlers[i].fn(ctx, &ev);
      }
    }
    va_end(ev.ap);
  }
  
  rwlock_read_unlock(&ctx->rwlock);
}

void log_rotate(log *ctx) {
  if (!ctx || !ctx->file_prefix) return;
  
  rwlock_write_lock(&ctx->rwlock);
  rotate_file(ctx, ctx->file_prefix);
  rwlock_write_unlock(&ctx->rwlock);
}

int log_get_stats(log *ctx, log_stats *stats) {
  if (!ctx || !stats) return -1;
  
  rwlock_read_lock(&ctx->rwlock);
  stats_snapshot(ctx, stats);
  rwlock_read_unlock(&ctx->rwlock);
  
  return 0;
}

int log_format_json(log *ctx, log_event *ev, char *buf, size_t buf_size) {
  char *msg = format_message(ev);
  if (!msg) return 0;

  char time_buf[32];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf),
                   ctx && ctx->enable_ts_cache);

  char escaped_msg[8192];
  size_t j = 0;
  for (size_t i = 0; msg[i] && j < sizeof(escaped_msg) - 1; i++) {
    switch (msg[i]) {
      case '"':  if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = '"'; } break;
      case '\\': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = '\\'; } break;
      case '\n': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 'n'; } break;
      case '\r': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 'r'; } break;
      case '\t': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 't'; } break;
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
  return n;
}

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

/* Thread ID support implementation */
void log_enable_thread_id(log *ctx, int handler_idx, bool enable) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return;

  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].show_thread_id = enable;
  rwlock_write_unlock(&ctx->rwlock);
}

/* Syslog support implementation */
#if LOG_HAVE_SYSLOG
int log_level_to_syslog(int level) {
  switch (level) {
    case LOG_TRACE: return LOG_DEBUG;
    case LOG_DEBUG: return LOG_DEBUG;
    case LOG_INFO:  return LOG_INFO;
    case LOG_WARN:  return LOG_SYSLOG_WARNING;
    case LOG_ERROR: return LOG_SYSLOG_ERR;
    case LOG_FATAL: return LOG_SYSLOG_CRIT;
    default:        return LOG_INFO;
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
