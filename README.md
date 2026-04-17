# Enhanced C Log Library

A simple, powerful, and thread-safe logging library implemented in C17.

![screenshot](https://cloud.githubusercontent.com/assets/3920290/23831970/a2415e96-0723-11e7-9886-f8f5d2de60fe.png)

## Features

- **Thread-Safe**: Reader-writer locks for concurrent access
- **Async Logging**: Lock-free queue for non-blocking writes
- **Log Rotation**: Automatic file rotation by size
- **Structured Logging**: JSON format support
- **Thread ID Tracking**: Optional thread ID in output
- **Syslog Integration**: Native syslog support
- **Dynamic Configuration**: Runtime level/format changes
- **Performance Stats**: Built-in metrics and monitoring
- **NULL Safe**: Robust handling of NULL strings

## Quick Start

### Basic Usage

```c
#include "log.h"

int main(void) {
    log *ctx = log_create();

    log_ctx_trace(ctx, "Detailed debug info");
    log_ctx_info(ctx, "Application started");
    log_ctx_error(ctx, "Failed to connect: %s", "timeout");

    log_destroy(ctx);
    return 0;
}
```

### Using Default Logger

```c
log_info("Application started");
log_error("Error: %s", strerror(errno));
```

## Core Features

### 1. Log Levels

Six log levels from most to least verbose:

| Level | Description |
|-------|-------------|
| LOG_TRACE | Detailed debugging information |
| LOG_DEBUG | Debug information |
| LOG_INFO | General informational messages |
| LOG_WARN | Warning messages |
| LOG_ERROR | Error conditions |
| LOG_FATAL | Critical errors |

### 2. File Logging with Rotation

```c
log *ctx = log_create();

// Configure rotation (10MB max size, 5 rotated files)
log_set_file_prefix(ctx, "app.log");
log_set_max_file_size(ctx, 10 * 1024 * 1024);

// Add file handler
FILE *fp = fopen("app.log", "a");
int idx = log_add_fp(ctx, fp, LOG_INFO);

// Write logs - automatic rotation when size exceeded
for (int i = 0; i < 10000; i++) {
    log_ctx_info(ctx, "Message %d", i);
}

log_remove_handler(ctx, idx);
fclose(fp);
log_destroy(ctx);
```

### 3. JSON Format Output

```c
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
```

**Output:**
```json
{"time": "2024-01-15T10:30:45.123", "level": "INFO", "file": "main.c", "line": 42, "message": "User login: id=123, name=Alice"}
{"time": "2024-01-15T10:30:45.124", "level": "ERROR", "file": "main.c", "line": 43, "message": "Database error: connection timeout"}
```

### 4. Thread-Safe Logging

```c
#include <pthread.h>

void* worker_thread(void *arg) {
    log *ctx = (log*)arg;
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "Thread %lu: Message %d", pthread_self(), i);
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

**Output:**
```
2024-01-15T10:30:45.123 INFO [140234567890432] main.c:42: Thread 140234567890432: Message 0
2024-01-15T10:30:45.124 INFO [140234567890432] main.c:42: Thread 140234567890432: Message 1
```

### 5. Async Logging

```c
int main(void) {
    log *ctx = log_create();

    // Enable async mode with lock-free queue
    log_set_async(ctx, true);

    FILE *fp = fopen("async.log", "w");
    log_add_fp(ctx, fp, LOG_INFO);

    // Write many messages quickly - won't block
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "Async message %d", i);
    }

    // Disable and wait for flush
    log_set_async(ctx, false);

    // Check performance stats
    log_stats stats;
    log_get_stats(ctx, &stats);
    printf("Total: %lu, Async writes: %lu, Queue drops: %lu\n",
           stats.total_count, stats.async_writes, stats.queue_drops);

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

### 6. Syslog Integration

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

### 7. Multiple Handlers

