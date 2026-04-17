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

#define _GNU_SOURCE
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

_Static_assert(sizeof(int) >= 4, "int must be at least 32 bits");

#define MAX_HANDLERS 32
#define DEFAULT_QUEUE_SIZE LOG_MAX_QUEUE_SIZE
#define DEFAULT_FORMAT LOG_FORMAT_TEXT

/* Global default logger */
static log *DEFAULT_LOG = NULL;

/* Level strings */
static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[91m"
};
#endif

/* Helper: Get high-precision timestamp */
static double get_timestamp(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Helper: Format timestamp to string */
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

/* Reader-Writer Lock Implementation */
static void rwlock_init(log_rwlock *lock) {
  atomic_store(&lock->readers, 0);
  atomic_store(&lock->writer, 0);
  atomic_store(&lock->write_waiting, false);
}

static void rwlock_read_lock(log_rwlock *lock) {
  int backoff = 1;
  while (atomic_load(&lock->writer) || atomic_load(&lock->write_waiting)) {
    for (int i = 0; i < backoff; i++) {
      __asm__ __volatile__("pause" ::: "memory");
    }
    if (backoff < 16) backoff *= 2;
  }
  atomic_fetch_add(&lock->readers, 1);
}

static void rwlock_read_unlock(log_rwlock *lock) {
  atomic_fetch_sub(&lock->readers, 1);
}

static void rwlock_write_lock(log_rwlock *lock) {
  atomic_store(&lock->write_waiting, true);
  int backoff = 1;
  while (atomic_load(&lock->readers) > 0 || atomic_load(&lock->writer) > 0) {
    for (int i = 0; i < backoff; i++) {
      __asm__ __volatile__("pause" ::: "memory");
    }
    if (backoff < 16) backoff *= 2;
  }
  atomic_store(&lock->write_waiting, false);
  atomic_store(&lock->writer, 1);
}

static void rwlock_write_unlock(log_rwlock *lock) {
  atomic_store(&lock->writer, 0);
}

/* Lock-free Queue Implementation */
static log_queue_entry* queue_entry_create(log_Event *ev) {
  log_queue_entry *entry = malloc(sizeof(log_queue_entry));
  if (!entry) return NULL;
  
  va_list args_copy;
  va_copy(args_copy, ev->ap);
  int len = vsnprintf(NULL, 0, ev->fmt, args_copy);
  va_end(args_copy);
  
  if (len < 0) {
    free(entry);
    return NULL;
  }
  
  entry->message = malloc(len + 1);
  if (!entry->message) {
    free(entry);
    return NULL;
  }
  vsnprintf(entry->message, len + 1, ev->fmt, ev->ap);
  
  entry->level = ev->level;
  entry->file = strdup(ev->file ? ev->file : "");
  entry->line = ev->line;
  entry->timestamp = ev->timestamp;
  entry->next = NULL;
  
  return entry;
}

static void queue_entry_destroy(log_queue_entry *entry) {
  if (!entry) return;
  free(entry->message);
  free(entry->file);
  free(entry);
}

static void queue_init(log_queue *q, size_t max_size) {
  log_queue_entry *dummy = calloc(1, sizeof(log_queue_entry));
  atomic_store(&q->head, dummy);
  atomic_store(&q->tail, dummy);
  atomic_store(&q->size, 0);
  atomic_store(&q->high_water_mark, 0);
  q->max_size = max_size;
}

static bool queue_push(log_queue *q, log_queue_entry *entry) {
  size_t current_size = atomic_load(&q->size);
  if (current_size >= q->max_size) {
    return false;
  }
  
  entry->next = NULL;
  log_queue_entry *old_tail = atomic_exchange(&q->tail, entry);
  atomic_store(&old_tail->next, entry);
  atomic_fetch_add(&q->size, 1);
  
  size_t new_size = atomic_load(&q->size);
  size_t hwm = atomic_load(&q->high_water_mark);
  while (new_size > hwm) {
    if (atomic_compare_exchange_weak(&q->high_water_mark, &hwm, new_size)) {
      break;
    }
  }
  
  return true;
}

static log_queue_entry* queue_pop(log_queue *q) {
  log_queue_entry *head = atomic_load(&q->head);
  log_queue_entry *next = atomic_load(&head->next);
  
  if (next == NULL) {
    return NULL;
  }
  
  if (atomic_compare_exchange_strong(&q->head, &head, next)) {
    atomic_fetch_sub(&q->size, 1);
    return head;
  }
  
  return NULL;
}

static void queue_destroy(log_queue *q) {
  log_queue_entry *entry;
  while ((entry = queue_pop(q)) != NULL) {
    queue_entry_destroy(entry);
  }
}

/* Default format functions */
static int format_text(log *ctx, log_Event *ev, char *buf, size_t buf_size) {
  (void)ctx;
  char time_buf[32];
  format_timestamp(ev->timestamp, time_buf, sizeof(time_buf));
  
  int written = snprintf(buf, buf_size, "%s %-5s %s:%d: ",
                         time_buf, level_strings[ev->level],
                         ev->file ? ev->file : "", ev->line);
  
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
  
  snprintf(buf, sizeof(buf),
    "{\"time\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %d, \"message\": \"%s\"}",
    time_buf, level_strings[ev->level],
    ev->file ? ev->file : "", ev->line, escaped_msg);
  
  fprintf(ev->udata, "%s\n", buf);
  fflush(ev->udata);
}

/* Async writer thread */
static void* async_writer_thread(void *arg) {
  log *ctx = (log*)arg;
  
  while (atomic_load(&ctx->async_running)) {
    log_queue_entry *entry = queue_pop(&ctx->queue);
    if (!entry) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000};
      nanosleep(&ts, NULL);
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
    
    queue_entry_destroy(entry);
  }
  
  return NULL;
}

