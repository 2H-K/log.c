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

#ifdef LOG_PLATFORM_POSIX
#define _GNU_SOURCE
#endif
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
#else
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  ULARGE_INTEGER uli;
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  /* Convert from 100ns intervals since 1601 to seconds since 1970 */
  return (double)(uli.QuadPart - 116444736000000000LL) / 10000000.0;
#endif
}

/* Sleep for specified milliseconds */
static void log_sleep_ms(int ms) {
#ifdef LOG_PLATFORM_POSIX
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
#else
  Sleep(ms);
#endif
}

/* CPU pause instruction for spinlocks */
static void log_cpu_pause(void) {
#ifdef __GNUC__
  __asm__ __volatile__("pause" ::: "memory");
#elif defined(_MSC_VER)
  _mm_pause();
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
#define DEFAULT_FORMAT LOG_FORMAT_TEXT

#define LOG_MPOOL_CHUNK_SIZE 64
#define LOG_MPOOL_MAX_CHUNKS 64

static log *DEFAULT_LOG = NULL;

static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[91m"
};
#endif

static LOG_THREAD_LOCAL log_thread_buffer thread_buf = {0};

static void mpool_init(log_mpool *mp, size_t max_size) {
  mp->free_list = NULL;
  mp->allocated = 0;
  mp->max_size = max_size;
  mp->chunk_count = 0;
}

static void mpool_destroy(log_mpool *mp) {
  while (mp->free_list) {
    log_queue_entry *next = mp->free_list->next;
    free(mp->free_list->message);
    free(mp->free_list->file);
    free(mp->free_list);
    mp->free_list = next;
  }
  mp->allocated = 0;
}

static log_queue_entry* mpool_alloc(log_mpool *mp) {
  if (mp->free_list) {
    log_queue_entry *entry = mp->free_list;
    mp->free_list = entry->next;
    entry->next = NULL;
    return entry;
  }
  if (mp->allocated >= mp->max_size) {
    return NULL;
  }
  log_queue_entry *entry = calloc(1, sizeof(log_queue_entry));
  if (entry) {
    entry->message = malloc(512);
    entry->file = malloc(128);
    if (!entry->message || !entry->file) {
      free(entry->message);
      free(entry->file);
      free(entry);
      return NULL;
    }
    entry->message[0] = '\0';
    entry->file[0] = '\0';
    mp->allocated++;
  }
  return entry;
}

static void mpool_free(log_mpool *mp, log_queue_entry *entry) {
  if (!entry) return;
  entry->next = mp->free_list;
  mp->free_list = entry;
}

static void ts_cache_init(log_ts_cache *cache) {
  cache->last_timestamp = 0.0;
  cache->cached_string[0] = '\0';
  cache->cache_hits = 0;
  cache->cache_misses = 0;
}

static void format_timestamp(double ts, char *buf, size_t size) {
  time_t t = (time_t)ts;
  struct tm *tm_info = localtime(&t);
  if (tm_info) {
    strftime(buf, size, "%Y-%m-%dT%H:%M:%S", tm_info);
    int ms = (int)((ts - (double)t) * 1000);
    snprintf(buf + strlen(buf), size - strlen(buf), ".%03d", ms);
  } else {
    buf[0] = '\0';
  }
}

static size_t format_timestamp_cached(log *ctx, double ts, char *buf, size_t size) {
  if (!ctx->enable_ts_cache) {
    format_timestamp(ts, buf, size);
    return strlen(buf);
  }
  log_ts_cache *cache = &ctx->ts_cache;
  if (ts - cache->last_timestamp < 0.001) {
    cache->cache_hits++;
    strncpy(buf, cache->cached_string, size - 1);
    buf[size - 1] = '\0';
    return strlen(buf);
  }
  cache->cache_misses++;
  format_timestamp(ts, buf, size);
  cache->last_timestamp = ts;
  strncpy(cache->cached_string, buf, sizeof(cache->cached_string) - 1);
  cache->cached_string[sizeof(cache->cached_string) - 1] = '\0';
  return strlen(buf);
}

/* Reader-Writer Lock Implementation */
static void rwlock_init(log_rwlock *lock) {
#ifdef LOG_USE_STDATOMIC
  atomic_store(&lock->readers, 0);
  atomic_store(&lock->writer, 0);
  atomic_store(&lock->write_waiting, false);
#else
  lock->readers = 0;
  lock->writer = 0;
  lock->write_waiting = false;
#endif
}

static void rwlock_read_lock(log_rwlock *lock) {
  int backoff = 1;
#ifdef LOG_USE_STDATOMIC
  while (atomic_load(&lock->writer) || atomic_load(&lock->write_waiting)) {
#else
  while (lock->writer || lock->write_waiting) {
#endif
    for (int i = 0; i < backoff; i++) {
      log_cpu_pause();
    }
    if (backoff < 16) backoff *= 2;
  }
#ifdef LOG_USE_STDATOMIC
  atomic_fetch_add(&lock->readers, 1);
#else
  InterlockedIncrement((volatile LONG*)&lock->readers);
#endif
}

static void rwlock_read_unlock(log_rwlock *lock) {
#ifdef LOG_USE_STDATOMIC
  atomic_fetch_sub(&lock->readers, 1);
#else
  InterlockedDecrement((volatile LONG*)&lock->readers);
#endif
}

static void rwlock_write_lock(log_rwlock *lock) {
#ifdef LOG_USE_STDATOMIC
  atomic_store(&lock->write_waiting, true);
#else
  lock->write_waiting = true;
#endif
  int backoff = 1;
#ifdef LOG_USE_STDATOMIC
  while (atomic_load(&lock->readers) > 0 || atomic_load(&lock->writer) > 0) {
#else
  while (lock->readers > 0 || lock->writer > 0) {
#endif
    for (int i = 0; i < backoff; i++) {
      log_cpu_pause();
    }
    if (backoff < 16) backoff *= 2;
  }
#ifdef LOG_USE_STDATOMIC
  atomic_store(&lock->write_waiting, false);
  atomic_store(&lock->writer, 1);
#else
  lock->write_waiting = false;
  lock->writer = 1;
#endif
}

static void rwlock_write_unlock(log_rwlock *lock) {
#ifdef LOG_USE_STDATOMIC
  atomic_store(&lock->writer, 0);
#else
  lock->writer = 0;
#endif
}

/* Lock-free Queue Implementation */
static log_queue_entry* queue_entry_create(log *ctx, log_Event *ev) {
  log_mpool *mp = &ctx->mpool;
  log_queue_entry *entry = ctx->enable_mpool ? mpool_alloc(mp) : malloc(sizeof(log_queue_entry));

  if (!entry) return NULL;

  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int len = vsnprintf(NULL, 0, ev->fmt, args_copy);
  va_end(args_copy);

  if (len < 0) {
    if (!ctx->enable_mpool) free(entry);
    return NULL;
  }

  if (ctx->enable_mpool) {
    if ((size_t)len >= 512) {
      entry->message = realloc(entry->message, len + 1);
    }
    if ((size_t)len >= 128) {
      entry->file = realloc(entry->file, len + 1);
    }
  } else {
    entry->message = malloc(len + 1);
    entry->file = strdup(ev->file ? ev->file : "");
  }

  if (!entry->message || (!entry->file && !ctx->enable_mpool)) {
    free(entry->message);
    if (!ctx->enable_mpool) free(entry->file);
    if (!ctx->enable_mpool) free(entry);
    return NULL;
  }

  vsnprintf(entry->message, len + 1, ev->fmt, ev->ap);

  entry->level = ev->level;
  if (ctx->enable_mpool) {
    strncpy(entry->file, ev->file ? ev->file : "", 127);
    entry->file[127] = '\0';
  }
  entry->line = ev->line;
  entry->timestamp = ev->timestamp;
  entry->next = NULL;

  return entry;
}

static void queue_entry_destroy(log *ctx, log_queue_entry *entry) {
  if (!entry) return;
  if (ctx->enable_mpool) {
    mpool_free(&ctx->mpool, entry);
  } else {
    free(entry->message);
    free(entry->file);
    free(entry);
  }
}

static void queue_init(log_queue *q, size_t max_size) {
  log_queue_entry *dummy = calloc(1, sizeof(log_queue_entry));
#ifdef LOG_USE_STDATOMIC
  atomic_store(&q->head, dummy);
  atomic_store(&q->tail, dummy);
  atomic_store(&q->size, 0);
  atomic_store(&q->high_water_mark, 0);
#else
  q->head = dummy;
  q->tail = dummy;
  q->size = 0;
  q->high_water_mark = 0;
#endif
  q->max_size = max_size;
}

static bool queue_push(log_queue *q, log_queue_entry *entry) {
#ifdef LOG_USE_STDATOMIC
  size_t current_size = atomic_load(&q->size);
#else
  size_t current_size = q->size;
#endif
  if (current_size >= q->max_size) {
    return false;
  }
  
  entry->next = NULL;
#ifdef LOG_USE_STDATOMIC
  log_queue_entry *old_tail = atomic_exchange(&q->tail, entry);
  atomic_store(&old_tail->next, entry);
  atomic_fetch_add(&q->size, 1);
#else
  log_queue_entry *old_tail = (log_queue_entry*)InterlockedExchangePointer((volatile PVOID*)&q->tail, entry);
  old_tail->next = entry;
  InterlockedIncrement((volatile LONG*)&q->size);
#endif
  
#ifdef LOG_USE_STDATOMIC
  size_t new_size = atomic_load(&q->size);
  size_t hwm = atomic_load(&q->high_water_mark);
  while (new_size > hwm) {
    if (atomic_compare_exchange_weak(&q->high_water_mark, &hwm, new_size)) {
      break;
    }
  }
#else
  size_t new_size = q->size;
  size_t hwm = q->high_water_mark;
  while (new_size > hwm) {
    if (InterlockedCompareExchange((volatile LONG*)&q->high_water_mark, (LONG)new_size, (LONG)hwm) == (LONG)hwm) {
      break;
    }
    hwm = q->high_water_mark;
  }
#endif
  
  return true;
}

static log_queue_entry* queue_pop(log_queue *q) {
#ifdef LOG_USE_STDATOMIC
  log_queue_entry *head = atomic_load(&q->head);
  log_queue_entry *next = atomic_load(&head->next);
#else
  log_queue_entry *head = q->head;
  log_queue_entry *next = head->next;
#endif
  
  if (next == NULL) {
    return NULL;
  }
  
#ifdef LOG_USE_STDATOMIC
  if (atomic_compare_exchange_strong(&q->head, &head, next)) {
    atomic_fetch_sub(&q->size, 1);
    return head;
  }
#else
  if (InterlockedCompareExchangePointer((volatile PVOID*)&q->head, next, head) == head) {
    InterlockedDecrement((volatile LONG*)&q->size);
    return head;
  }
#endif
  
  return NULL;
}

static void queue_destroy(log_queue *q) {
  (void)q;
}

/* Default format functions */
static int format_text(log *ctx, log_Event *ev, char *buf, size_t buf_size) {
  (void)ctx;
  char time_buf[32];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf));

  int written = 0;

  if (ctx && buf_size > 0) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active) {
        written = snprintf(buf, buf_size, "%s %-5s [%lu] %s:%d: ",
                        time_buf, level_strings[ev->level],
                        LOG_GET_THREAD_ID(),
                        ev->file ? ev->file : "", ev->line);
        break;
      }
    }
  }

  if (written == 0) {
    written = snprintf(buf, buf_size, "%s %-5s %s:%d: ",
                       time_buf, level_strings[ev->level],
                       ev->file ? ev->file : "", ev->line);
  }

  if (written < 0 || (size_t)written >= buf_size) {
    buf[buf_size - 1] = '\0';
    return buf_size - 1;
  }

  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int msg_len = vsnprintf(buf + written, buf_size - written, ev->fmt, args_copy);
  va_end(args_copy);

  return written + msg_len;
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
  
  ctx->stats.rotation_count++;
}

