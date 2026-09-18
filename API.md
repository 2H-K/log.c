# Enhanced Log Library API Documentation

Version 0.1.0

## Table of Contents

1. [Overview](#overview)
2. [Core Functions](#core-functions)
3. [Configuration Functions](#configuration-functions)
4. [Handler Management](#handler-management)
5. [Filtering & Flood Control](#filtering--flood-control)
6. [Named Loggers](#named-loggers)
7. [Format Functions](#format-functions)
8. [Thread Safety Features](#thread-safety-features)
9. [Syslog Support](#syslog-support)
10. [Performance Monitoring](#performance-monitoring)
11. [Macros](#macros)
12. [Examples](#examples)

---

## Overview

This is an enhanced logging library for C17 that provides:

- Thread-safe logging with reader-writer locks
- Asynchronous logging with Asynchronous Queue: dedicated writer thread drains a bounded queue
- Log file rotation based on size
- JSON format output for structured logging
- Typed key-value metadata (`LOG_KV_*`) as top-level JSON fields
- Fork/exit lifecycle safety (POSIX): reinitialize locks in the child, flush on exit
- Dynamic runtime configuration
- Thread ID tracking
- Syslog integration
- Performance statistics

### Public API vs. internals

Only the functions, macros and types documented in this file are public.
`src/log.h` also exposes the concrete layout of `log_handle` and its helper
structs (fenced as `INTERNAL` in the header) so the two-file build and its
white-box tests can compile. Those fields are not part of the API, carry no
ABI guarantee, and may change without notice — do not access them from
application code.

### Versioning & stability

The current version is **0.1.0**, and there is no source/ABI stability guarantee
yet. Public declarations may still be renamed, reshaped or removed between
revisions.

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
log_handle* log_create(void);
```

**Returns:**
- Pointer to new logger context, or NULL on failure

**Example:**
```c
log_handle *ctx = log_create();
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
void log_destroy(log_handle *ctx);
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
log_handle* log_default(void);
```

**Returns:**
- Pointer to default logger context

**Example:**
```c
log_handle *default = log_default();
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
void log_log(log_handle *ctx, int level, const char *file, int line, const char *fmt, ...);
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
- Supports async mode with Asynchronous Queue: dedicated writer thread drains a bounded queue

---

### log_log_kv()

Structured logging with typed key-value metadata. Prefer the `log_*_kv()`
macros, which build the pair array on the caller's stack.

**Prototype:**
```c
void log_log_kv(log_handle *ctx, int level, const char *file, int line,
                const log_kv *kvs, int kv_count, const char *msg);
```

**Types and helpers:**
```c
typedef struct log_kv {
    const char *key;   /* NULL entries are skipped */
    int type;          /* LOG_KV_T_INT / _DOUBLE / _STR / _BOOL */
    long long i;       /* INT / BOOL value */
    double d;          /* DOUBLE value */
    const char *s;     /* STR value (borrowed) */
} log_kv;

#define LOG_KV_INT(k, v)
#define LOG_KV_DOUBLE(k, v)
#define LOG_KV_STR(k, v)
#define LOG_KV_BOOL(k, v)
#define LOG_KV_END
```

**Example:**
```c
log_ctx_info_kv(ctx, "login",
                LOG_KV_STR("user", "alice"),
                LOG_KV_INT("id", 42));
```

**Notes:**
- `msg` is emitted **literally** (not a printf format), so it may contain `%`.
- JSON output emits each pair as a top-level field; text output appends
  `key=value` after the message.
- At most `LOG_KV_MAX_PAIRS` (8) pairs are encoded; extras are dropped and
  counted in `log_stats.truncated_count`.
- Works on the sync and async paths.
- Trimmed by `LOG_DISABLE_KV` / `LOG_MINIMAL`; the `*_kv` macros then degrade to
  plain message logging.
- Per-level macros: `log_ctx_trace_kv`, `log_ctx_debug_kv`, `log_ctx_info_kv`,
  `log_ctx_warn_kv`, `log_ctx_error_kv`, `log_ctx_fatal_kv`, plus the
  default-context `log_trace_kv` … `log_fatal_kv`.

---

### log_install_atfork()

Installs `pthread_atfork` handlers for a context (POSIX only). Around `fork()`
the context is quiesced (all locks held); in the child the synchronization
primitives are reinitialized and async logging is downgraded to synchronous, so
the child can keep logging without deadlocking on locks inherited from threads
that no longer exist.

**Prototype:**
```c
int log_install_atfork(log_handle *ctx);
```

**Returns:**
- 0 on success, -1 on failure / unsupported platform (Windows)

**Example:**
```c
log_install_atfork(ctx);   /* once during setup, before spawning threads */
```

**Notes:**
- Idempotent per context; `log_destroy()` unregisters it (up to 8 contexts).
- Call from a context that is not inside a log handler (do not call `fork()`
  while the library holds this context's locks).
- Pending async entries at the instant of `fork()` are abandoned in the child,
  not duplicated.

---

### log_install_atexit()

Registers an `atexit` handler that drains a still-async context when the process
exits through `exit()` / `return` from `main` (POSIX only), so queued messages
are not lost.

**Prototype:**
```c
int log_install_atexit(log_handle *ctx);
```

**Returns:**
- 0 on success, -1 on failure / unsupported platform (Windows)

**Example:**
```c
log_set_async(ctx, true);
log_install_atexit(ctx);
/* ... log without calling log_set_async(false) ... */
```

**Notes:**
- Idempotent per context; `log_destroy()` unregisters it.
- On Windows, call `log_set_async(ctx, false)` before exiting to flush.

---

## Configuration Functions

### log_set_level()

Sets the minimum log level to output.

**Prototype:**
```c
void log_set_level(log_handle *ctx, int level);
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
void log_set_quiet(log_handle *ctx, bool enable);
```

**Parameters:**
- `ctx`: Logger context
- `enable`: true to suppress all logging output

**Example:**
```c
log_set_quiet(ctx, true);  // Suppress all output
```

**Notes:**
- When enabled, suppresses ALL handlers (including file handlers)
- Use this to completely silence the logger
- To suppress only stderr while keeping file handlers, remove the stderr handler instead

---

### log_set_format()

Sets a custom format function.

**Prototype:**
```c
void log_set_format(log_handle *ctx, log_FormatFn fn);
```

**Parameters:**
- `ctx`: Logger context
- `fn`: Format function (NULL for default)

**Format Function Signature:**
```c
typedef int (*log_FormatFn)(log_handle *ctx, log_Event *ev, char *buf, size_t buf_size);
```

**Example:**
```c
int custom_format(log_handle *ctx, log_Event *ev, char *buf, size_t buf_size) {
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
int log_set_async(log_handle *ctx, bool enable);
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
- Uses a bounded queue drained by a writer thread
- Background thread handles writes
- What happens when the queue is full is controlled by `log_set_queue_policy`; the default is `LOG_QUEUE_FALLBACK_SYNC` (writes on the calling thread, does **not** drop)
- Must disable before destroy

---

### log_set_queue_policy()

Selects what happens when the async queue is full. Only meaningful with async
logging enabled.

**Prototype:**
```c
void log_set_queue_policy(log_handle *ctx, int policy);
```

**Parameters:**
- `ctx`: Logger context
- `policy`: One of `LOG_QUEUE_FALLBACK_SYNC` (default), `LOG_QUEUE_DROP`, `LOG_QUEUE_BLOCK`

**Trade-offs:**

| Policy | Blocks caller? | Can lose messages? | Worst-case caller latency | Fits |
|--------|----------------|--------------------|---------------------------|------|
| `LOG_QUEUE_FALLBACK_SYNC` | On overflow | No (written synchronously) | Sink latency, on the calling thread | Correctness over latency |
| `LOG_QUEUE_DROP` | Never | Yes (`queue_drops`) | Bounded | Best-effort, latency-critical paths |
| `LOG_QUEUE_BLOCK` | On overflow | No | Unbounded under sustained overload | Producers that tolerate backpressure |

No policy provides both a hard latency ceiling and guaranteed delivery; that
guarantee must come from an external collector. Monitor
`log_get_stats`→`queue_drops` / `queue_blocked`.

**Example:**
```c
log_set_async(ctx, true);
log_set_queue_policy(ctx, LOG_QUEUE_DROP);
```

---

### log_set_max_file_size()

Sets the maximum file size for rotation.

**Prototype:**
```c
void log_set_max_file_size(log_handle *ctx, size_t size);
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
void log_set_file_prefix(log_handle *ctx, const char *prefix);
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
int log_add_handler(log_handle *ctx, log_LogFn fn, void *udata, int level);
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
typedef void (*log_LogFn)(log_handle *ctx, log_Event *ev);
```

**Example:**
```c
void my_handler(log_handle *ctx, log_Event *ev) {
    fprintf((FILE*)ev->udata, "CUSTOM: %s\n", ev->fmt);
}

int idx = log_add_handler(ctx, my_handler, myfile, LOG_INFO);
```

---

### log_add_fp()

Adds a file pointer as a handler.

**Prototype:**
```c
int log_add_fp(log_handle *ctx, FILE *fp, int level);
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

### log_add_memory_handler()

Adds an in-memory flight-recorder handler (B1): it retains the most recent
`lines` fully rendered log lines in a ring, performing no I/O.

**Prototype:**
```c
int log_add_memory_handler(log_handle *ctx, int lines, int level);
```

**Parameters:**
- `ctx`: Logger context
- `lines`: Number of recent lines to retain; clamped to the compile-time cap
  `LOG_MEMORY_MAX_LINES`. Must be > 0.
- `level`: Minimum level for this handler

**Returns:**
- Handler index on success, -1 on invalid arguments / allocation failure /
  handler table full

**Example:**
```c
int idx = log_add_memory_handler(ctx, 256, LOG_WARN);
/* ... run ... */
log_dump_memory_handler(ctx, idx, stderr);   /* oldest -> newest */
```

**Notes:**
- Storage is allocated per handler at call time; contexts without a memory
  handler pay nothing.
- Trim with `LOG_DISABLE_MEMORY_HANDLER` / `LOG_MINIMAL` (then this returns -1).

---

### log_dump_memory_handler()

Writes the lines retained by a memory handler to a stream, oldest first.

**Prototype:**
```c
void log_dump_memory_handler(log_handle *ctx, int handler_idx, FILE *out);
```

**Parameters:**
- `ctx`: Logger context
- `handler_idx`: Index returned by `log_add_memory_handler()`
- `out`: Destination stream (e.g. `stderr`, `stdout`, or an `fopen` result)

**Returns:**
- Nothing. Invalid arguments or a non-memory handler index are no-ops.

**Notes:**
- The snapshot is taken under the context read lock and the per-handler store
  lock, so it is consistent with concurrent producers (no torn or reordered
  records).
- Each record is a complete rendered line including its trailing `\n`;
  oversized lines are truncated to `LOG_MEMORY_LINE_MAX` and counted in
  `log_stats::truncated_count`.

---

### log_remove_handler()

Removes a handler by index.

**Prototype:**
```c
void log_remove_handler(log_handle *ctx, int idx);
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
void log_handler_set_level(log_handle *ctx, int handler_idx, int new_level);
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
void log_handler_set_formatter(log_handle *ctx, int handler_idx, log_FormatFn new_fn);
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

## Filtering & Flood Control

Rate limiting and duplicate suppression keep a repeated error from amplifying
an incident (B3). Both are configured per level and are applied **before** the
message is enqueued, so a flood never fills the async queue (and therefore
never triggers `LOG_QUEUE_DROP`). Suppressed messages are counted in
`log_stats::suppressed_count`. Rules are inert unless configured, and the
whole feature is compiled out by `LOG_DISABLE_FILTER` / `LOG_MINIMAL`.

### log_set_rate_limit()

Allows at most `max_per_sec` messages of `level` per one-second window.

**Prototype:**
```c
void log_set_rate_limit(log_handle *ctx, int level, unsigned max_per_sec);
```

**Parameters:**
- `ctx`: Logger context
- `level`: The exact level the rule applies to (out-of-range ignored)
- `max_per_sec`: Allowed messages per second; `0` disables the rule

**Notes:**
- The window starts with the first accepted message and resets one second
  later; the first `max_per_sec` messages of a window are emitted, the rest
  are suppressed.

**Example:**
```c
log_set_rate_limit(ctx, LOG_ERROR, 10);   /* at most 10 errors/second */
```

---

### log_set_dedupe()

Suppresses consecutive identical messages of `level` within `window_ms`,
emitting a single `last message repeated N times` summary when the window
expires or a different message arrives.

**Prototype:**
```c
void log_set_dedupe(log_handle *ctx, int level, unsigned window_ms);
```

**Parameters:**
- `ctx`: Logger context
- `level`: The exact level the rule applies to (out-of-range ignored)
- `window_ms`: Suppression window in milliseconds; `0` disables the rule

**Notes:**
- The comparison is the FNV-1a hash of the rendered message; structured (KV)
  events also hash their encoded fields, so the same body with different
  fields is not collapsed.
- Each level keeps its own active group, so there is no cross-level collision.

**Example:**
```c
log_set_dedupe(ctx, LOG_WARN, 1000);   /* collapse repeated warnings */
```

---

### log_flush_suppressed()

Immediately emits any pending dedupe summaries.

**Prototype:**
```c
void log_flush_suppressed(log_handle *ctx);
```

**Notes:**
- Optional: `log_destroy()` flushes pending summaries automatically, and a
  summary for a group is emitted as soon as its window expires and another
  message arrives.
- Safe to call at any time; a no-op when there is nothing pending.

---

## Named Loggers

Named loggers (B4) give each module an independent level switch. They are
created lazily and attached to the default context, so all handlers are
shared. A named logger is not re-filtered by the default context's level,
only by its own level; per-handler minimum levels still apply.

### log_get()

Returns (creating once if needed) the named logger handle.

**Prototype:**
```c
log_handle* log_get(const char *name);
```

**Parameters:**
- `name`: Logger name, at most `LOG_NAMED_NAME_MAX - 1` (15) characters

**Returns:**
- The named handle on success; the default context if `name` is NULL/empty/
  overlong or the registry (`LOG_NAMED_MAX` = 16 slots) is full (a warning is
  logged in those cases, never a failure)

**Example:**
```c
log_handle *net = log_get("net");
log_ctx_info(net, "listening on %s", addr);
```

**Notes:**
- Safe to call concurrently: the same name always returns the same handle.
- The returned handle is an alias owned by the default context. Destroying it
  with `log_destroy()` is a no-op; it is freed when the default context is
  destroyed.

---

### log_named_set_level()

Sets the level of a named logger, creating it if necessary.

**Prototype:**
```c
void log_named_set_level(const char *name, int level);
```

**Parameters:**
- `name`: Logger name (invalid/overlong names are ignored)
- `level`: Minimum level for this logger

**Example:**
```c
log_named_set_level("net", LOG_DEBUG);
log_named_set_level("db",  LOG_ERROR);
```

---

## Format Functions

### log_format_json()

Formats a log event as a full JSON line.

**Prototype:**
```c
int log_format_json(log_handle *ctx, log_Event *ev, char *buf, size_t buf_size);
```

**Parameters:**
- `ctx`: Logger context
- `ev`: Log event
- `buf`: Output buffer
- `buf_size`: Buffer size

**Returns:**
- Number of characters written into `buf` (excluding NUL)

**Example:**
```c
char buf[8192];
log_Event ev = {0};
ev.fmt = "Test message";
ev.level = LOG_INFO;
int n = log_format_json(ctx, &ev, buf, sizeof(buf));
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
void log_enable_text_format(log_handle* ctx);
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
void log_enable_json_format(log_handle* ctx);
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
void log_enable_thread_id(log_handle *ctx, int handler_idx, bool enable);
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
- Asynchronous Queue: dedicated writer thread drains a bounded queue
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
int log_add_syslog_handler(log_handle *ctx, const char *ident, int facility, int level);
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
void log_handler_enable_syslog(log_handle *ctx, int handler_idx, bool enable);
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
int log_get_stats(log_handle *ctx, log_stats *stats);
```

**Parameters:**
- `ctx`: Logger context
- `stats`: Output structure

**Returns:**
- 0 on success, -1 on failure

**Notes:**
Counters are accumulated per thread without contention and aggregated across
every thread that logged to `ctx` when this function runs, so the totals are
process-wide regardless of the calling thread. At most 64 distinct
registrations per context are aggregated; additional threads still count
locally but are not included.

**Structure Definition:**
```c
typedef struct log_stats {
    uint64_t total_count;              // Total messages logged
    uint64_t level_counts[LOG_LEVELS]; // Count per level
    uint64_t queue_drops;             // Dropped messages (async)
    uint64_t queue_blocked;           // Times a producer blocked (async BLOCK)
    uint64_t rotation_count;          // File rotations
    double avg_queue_latency_ms;       // Mean async enqueue->dequeue latency (ms)
    uint64_t async_writes;            // Async write count
    uint64_t sync_writes;             // Sync write count
    uint64_t truncated_count;         // Messages truncated (static mode)
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
void log_rotate(log_handle *ctx);
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
    log_handle *ctx = log_create();

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
    log_handle *ctx = log_create();

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
    log_handle *ctx = log_create();

    // Set JSON format
    log_enable_json_format(ctx);

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
    log_handle *ctx = (log_handle*)arg;
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "Thread %lu: Message %d",
                    pthread_self(), i);
    }
    return NULL;
}

int main(void) {
    log_handle *ctx = log_create();

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
    log_handle *ctx = log_create();

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
    log_handle *ctx = log_create();

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
    log_handle *ctx = log_create();

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
    log_enable_json_format(ctx);
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
    log_handle *ctx = log_create();

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
    log_enable_json_format(ctx);
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