```c
int main(void) {
    log *ctx = log_create();

    // Console handler (all levels) - added by default

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

### 8. Dynamic Configuration

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

## API Reference

For complete API documentation, see [API.md](API.md).

### Core Functions

```c
log* log_create(void);
void log_destroy(log *ctx);
log* log_default(void);
void log_log(log *ctx, int level, const char *file, int line, const char *fmt, ...);
```

### Configuration

```c
void log_set_level(log *ctx, int level);
void log_set_quiet(log *ctx, bool enable);
void log_set_format(log *ctx, log_FormatFn fn);
int log_set_async(log *ctx, bool enable);
void log_set_max_file_size(log *ctx, size_t size);
void log_set_file_prefix(log *ctx, const char *prefix);
```

### Handler Management

```c
int log_add_handler(log *ctx, log_LogFn fn, void *udata, int level);
int log_add_fp(log *ctx, FILE *fp, int level);
void log_remove_handler(log *ctx, int idx);
void log_handler_set_level(log *ctx, int handler_idx, int new_level);
void log_handler_set_formatter(log *ctx, int handler_idx, log_FormatFn new_fn);
```

### Format Functions

```c
const char* log_format_json(log *ctx, log_Event *ev, char *buf, size_t buf_size);
void log_enable_text_format(log* ctx);
void log_enable_json_format(log* ctx);
```

### Thread Safety Features

```c
void log_enable_thread_id(log *ctx, int handler_idx, bool enable);
```

### Syslog Support

```c
int log_level_to_syslog(int level);
int log_add_syslog_handler(log *ctx, const char *ident, int facility, int level);
void log_handler_enable_syslog(log *ctx, int handler_idx, bool enable);
```

### Performance Monitoring

```c
int log_get_stats(log *ctx, log_stats *stats);
void log_rotate(log *ctx);
```

## Macros

### Default Logger (Global)

```c
log_trace(fmt, ...);
log_debug(fmt, ...);
log_info(fmt, ...);
log_warn(fmt, ...);
log_error(fmt, ...);
log_fatal(fmt, ...);
```

### Context-Specific

```c
log_ctx_trace(ctx, fmt, ...);
log_ctx_debug(ctx, fmt, ...);
log_ctx_info(ctx, fmt, ...);
log_ctx_warn(ctx, fmt, ...);
log_ctx_error(ctx, fmt, ...);
log_ctx_fatal(ctx, fmt, ...);
```

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

## Examples

See [src/example.c](src/example.c) for comprehensive examples of all features:

- Basic logging
- Level filtering
- JSON format output
- File rotation
- Async logging
- Thread safety
- Dynamic handler configuration
- Mixed formatting
- Performance statistics

## Testing

Run the test suite:

```bash
make test
```

## Performance Statistics

The library provides built-in performance metrics:

```c
typedef struct log_stats {
    uint64_t total_count;               // Total messages logged
    uint64_t level_counts[LOG_LEVELS]; // Count per level
    uint64_t queue_drops;              // Dropped messages (async)
    uint64_t rotation_count;           // File rotations
    double avg_queue_latency_ms;        // Avg async latency
    uint64_t async_writes;             // Async write count
    uint64_t sync_writes;              // Sync write count
} log_stats;
```

## Thread Safety

All public APIs are thread-safe:

- **Reader-Writer Locks**: Protects configuration changes
- **Lock-Free Queue**: For async mode (SPSC)
- **Atomic Operations**: For statistics
- **Safe from Multiple Threads**: Can be called concurrently

## NULL Safety

The library handles NULL strings gracefully:

```c
log_ctx_info(ctx, NULL);        // Safe: prints "(null)"
log_ctx_info(ctx, "%s", NULL);  // Safe: prints "(null)"
```

## Configuration Constants

```c
#define LOG_VERSION "2.0.0"
#define LOG_MAX_QUEUE_SIZE 4096
#define LOG_MAX_ROTATION_FILES 5
#define LOG_DEFAULT_MAX_SIZE (10 * 1024 * 1024)  // 10MB
```

## License

MIT License - See [LICENSE](LICENSE) for details.

## Version History

- **2.0.0** (2026): Major enhancements
  - Added async logging with lock-free queue
  - Added JSON format support
  - Added thread ID tracking
  - Added syslog integration
  - Added performance statistics
  - Added dynamic configuration APIs
  - Enhanced thread safety with reader-writer locks
  - Added NULL string safety

- **1.0.0** (2020): Original implementation by rxi

## Credits

Based on [rxi/log.c](https://github.com/rxi/log.c) (Copyright 2020 rxi)

Enhanced with additional features in 2026.

## Contributing

Contributions are welcome! Please ensure:

1. Code follows C17 standard
2. All functions are documented
3. Tests pass
4. Thread safety is maintained