/* Output handlers */
static void stdout_handler(log *ctx, log_Event *ev) {
  (void)ctx;
  char time_buf[32];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf));

  if (ctx) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active && ctx->handlers[i].udata == ev->udata) {
#ifdef LOG_USE_COLOR
        fprintf(ev->udata, "%s %s%-5s\x1b[0m \x1b[90m[%lu] %s:%d:\x1b[0m ",
                time_buf, level_colors[ev->level], level_strings[ev->level],
                LOG_GET_THREAD_ID(), ev->file ? ev->file : "", ev->line);
#else
        fprintf(ev->udata, "%s %-5s [%lu] %s:%d: ",
                time_buf, level_strings[ev->level],
                LOG_GET_THREAD_ID(), ev->file ? ev->file : "", ev->line);
#endif
        vfprintf(ev->udata, ev->fmt, ev->ap);
        fprintf(ev->udata, "\n");
        fflush(ev->udata);
        return;
      }
    }
  }

#ifdef LOG_USE_COLOR
  fprintf(ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
          time_buf, level_colors[ev->level], level_strings[ev->level],
          ev->file ? ev->file : "", ev->line);
#else
  fprintf(ev->udata, "%s %-5s %s:%d: ",
          time_buf, level_strings[ev->level],
          ev->file ? ev->file : "", ev->line);