/* API Implementation */
log* log_create(void) {
  log *ctx = calloc(1, sizeof(log));
  if (!ctx) return NULL;
  
  rwlock_init(&ctx->rwlock);
  pthread_mutex_init(&ctx->mutex, NULL);
  
  ctx->level = LOG_TRACE;
  ctx->quiet = false;
  ctx->format_mode = DEFAULT_FORMAT;
  ctx->max_file_size = LOG_DEFAULT_MAX_SIZE;
  ctx->async_enabled = false;
  atomic_store(&ctx->async_running, false);
  
  queue_init(&ctx->queue, DEFAULT_QUEUE_SIZE);
  
  ctx->handler_capacity = MAX_HANDLERS;
  ctx->handlers = calloc(MAX_HANDLERS, sizeof(log_handler));
  ctx->handler_count = 0;
  
  ctx->format_fn = format_text;
  
  memset(&ctx->stats, 0, sizeof(ctx->stats));
  
  log_add_handler(ctx, stdout_handler, stderr, LOG_TRACE);
  
  return ctx;
}

void log_destroy(log *ctx) {
  if (!ctx) return;
  
  if (ctx->async_enabled) {
    atomic_store(&ctx->async_running, false);
    pthread_join(ctx->async_thread, NULL);
  }
  
  queue_destroy(&ctx->queue);
  
  for (int i = 0; i < ctx->handler_count; i++) {
    free(ctx->handlers[i].filename);
    /* File pointers opened by user should not be closed here */
  }
  free(ctx->handlers);
  
  free(ctx->file_prefix);
  pthread_mutex_destroy(&ctx->mutex);
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
    atomic_store(&ctx->async_running, true);
    if (pthread_create(&ctx->async_thread, NULL, async_writer_thread, ctx) != 0) {
      atomic_store(&ctx->async_running, false);
      return -1;
    }
    ctx->async_enabled = true;
  } else if (!enable && ctx->async_enabled) {
    atomic_store(&ctx->async_running, false);
    pthread_join(ctx->async_thread, NULL);
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
    log_queue_entry *entry = queue_entry_create(&ev);
    va_end(ev.ap);
    
    if (entry && queue_push(&ctx->queue, entry)) {
      ctx->stats.async_writes++;
    } else {
      ctx->stats.queue_drops++;
      if (entry) {
        queue_entry_destroy(entry);
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
