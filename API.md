# Enhanced Log Library API Documentation

Version 2.0.0

## Table of Contents

1. [Overview](#overview)
2. [Core Functions](#core-functions)
3. [Configuration Functions](#configuration-functions)
4. [Handler Management](#handler-management)
5. [Format Functions](#format-functions)
6. [Thread Safety Features](#thread-safety-features)
7. [Syslog Support](#syslog-support)
8. [Performance Monitoring](#performance-monitoring)
9. [Macros](#macros)
10. [Examples](#examples)

---

## Overview

This is an enhanced logging library for C17 that provides:

- Thread-safe logging with reader-writer locks
- Asynchronous logging with lock-free queues
- Log file rotation based on size
- JSON format output for structured logging
- Dynamic runtime configuration
- Thread ID tracking
- Syslog integration
- Performance statistics

### Log Levels

```c
enum {
    LOG_TRACE,    // 0 - Most verbose
    LOG_DEBUG,    // 1 - Debug information
    LOG_INFO,     // 2 - General information
    LOG_WARN,     // 3 - Warning messages
    LOG_ERROR,    // 4 - Error messages
    LOG_FATAL,    // 5 - Critical errors
    LOG_LEVELS    // 6 - Number of levels
};
```

---

## Core Functions

### log_create()

Creates a new logger context.

**Prototype:**
```c
log* log_create(void);
```

**Returns:**
- Pointer to new logger context, or NULL on failure

**Example:**
```c
log *ctx = log_create();
if (!ctx) {
    fprintf(stderr, "Failed to create logger\n");
    exit(1);
}
```

**Notes:**
- Initializes with default settings
- Automatically adds stderr handler at LOG_TRACE level
- Must be paired with `log_destroy()`

---

### log_destroy()

Destroys a logger context and releases resources.

**Prototype:**
```c
void log_destroy(log *ctx);
```

**Parameters:**
- `ctx`: Logger context to destroy (can be NULL)

**Example:**
```c
log_destroy(ctx);
```

**Notes:**
- Stops async writer thread if enabled
- Closes syslog connection if open
- Frees all allocated memory
- Does NOT close file pointers added via `log_add_fp()`

---

### log_default()

Returns or creates the global default logger.

**Prototype:**
```c
log* log_default(void);
```

**Returns:**
- Pointer to default logger context

**Example:**
```c
log *default = log_default();
log_info("Using default logger");
```

**Notes:**
- Creates default logger on first call
- Persists for program lifetime
- Used by `log_*` macros without context

---

### log_log()

Core logging function that all macros call.

**Prototype:**
```c
void log_log(log *ctx, int level, const char *file, int line, const char *fmt, ...);
```

**Parameters:**
- `ctx`: Logger context
- `level`: Log level (LOG_TRACE to LOG_FATAL)
- `file`: Source file name (__FILE__)
- `line`: Line number (__LINE__)
- `fmt`: Printf-style format string
- `...`: Variable arguments

**Example:**
```c
log_log(ctx, LOG_INFO, __FILE__, __LINE__, "Value: %d", 42);
```

**Notes:**
- Thread-safe with reader-writer locks
- Filters messages below configured level
- Respects quiet mode setting
- Supports async mode with lock-free queue

---

## Configuration Functions

### log_set_level()

Sets the minimum log level to output.

**Prototype:**
```c
void log_set_level(log *ctx, int level);
```

**Parameters:**
- `ctx`: Logger context
- `level`: Minimum level (LOG_TRACE to LOG_FATAL)

**Example:**
```c
// Only show warnings and above
log_set_level(ctx, LOG_WARN);
```

**Thread Safety:**
- Safe to call from multiple threads

---

### log_set_quiet()

Enables or disables quiet mode.

**Prototype:**
```c
void log_set_quiet(log *ctx, bool enable);
```

**Parameters:**
- `ctx`: Logger context
- `enable`: true to suppress stderr output

**Example:**
```c
log_set_quiet(ctx, true);  // Suppress stderr
```

**Notes:**
- Only affects stderr handler
- File handlers continue to receive logs

---

### log_set_format()

Sets a custom format function.

**Prototype:**
```c
void log_set_format(log *ctx, log_FormatFn fn);
```

**Parameters:**
- `ctx`: Logger context
- `fn`: Format function (NULL for default)

**Format Function Signature:**
```c
typedef int (*log_FormatFn)(log *ctx, log_Event *ev, char *buf, size_t buf_size);
```

**Example:**
```c
int custom_format(log *ctx, log_Event *ev, char *buf, size_t buf_size) {
    return snprintf(buf, buf_size, "[%s] %s:%d - ",
                   log_level_string(ev->level), ev->file, ev->line);
}

log_set_format(ctx, custom_format);
```

---

### log_set_async()

Enables or disables asynchronous logging.

**Prototype:**
```c
int log_set_async(log *ctx, bool enable);
```

**Parameters:**
- `ctx`: Logger context
- `enable`: true to enable async mode

**Returns:**
- 0 on success, -1 on failure

**Example:**
```c
if (log_set_async(ctx, true) != 0) {
    fprintf(stderr, "Failed to enable async logging\n");
}
```

**Notes:**
- Uses lock-free SPSC queue
- Background thread handles writes
- Queue drops messages when full
- Must disable before destroy

---

### log_set_max_file_size()

Sets the maximum file size for rotation.

**Prototype:**
```c
void log_set_max_file_size(log *ctx, size_t size);
```

**Parameters:**
- `ctx`: Logger context
- `size`: Maximum size in bytes

**Example:**
```c
log_set_max_file_size(ctx, 10 * 1024 * 1024);  // 10MB
```

**Notes:**
- Default is 10MB
- Applies to all file handlers

---

### log_set_file_prefix()

Sets the file prefix for log rotation.

**Prototype:**
```c
void log_set_file_prefix(log *ctx, const char *prefix);
```

**Parameters:**
- `ctx`: Logger context
- `prefix`: File prefix (e.g., "app.log")

**Example:**
```c
log_set_file_prefix(ctx, "application.log");
```

**Notes:**
- Default is "log"
- Rotated files named: prefix.1, prefix.2, etc.

---

## Handler Management

### log_add_handler()

Adds a custom handler function.

**Prototype:**
```c
int log_add_handler(log *ctx, log_LogFn fn, void *udata, int level);
```

**Parameters:**
- `ctx`: Logger context
- `fn`: Handler function
- `udata`: User data passed to handler
- `level`: Minimum level for this handler

**Returns:**
- Handler index on success, -1 on failure

**Handler Function Signature:**
```c
typedef void (*log_LogFn)(log *ctx, log_Event *ev);
```

**Example:**
```c
void my_handler(log *ctx, log_Event *ev) {
    fprintf((FILE*)ev->udata, "CUSTOM: %s\n", ev->fmt);
}

int idx = log_add_handler(ctx, my_handler, myfile, LOG_INFO);
```

---

### log_add_fp()

Adds a file pointer as a handler.

**Prototype:**
```c
int log_add_fp(log *ctx, FILE *fp, int level);
```

**Parameters:**
- `ctx`: Logger context
- `fp`: File pointer (e.g., fopen result)
- `level`: Minimum level for this handler

**Returns:**
- Handler index on success, -1 on failure

**Example:**
```c
FILE *fp = fopen("app.log", "a");
int idx = log_add_fp(ctx, fp, LOG_INFO);
```

**Notes:**
- Supports automatic rotation
- User must close file pointer

---

### log_remove_handler()

Removes a handler by index.

**Prototype:**
```c
void log_remove_handler(log *ctx, int idx);
```

**Parameters:**
- `ctx`: Logger context
- `idx`: Handler index (from log_add_handler/add_fp)

**Example:**
```c
log_remove_handler(ctx, idx);
```

---

### log_handler_set_level()

Changes a handler's minimum level.

**Prototype:**
```c
void log_handler_set_level(log *ctx, int handler_idx, int new_level);
```

**Parameters:**
- `ctx`: Logger context
- `handler_idx`: Handler index
- `new_level`: New minimum level

**Example:**
```c
// Change handler to only show errors
log_handler_set_level(ctx, idx, LOG_ERROR);
```

---

### log_handler_set_formatter()

Changes a handler's format function.

**Prototype:**
```c
void log_handler_set_formatter(log *ctx, int handler_idx, log_FormatFn new_fn);
```

**Parameters:**
- `ctx`: Logger context
- `handler_idx`: Handler index
- `new_fn`: New format function

**Example:**
```c
log_handler_set_formatter(ctx, idx, log_format_json);
```

---

## Format Functions

### log_format_json()

Formats a log event as JSON.

**Prototype:**
```c
const char* log_format_json(log *ctx, log_Event *ev, char *buf, size_t buf_size);
```

**Parameters:**
- `ctx`: Logger context
- `ev`: Log event
- `buf`: Output buffer
- `buf_size`: Buffer size

**Returns:**
- Pointer to formatted string (same as buf)

**Example:**
```c
char buf[8192];
log_Event ev = {0};
ev.fmt = "Test message";
ev.level = LOG_INFO;
const char *json = log_format_json(ctx, &ev, buf, sizeof(buf));
```

**Output Format:**
```json
{
  "time": "2024-01-15T10:30:45.123",
  "level": "INFO",
  "file": "main.c",
  "line": 42,
  "message": "Test message"
}
```

---

### log_enable_text_format()

Enables text format mode.

**Prototype:**
```c
void log_enable_text_format(log* ctx);
```

**Parameters:**
- `ctx`: Logger context

**Example:**
```c
log_enable_text_format(ctx);
```

---

### log_enable_json_format()

Enables JSON format mode.

**Prototype:**
```c
void log_enable_json_format(log* ctx);
```

**Parameters:**
- `ctx`: Logger context

**Example:**
```c
log_enable_json_format(ctx);
```

---

## Thread Safety Features

### log_enable_thread_id()

Enables or disables thread ID in log output.

**Prototype:**
```c
void log_enable_thread_id(log *ctx, int handler_idx, bool enable);
```

**Parameters:**
- `ctx`: Logger context
- `handler_idx`: Handler index
- `enable`: true to show thread ID

**Example:**
```c
int idx = log_add_fp(ctx, fp, LOG_INFO);
log_enable_thread_id(ctx, idx, true);
```

**Output Example:**
```
2024-01-15T10:30:45.123 INFO [140234567890432] main.c:42: Message
```

**Thread Safety:**
- All public APIs are thread-safe
- Uses reader-writer locks for configuration
- Lock-free queue for async mode
- Safe to call from multiple threads

---

## Syslog Support

### log_level_to_syslog()

Converts log level to syslog priority.

**Prototype:**
```c
int log_level_to_syslog(int level);
```

**Parameters:**
- `level`: Log level

**Returns:**
- Syslog priority constant

**Mapping:**
| Log Level | Syslog Priority |
|-----------|----------------|
| LOG_TRACE | LOG_DEBUG |
| LOG_DEBUG | LOG_DEBUG |
| LOG_INFO  | LOG_INFO |
| LOG_WARN  | LOG_WARNING |
| LOG_ERROR | LOG_ERR |
| LOG_FATAL | LOG_CRIT |

---

### log_add_syslog_handler()

Adds a syslog handler.

**Prototype:**
```c
int log_add_syslog_handler(log *ctx, const char *ident, int facility, int level);
```

**Parameters:**
- `ctx`: Logger context
- `ident`: Program identifier (NULL for default)
- `facility`: Syslog facility (e.g., LOG_USER, LOG_LOCAL0)
- `level`: Minimum level

**Returns:**
- Handler index on success, -1 on failure

**Example:**
```c
int idx = log_add_syslog_handler(ctx, "myapp", LOG_USER, LOG_INFO);
log_info("This goes to syslog");
```

**Notes:**
- Opens syslog connection on first call
- Uses LOG_PID | LOG_NDELAY flags
- Thread-safe

---

### log_handler_enable_syslog()

Enables syslog for an existing handler.

**Prototype:**
```c
void log_handler_enable_syslog(log *ctx, int handler_idx, bool enable);
```

**Parameters:**
- `ctx`: Logger context
- `handler_idx`: Handler index
- `enable`: true to enable syslog

**Example:**
```c
int idx = log_add_fp(ctx, fp, LOG_INFO);
log_handler_enable_syslog(ctx, idx, true);
```

---

## Performance Monitoring

### log_get_stats()

Retrieves performance statistics.

**Prototype:**
```c
int log_get_stats(log *ctx, log_stats *stats);
```

**Parameters:**
- `ctx`: Logger context
- `stats`: Output structure

**Returns:**
- 0 on success, -1 on failure

**Structure Definition:**
```c
typedef struct log_stats {
    uint64_t total_count;              // Total messages logged
    uint64_t level_counts[LOG_LEVELS]; // Count per level
    uint64_t queue_drops;             // Dropped messages (async)
    uint64_t rotation_count;          // File rotations
    double avg_queue_latency_ms;       // Avg async latency
    uint64_t async_writes;            // Async write count
    uint64_t sync_writes;             // Sync write count
} log_stats;
```

**Example:**
```c
log_stats stats;
log_get_stats(ctx, &stats);
printf("Total messages: %lu\n", stats.total_count);
printf("Queue drops: %lu\n", stats.queue_drops);
```

---

### log_rotate()

Manually triggers log rotation.

**Prototype:**
```c
void log_rotate(log *ctx);
```

**Parameters:**
- `ctx`: Logger context

**Example:**
```c
log_rotate(ctx);
```

**Notes:**
- Rotates all file handlers
- Creates rotated files: .1, .2, .3, .4, .5
- Removes oldest (.5) if exists

---

## Macros

### Default Logger Macros

These use the global default logger:

```c
log_trace(fmt, ...);
log_debug(fmt, ...);
log_info(fmt, ...);
log_warn(fmt, ...);
log_error(fmt, ...);
log_fatal(fmt, ...);
```

**Example:**
```c
log_info("Application started");
log_error("Failed to open file: %s", filename);
```

---

### Context-Specific Macros

These use a specific logger context:

```c
log_ctx_trace(ctx, fmt, ...);
log_ctx_debug(ctx, fmt, ...);
log_ctx_info(ctx, fmt, ...);
log_ctx_warn(ctx, fmt, ...);
log_ctx_error(ctx, fmt, ...);
log_ctx_fatal(ctx, fmt, ...);
```

**Example:**
```c
log_ctx_info(ctx, "Processing item %d", i);
```

---

## Examples

### Basic Usage

```c
#include "log.h"

int main(void) {
    log *ctx = log_create();

    log_ctx_trace(ctx, "Detailed debug info");
    log_ctx_debug(ctx, "Debug information");
    log_ctx_info(ctx, "Application started");
    log_ctx_warn(ctx, "Configuration file missing");
    log_ctx_error(ctx, "Failed to connect");
    log_ctx_fatal(ctx, "Critical error, exiting");

    log_destroy(ctx);
    return 0;
}
```

---

### File Logging with Rotation

```c
int main(void) {
    log *ctx = log_create();

    // Configure rotation
    log_set_file_prefix(ctx, "app.log");
    log_set_max_file_size(ctx, 5 * 1024 * 1024);  // 5MB

    // Add file handler
    FILE *fp = fopen("app.log", "a");
    int idx = log_add_fp(ctx, fp, LOG_INFO);

    // Write logs
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "Log message %d", i);
    }

    log_remove_handler(ctx, idx);
    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

---

### JSON Format Output

```c
int main(void) {
    log *ctx = log_create();

    // Set JSON format
    log_set_format(ctx, log_format_json);

    // Add file handler
    FILE *fp = fopen("logs.json", "w");
    log_add_fp(ctx, fp, LOG_INFO);

    log_ctx_info(ctx, "User login: id=%d, name=%s", 123, "Alice");
    log_ctx_error(ctx, "Database error: %s", "connection timeout");

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

---

### Thread Safety

```c
#include <pthread.h>

void* worker_thread(void *arg) {
    log *ctx = (log*)arg;
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "Thread %lu: Message %d",
                    pthread_self(), i);
    }
    return NULL;
}

int main(void) {
    log *ctx = log_create();

    // Enable thread ID for file handler
    FILE *fp = fopen("thread.log", "w");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    log_enable_thread_id(ctx, idx, true);

    // Create threads
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker_thread, ctx);
    }

    // Wait for completion
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

---

### Async Logging

```c
int main(void) {
    log *ctx = log_create();

    // Enable async mode
    log_set_async(ctx, true);

    FILE *fp = fopen("async.log", "w");
    log_add_fp(ctx, fp, LOG_INFO);

    // Write many messages quickly
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "Async message %d", i);
    }

    // Disable and wait for flush
    log_set_async(ctx, false);

    // Check stats
    log_stats stats;
    log_get_stats(ctx, &stats);
    printf("Total: %lu, Async writes: %lu\n",
           stats.total_count, stats.async_writes);

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

---

### Syslog Integration

```c
int main(void) {
    log *ctx = log_create();

    // Add syslog handler
    int idx = log_add_syslog_handler(ctx, "myapp", LOG_USER, LOG_INFO);

    // Enable thread ID in syslog
    log_enable_thread_id(ctx, idx, true);

    log_ctx_info(ctx, "Application started");
    log_ctx_error(ctx, "Failed to connect to database");

    log_destroy(ctx);
    return 0;
}
```

---

### Multiple Handlers

```c
int main(void) {
    log *ctx = log_create();

    // Console handler (all levels)
    // Already added by log_create()

    // File handler (INFO+)
    FILE *fp_info = fopen("info.log", "w");
    int idx_info = log_add_fp(ctx, fp_info, LOG_INFO);

    // Error file handler (ERROR+ only)
    FILE *fp_error = fopen("error.log", "w");
    int idx_error = log_add_fp(ctx, fp_error, LOG_ERROR);

    // JSON handler
    FILE *fp_json = fopen("logs.json", "w");
    log_set_format(ctx, log_format_json);
    int idx_json = log_add_fp(ctx, fp_json, LOG_INFO);

    // Write logs
    log_ctx_info(ctx, "This goes to console and info.log");
    log_ctx_error(ctx, "This goes to all handlers");

    // Cleanup
    log_remove_handler(ctx, idx_info);
    log_remove_handler(ctx, idx_error);
    log_remove_handler(ctx, idx_json);
    fclose(fp_info);
    fclose(fp_error);
    fclose(fp_json);
    log_destroy(ctx);
    return 0;
}
```

---

### Dynamic Configuration

```c
int main(void) {
    log *ctx = log_create();

    FILE *fp = fopen("app.log", "w");
    int idx = log_add_fp(ctx, fp, LOG_INFO);

    // Initial configuration
    log_ctx_info(ctx, "Starting with INFO level");

    // Change level dynamically
    log_set_level(ctx, LOG_ERROR);
    log_ctx_info(ctx, "This won't appear (below ERROR)");

    // Change handler level
    log_handler_set_level(ctx, idx, LOG_DEBUG);
    log_ctx_info(ctx, "Now INFO appears for this handler");

    // Switch to JSON format
    log_set_format(ctx, log_format_json);
    log_ctx_info(ctx, "This is JSON formatted");

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

---

## Building

### Compile as Library

```bash
gcc -c -std=c17 -Wall -Wextra src/log.c -o log.o
ar rcs liblog.a log.o
```

### Compile with Application

```bash
gcc -std=c17 -Wall -Wextra -I./src \
    src/log.c src/example.c -o example -lpthread
```

### Enable Color Output

```bash
gcc -DLOG_USE_COLOR -std=c17 -Wall -Wextra \
    src/log.c src/example.c -o example -lpthread
```

---

## License

MIT License - See LICENSE file for details.

## Version History

- **2.0.0** (2026): Added async logging, JSON format, thread ID, syslog support
- **1.0.0** (2020): Original implementation by rxi