#endif

  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}

static void file_handler_internal(log *ctx, log_Event *ev, int handler_idx) {
  char buf[4096];
  char time_buf[32];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf));
  
  int len = snprintf(buf, sizeof(buf), "%s %-5s %s:%d: ",
                     time_buf, level_strings[ev->level],
                     ev->file ? ev->file : "", ev->line);
  
  if (len < 0) return;
  
  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int msg_len = vsnprintf(buf + len, sizeof(buf) - len, ev->fmt, args_copy);
  va_end(args_copy);
  
  if (msg_len > 0) {
    buf[len + msg_len] = '\n';
    buf[len + msg_len + 1] = '\0';
    
    FILE *fp = ctx->handlers[handler_idx].fp;
    size_t written = fwrite(buf, 1, len + msg_len + 1, fp);
    ctx->handlers[handler_idx].file_size += written;
    
    if (ctx->handlers[handler_idx].file_size >= ctx->max_file_size) {
      fflush(fp);
      if (fp != stderr && fp != stdout) {
        fclose(fp);
      }
      rotate_file(ctx, ctx->file_prefix);
      
      ctx->handlers[handler_idx].fp = fopen(ctx->file_prefix, "a");
      if (ctx->handlers[handler_idx].fp) {
        ctx->handlers[handler_idx].file_size = 0;
      }
    }
  }
  
  fflush(ev->udata);
}

static void file_handler_wrapper(log *ctx, log_Event *ev) {
  FILE *target_fp = ev->udata;
  
  for (int i = 0; i < ctx->handler_count; i++) {
    if (ctx->handlers[i].udata == target_fp && ctx->handlers[i].fp) {
      file_handler_internal(ctx, ev, i);
      return;
    }
  }
  
  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}

static void json_handler(log *ctx, log_Event *ev) {
  char buf[8192];
  char time_buf[32];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf));

  va_list args_copy;
  va_copy(args_copy, ev->ap);
  char msg_buf[4096];
  vsnprintf(msg_buf, sizeof(msg_buf), ev->fmt, args_copy);
  va_end(args_copy);

  char escaped_msg[8192];
  size_t j = 0;
  for (size_t i = 0; msg_buf[i] && j < sizeof(escaped_msg) - 1; i++) {
    switch (msg_buf[i]) {
      case '"':  if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = '"'; } break;
      case '\\': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = '\\'; } break;
      case '\n': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 'n'; } break;
      case '\r': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 'r'; } break;
      case '\t': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 't'; } break;
      default:   escaped_msg[j++] = msg_buf[i]; break;
    }
  }
  escaped_msg[j] = '\0';

  bool show_tid = false;
  if (ctx) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active && ctx->handlers[i].udata == ev->udata) {
        show_tid = true;
        break;
      }
    }
  }

  if (show_tid) {
    snprintf(buf, sizeof(buf),
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"thread_id\": %lu, \"message\": \"%s\"}",
      time_buf, level_strings[ev->level],
      ev->file ? ev->file : "", ev->line, LOG_GET_THREAD_ID(), escaped_msg);
  } else {
    snprintf(buf, sizeof(buf),
      "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
      time_buf, level_strings[ev->level],
      ev->file ? ev->file : "", ev->line, escaped_msg);
  }

  fprintf(ev->udata, "%s\n", buf);
  fflush(ev->udata);
}

/* Async writer thread */
#if LOG_PLATFORM_POSIX
static void* async_writer_thread(void *arg) {
#else
static DWORD WINAPI async_writer_thread(LPVOID arg) {
#endif
  log *ctx = (log*)arg;
  
#ifdef LOG_USE_STDATOMIC
  while (atomic_load(&ctx->async_running)) {
#else
  while (ctx->async_running) {
#endif
    log_queue_entry *entry = queue_pop(&ctx->queue);
    if (!entry) {
      log_sleep_ms(100);
      continue;
    }
    
    double queue_latency = (get_timestamp() - entry->timestamp) * 1000.0;
    
    log_Event ev = {0};
    ev.level = entry->level;
    ev.file = entry->file;
    ev.line = entry->line;
    ev.timestamp = entry->timestamp;
    ev.fmt = entry->message;
    
    uint64_t total_latency_ops = ctx->stats.total_count > 0 ? 
      (uint64_t)(ctx->stats.avg_queue_latency_ms * ctx->stats.total_count) : 0;
    ctx->stats.total_count++;
    if (ctx->stats.total_count > 0) {
      ctx->stats.avg_queue_latency_ms = 
        (double)(total_latency_ops + (uint64_t)(queue_latency * 1000)) / ctx->stats.total_count;
    }
    ctx->stats.async_writes++;
    
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

#if LOG_PLATFORM_POSIX
  return NULL;
#else
  return 0;
#endif
}

/* API Implementation */
log* log_create(void) {
  log *ctx = calloc(1, sizeof(log));
  if (!ctx) return NULL;

  rwlock_init(&ctx->rwlock);
#if LOG_PLATFORM_POSIX
  pthread_mutex_init(&ctx->mutex, NULL);
#else
  InitializeCriticalSection(&ctx->mutex);
#endif

  ctx->level = LOG_TRACE;
  ctx->quiet = false;
  ctx->format_mode = DEFAULT_FORMAT;
  ctx->max_file_size = LOG_DEFAULT_MAX_SIZE;
  ctx->async_enabled = false;
#ifdef LOG_USE_STDATOMIC
  atomic_store(&ctx->async_running, false);
#else
  ctx->async_running = false;
#endif

  queue_init(&ctx->queue, DEFAULT_QUEUE_SIZE);

  ctx->handler_capacity = MAX_HANDLERS;
  ctx->handlers = calloc(MAX_HANDLERS, sizeof(log_handler));
  ctx->handler_count = 0;

  ctx->format_fn = format_text;

  memset(&ctx->stats, 0, sizeof(ctx->stats));

  ctx->syslog_ident = NULL;
  ctx->syslog_facility = LOG_USER;
  ctx->syslog_enabled_global = false;

  mpool_init(&ctx->mpool, LOG_MPOOL_MAX_CHUNKS * LOG_MPOOL_CHUNK_SIZE);
  ts_cache_init(&ctx->ts_cache);
  ctx->enable_ts_cache = true;
  ctx->enable_mpool = false;

  log_add_handler(ctx, stdout_handler, stderr, LOG_TRACE);

  return ctx;
}

void log_destroy(log *ctx) {
  if (!ctx) return;

  if (ctx->async_enabled) {
#ifdef LOG_USE_STDATOMIC
    atomic_store(&ctx->async_running, false);
#else
    ctx->async_running = false;
#endif
    LOG_THREAD_JOIN(ctx->async_thread);
  }

  queue_destroy(&ctx->queue);

#if LOG_HAVE_SYSLOG
  if (ctx->syslog_enabled_global) {
    closelog();
  }
#endif

  for (int i = 0; i < ctx->handler_count; i++) {
    free(ctx->handlers[i].filename);
  }
  free(ctx->handlers);

  free(ctx->file_prefix);
  free(ctx->syslog_ident);
#if LOG_PLATFORM_POSIX
  pthread_mutex_destroy(&ctx->mutex);
#else
  DeleteCriticalSection(&ctx->mutex);
#endif

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
  ctx->format_fn = fn ? fn : format_text;
  rwlock_write_unlock(&ctx->rwlock);
}

int log_set_async(log *ctx, bool enable) {
  if (enable && !ctx->async_enabled) {
#ifdef LOG_USE_STDATOMIC
    atomic_store(&ctx->async_running, true);
#else
    ctx->async_running = true;
#endif
#if LOG_PLATFORM_POSIX
    if (LOG_THREAD_CREATE(ctx->async_thread, async_writer_thread, ctx) != 0) {
      atomic_store(&ctx->async_running, false);
      return -1;
    }
#else
    ctx->async_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)async_writer_thread, ctx, 0, NULL);
    if (ctx->async_thread == NULL) {
      ctx->async_running = false;
      return -1;
    }
#endif
    ctx->async_enabled = true;
  } else if (!enable && ctx->async_enabled) {
#ifdef LOG_USE_STDATOMIC
    atomic_store(&ctx->async_running, false);
#else
    ctx->async_running = false;
#endif
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

void log_set_file_prefix(log *ctx, const char *prefix) {
  rwlock_write_lock(&ctx->rwlock);
  free(ctx->file_prefix);
  ctx->file_prefix = strdup(prefix ? prefix : "log");
  rwlock_write_unlock(&ctx->rwlock);
}

void log_enable_mpool(log *ctx, bool enable) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  if (!enable && ctx->enable_mpool) {
    mpool_destroy(&ctx->mpool);
    mpool_init(&ctx->mpool, LOG_MPOOL_MAX_CHUNKS * LOG_MPOOL_CHUNK_SIZE);
  }
  ctx->enable_mpool = enable;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_enable_ts_cache(log *ctx, bool enable) {
  if (!ctx) return;
  rwlock_write_lock(&ctx->rwlock);
  ctx->enable_ts_cache = enable;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_get_perf_stats(log *ctx, log_stats *stats) {
  if (!ctx || !stats) return;
  rwlock_read_lock(&ctx->rwlock);
  *stats = ctx->stats;
  stats->queue_drops = ctx->mpool.allocated;
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

  rwlock_write_unlock(&ctx->rwlock);
  return ctx->handler_count - 1;
}

int log_add_fp(log *ctx, FILE *fp, int level) {
  if (!ctx) return -1;

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
  
  ctx->stats.total_count++;
  if (level >= 0 && level < LOG_LEVELS) {
    ctx->stats.level_counts[level]++;
  }
  
  log_Event ev = {0};
  ev.fmt = fmt;
  ev.file = file;
  ev.line = line;
  ev.level = level;
  ev.timestamp = get_timestamp();

  if (ctx->async_enabled) {
    va_start(ev.ap, fmt);
    log_queue_entry *entry = queue_entry_create(ctx, &ev);
    va_end(ev.ap);

    if (entry && queue_push(&ctx->queue, entry)) {
      ctx->stats.async_writes++;
    } else {
      ctx->stats.queue_drops++;
      if (entry) {
        queue_entry_destroy(ctx, entry);
      }
      va_start(ev.ap, fmt);
      for (int i = 0; i < ctx->handler_count; i++) {
        if (ctx->handlers[i].active && ctx->handlers[i].fn && level >= ctx->handlers[i].level) {
          ev.udata = ctx->handlers[i].udata;
          ctx->handlers[i].fn(ctx, &ev);
        }
      }
      va_end(ev.ap);
      ctx->stats.sync_writes++;
    }
  } else {
    ctx->stats.sync_writes++;
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
  *stats = ctx->stats;
  rwlock_read_unlock(&ctx->rwlock);
  
  return 0;
}

const char* log_format_json(log *ctx, log_Event *ev, char *buf, size_t buf_size) {
  char time_buf[32];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf));
  
  va_list args_copy;
  va_copy(args_copy, ev->ap);
  char msg_buf[4096];
  vsnprintf(msg_buf, sizeof(msg_buf), ev->fmt, args_copy);
  va_end(args_copy);
  
  char escaped_msg[8192];
  size_t j = 0;
  for (size_t i = 0; msg_buf[i] && j < sizeof(escaped_msg) - 1; i++) {
    switch (msg_buf[i]) {
      case '"':  if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = '"'; } break;
      case '\\': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = '\\'; } break;
      case '\n': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 'n'; } break;
      case '\r': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 'r'; } break;
      case '\t': if (j < sizeof(escaped_msg) - 2) { escaped_msg[j++] = '\\'; escaped_msg[j++] = 't'; } break;
      default:   escaped_msg[j++] = msg_buf[i]; break;
    }
  }
  escaped_msg[j] = '\0';
  
  (void)ctx;
  snprintf(buf, buf_size,
    "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
    time_buf, level_strings[ev->level],
    ev->file ? ev->file : "", ev->line, escaped_msg);
  return buf;
}

void log_handler_set_level(log *ctx, int handler_idx, int new_level) {
  if (!ctx || handler_idx < 0 || handler_idx >= ctx->handler_count) return;
  
  rwlock_write_lock(&ctx->rwlock);
  ctx->handlers[handler_idx].level = new_level;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_handler_set_formatter(log *ctx, int handler_idx, log_FormatFn new_fn) {
  (void)handler_idx;
  rwlock_write_lock(&ctx->rwlock);
  ctx->format_fn = new_fn ? new_fn : format_text;
  rwlock_write_unlock(&ctx->rwlock);
}

typedef struct {
  log_FormatFn transform;
  log_LogFn output;
  void* context;
} log_stage_function;

void log_configure_pipeline(log* ctx, log_stage_function* stages, int stage_count) {
  rwlock_write_lock(&ctx->rwlock);
  
  for (int i = 0; i < stage_count && i < ctx->handler_count; i++) {
    if (stages[i].transform) {
      ctx->format_fn = stages[i].transform;
    }
    if (stages[i].output) {
      ctx->handlers[i].fn = stages[i].output;
    }
  }
  
  rwlock_write_unlock(&ctx->rwlock);
}

void log_enable_text_format(log* ctx) {
  rwlock_write_lock(&ctx->rwlock);
  ctx->format_mode = LOG_FORMAT_TEXT;
  rwlock_write_unlock(&ctx->rwlock);
}

void log_enable_json_format(log* ctx) {
  rwlock_write_lock(&ctx->rwlock);
  ctx->format_mode = LOG_FORMAT_JSON;
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

static void syslog_handler(log *ctx, log_Event *ev) {
  if (!ctx) return;

  int priority = LOG_USER | log_level_to_syslog(ev->level);

  va_list args_copy;
  va_copy(args_copy, ev->ap);
  char msg_buf[4096];
  vsnprintf(msg_buf, sizeof(msg_buf), ev->fmt, args_copy);
  va_end(args_copy);

  if (ctx->handlers && ctx->handler_count > 0) {
    for (int i = 0; i < ctx->handler_count; i++) {
      if (ctx->handlers[i].show_thread_id && ctx->handlers[i].active) {
        char full_msg[5120];
        snprintf(full_msg, sizeof(full_msg), "[%lu] %s:%d: %s",
                LOG_GET_THREAD_ID(),
                ev->file ? ev->file : "", ev->line, msg_buf);
        syslog(priority, "%s", full_msg);
        return;
      }
    }
  }

  syslog(priority, "%s:%d: %s", ev->file ? ev->file : "", ev->line, msg_buf);
}

int log_add_syslog_handler(log *ctx, const char *ident, int facility, int level) {
  if (!ctx) return -1;

  rwlock_write_lock(&ctx->rwlock);

  if (!ctx->syslog_enabled_global) {
    if (ident) {
      free(ctx->syslog_ident);
      ctx->syslog_ident = strdup(ident);
    }
    ctx->syslog_facility = facility;
    openlog(ctx->syslog_ident ? ctx->syslog_ident : "log", LOG_PID | LOG_NDELAY, facility);
    ctx->syslog_enabled_global = true;
  }

  log_handler *h = &ctx->handlers[ctx->handler_count++];
  h->fn = syslog_handler;
  h->udata = NULL;
  h->level = level;
  h->active = true;
  h->fp = NULL;
  h->filename = NULL;
  h->file_size = 0;
  h->syslog_enabled = true;
  h->syslog_facility = facility;
  h->show_thread_id = false;

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

/* Wrapper for json_handler to avoid unused warning */
static void json_handler_wrapper(log *ctx, log_Event *ev) {
  json_handler(ctx, ev);
}

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
