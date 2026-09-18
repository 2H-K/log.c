# Enhanced C Log Library - Cross Platform

A simple, powerful, and thread-safe logging library implemented in C17 with full cross-platform support.

![screenshot](https://cloud.githubusercontent.com/assets/3920290/23831970/a2415e96-0723-11e7-9886-f8f5d2de60fe.png)

## 🚀 Features

- **Cross-Platform Support**: Windows (MSVC/MinGW-w64) & Linux/macOS (GCC/Clang)
- **Thread-Safe**: Reader-writer locks for configuration and concurrent access
- **Async Logging**: Mutex + condvar protected ring buffer queue with dedicated writer thread (for decoupling, not throughput)
- **Crash Safety**: `log_set_crash_safe` flushes every line; `log_install_crash_handler` writes a marker and dumps the flight-recorder tail on fatal signals (POSIX)
- **Lifecycle Safety**: `log_install_atfork` reinitializes locks in the child and downgrades async to sync after `fork()`; `log_install_atexit` flushes a still-async context on exit (POSIX)
- **Durability Policies**: Per-handler NEVER / EVERY / INTERVAL flush with an independent fsync switch
- **Static Zero-Allocation**: `-DLOG_STATIC_ALLOC` + `log_create_static`, zero heap allocations on the hot path
- **Log Rotation**: Automatic file rotation by size (up to 5 rotated files)
- **Structured Logging**: JSON format support; typed key-value metadata (`LOG_KV_*`) emitted as top-level JSON fields or a `key=value` text suffix
- **In-Memory Flight Recorder**: `log_add_memory_handler` retains the last N lines; `log_dump_memory_handler` exports a consistent snapshot with no I/O
- **Flood Control**: `log_set_rate_limit` per level and `log_set_dedupe` collapses repeats with a `last message repeated N times` summary; observable via `suppressed_count`
- **Named Loggers**: `log_get("net")` / `log_named_set_level` give each module an independent level while sharing handlers
- **Thread ID Tracking**: Optional thread ID in output
- **Syslog Integration**: Native syslog support (POSIX only)
- **Dynamic Configuration**: Runtime level/format changes
- **Performance Stats**: Built-in metrics and monitoring
- **NULL Safe**: Robust handling of NULL strings
- **Color Output**: Optional colored terminal output (ANSI codes)
- **Compile-Time Flags**: Selectively disable features to reduce binary size

## 📊 Performance

Measured numbers (2026-09, Linux x86_64, GCC -O2, `taskset -c 2` core pinning, sink /dev/null, message `"bench msg %d"`; medians of 5 runs):

| Mode | Throughput | Latency |
|------|-----------|---------|
| Sync (single-thread) | ~3,400,000 msg/s | ~0.29 us/msg |
| Sync (8 threads) | ~3,400,000 msg/s | — |
| Async (single-thread) | ~1,900,000 msg/s | ~0.53 us/msg |
| Async (8 threads) | ~3,200,000 msg/s | — |

Durability tiers (sync path, per-handler flush policy; see `log_handler_set_flush`):

| Tier | Throughput | Latency |
|------|-----------|---------|
| `buffered` (`LOG_FLUSH_NEVER`) | ~2,460,000 msg/s | ~0.41 us/msg |
| `flushed` (`LOG_FLUSH_EVERY`) | ~519,000 msg/s | ~1.92 us/msg |
| `fsynced` (`LOG_FLUSH_EVERY` + `log_handler_set_fsync`) | ~2,009 msg/s | ~497 us/msg |

Crash-loss promise per tier (child `_exit()` with no stdio flush; verified by
`tests/platform/test_flush.c`): `EVERY` loses nothing, `NEVER` may lose the
buffered tail, and `INTERVAL(n)` withholds at most `n` ms — its buffer is
flushed on the first write after the window elapses.

Producer tail latency under overload (async, 256-slot queue, 50 us slow sink,
`LOG_QUEUE_DROP`): **p50 0.04 us, p99 ~0.1 us**, worst scheduler spike < 0.1 ms
across runs. The caller never touches the slow sink, so its pauses stay bounded.

Queue-policy overload observations (async, 16-slot queue, 50 us slow sink):

| Policy | `queue_drops` | `queue_blocked` | `sync_writes` | Outcome |
|--------|--------------|-----------------|---------------|---------|
| `LOG_QUEUE_DROP` | yes | 0 | 0 | excess dropped, caller never blocks |
| `LOG_QUEUE_FALLBACK_SYNC` | 0 | 0 | > 0 | overflow written on the caller thread |
| `LOG_QUEUE_BLOCK` | 0 | > 0 | 0 | caller waits for queue space |

Reproduce with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
taskset -c 2 ./build/test_perf
```

Measurement notes:

- Numbers are **end-to-end delivery**: when the queue is full, the default `LOG_QUEUE_FALLBACK_SYNC` policy writes synchronously on the calling thread instead of dropping. The benchmark queue is sized so this path is not exercised on the hot run. That is a trade-off (worst-case caller latency for not losing messages), not a free win — see the policy table below. Historical versions silently dropped overflowing messages, so the old ~5,700,000 msg/s figure was fake throughput and is not comparable.
- The value of async mode is **decoupling** (producers are never blocked by a slow sink, with DROP/BLOCK/FALLBACK_SYNC full-queue policies), not throughput.

## 🧭 Scope, Non-Goals & Composition

This library is an **in-process emit layer**. It deliberately is not a transport or
reliability layer, and that is exactly what keeps it at two files, zero
dependencies, and small enough to embed anywhere.

**It does:** level filtering, text/JSON formatting, custom formatters and handlers,
async decoupling via a writer thread, file rotation, syslog, flush/fsync policies,
crash-safe marker output and flight-recorder tail dump, and a static zero-allocation mode.

**It does not (by design):** at-least-once delivery, on-disk spooling or offset
tracking, network/TLS transport, cross-process aggregation, or storage of large
binary payloads.

**Compose instead of expanding scope.** Hand bytes to an external collector
(Vector / Filebeat / Fluent Bit / syslog) that owns buffering, delivery, and
durability. The seam already exists: a custom handler runs on the async writer
thread, so a blocking send there never stalls your application.

```c
static void collector_handler(log_handle *ctx, log_event *ev) {
    (void)ctx;
    /* The formatted line is in ev->raw_msg; use it instead of re-expanding
       ev->fmt / ev->ap, which are not valid on the async writer thread. */
    const char *line = ev->raw_msg ? ev->raw_msg : "";
    (void)write(spool_fd, line, strlen(line));   /* or send() to a local collector */
    (void)write(spool_fd, "\n", 1);
}

log_set_async(ctx, true);
log_add_handler(ctx, collector_handler, NULL, LOG_INFO);
```

Keep large payloads out of the log stream: emit metadata plus a hash/path pointer
and store the payload separately. Oversized messages are **truncated** (never
split); `log_stats.truncated_count` counts them.

### Queue-full policy trade-offs

Async decoupling only holds while the bounded queue has room. Choose the policy
that matches your latency-vs-loss requirement (default is `LOG_QUEUE_FALLBACK_SYNC`):

| Policy | Blocks caller? | Can lose messages? | Worst-case caller latency | Fits |
|--------|----------------|--------------------|---------------------------|------|
| `LOG_QUEUE_FALLBACK_SYNC` | On overflow | No (written synchronously) | Sink latency, on the calling thread | Correctness over latency |
| `LOG_QUEUE_DROP` | Never | Yes (`queue_drops`) | Bounded | Best-effort, latency-critical paths |
| `LOG_QUEUE_BLOCK` | On overflow | No | Unbounded under sustained overload | Producers that tolerate backpressure |

No policy provides both a hard latency ceiling *and* guaranteed delivery — that
guarantee has to come from an external collector. Whichever you choose, monitor
`log_stats.queue_drops` / `queue_blocked` as your loss/backpressure signal.

### Latency-sensitive & high-value logging

For services where logging must not perturb the request path (trading systems,
security sensors, control planes), the same generic setup applies:

- Enable async and keep the sink fast so the queue never fills; if a sink can
  stall, do not rely on `FALLBACK_SYNC` in that path (see the table above).
- Keep `fsync` off the hot path (`log_handler_set_fsync(..., false)`); use
  `LOG_FLUSH_INTERVAL` for bounded durability latency.
- Ship bytes out of the process to a collector; treat the in-process queue as a
  latency smoother, not as durable storage.
- Watch `truncated_count` and `queue_drops` — both are silent-loss indicators.
- For small containers, set `LOG_RING_CAPACITY` and/or enable `LOG_STATIC_ALLOC`
  so the context fits your memory budget (see the footprint table below).

## 📦 Quick Start

### Basic Usage

```c
#include "log.h"

int main(void) {
    log_handle *ctx = log_create();

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

## 🛠️ Building

### CMake (Cross-Platform Recommended)

```bash
# Create build directory
mkdir build && cd build

# Configure with CMake
# Windows with MSVC
cmake ..

# Windows with MinGW-w64
cmake -G "MinGW Makefiles" ..

# Linux/macOS
cmake ..

# Build the project
cmake --build .

# Run tests
ctest --output-on-failure
```

### Compiler Options

```bash
# Enable color output (default: ON)
cmake -DENABLE_LOG_COLOR=ON ..

# Disable examples
cmake -DBUILD_EXAMPLES=OFF ..

# Disable tests
cmake -DBUILD_TESTS=OFF ..
```

### Makefile Build

The project provides a cross-platform Makefile that auto-detects the compiler and OS:

#### Linux / macOS (GCC / Clang)

```bash
# Build static library
make

# Build and run all tests
make run-tests

# Build and run unified test suite
make run-all

# Clean
make clean
```

#### Windows (MinGW-w64)

```bash
mingw32-make

mingw32-make run-tests

mingw32-make clean
```

#### Windows (MSVC)

Run from the "Developer Command Prompt" or after sourcing `vcvars64.bat`:

```bash
make CC=cl

make CC=cl run-tests

make CC=cl clean
```

#### Build Targets

| Target | Description |
|--------|-------------|
| `all` (default) | Static library `liblogc.a` (GCC) or `logc.lib` (MSVC) |
| `test_core` | Core functionality tests |
| `test_thread` | Thread safety tests |
| `test_platform` | Platform-specific tests |
| `test_stress` | Stress and edge-case tests |
| `test_perf` | Performance benchmarks |
| `test_all` | Unified test runner (all categories) |
| `run-tests` | Build and run all test categories |
| `run-all` | Build and run unified test suite |

#### Linking With Your Application

```bash
# Build the library first
make

# Then link your program
gcc -std=c17 -Wall -Wextra -I./src \
    your_app.c -L. -llogc -o your_app

# Or compile everything together (no Makefile needed)
gcc -std=c17 -Wall -Wextra -DLOG_USE_COLOR -I./src \
    src/log.c your_app.c -o your_app -lpthread
```

## 🌐 Cross-Platform Support

### Supported Platforms
- **Windows**: MSVC 2015+, MinGW-w64
- **Linux**: GCC 4.8+, Clang 3.4+
- **macOS**: Clang, GCC
- **Other POSIX**: FreeBSD, etc.

### Platform-Specific Features

| Feature | Windows | POSIX |
|---------|---------|-------|
| Threads | Win32 API | pthread |
| Atomic Ops | InterlockedXxx | C17 stdatomic |
| Syslog | ❌ Not available | ✅ Available |
| High-res Time | GetSystemTimePreciseAsFileTime | clock_gettime |

## 🎨 Color Output

Color output is enabled by default and uses ANSI escape codes:

```c
// Enable/disable color output at compile time
// Via CMake: -DENABLE_LOG_COLOR=ON/OFF
// Via compiler: -DLOG_USE_COLOR

// Color mapping:
// TRACE: Gray    (\x1b[90m)
// DEBUG: Cyan    (\x1b[36m)
// INFO:  Green   (\x1b[32m)
// WARN:  Yellow  (\x1b[33m)
// ERROR: Red     (\x1b[31m)
// FATAL: Bright Red (\x1b[91m)
```

## 🔧 Compile-Time Feature Flags

Disable optional features to reduce binary size (savings are measured .text deltas: `gcc -std=c17 -O2 -c src/log.c` + `size`, 2026-09; varies slightly by compiler/arch):

| Flag | Description | Savings |
|------|-------------|---------|
| `LOG_DISABLE_JSON` | Disable JSON formatting | ~4.7 KB |
| `LOG_DISABLE_SYSLOG` | Disable Syslog support | ~1.4 KB |
| `LOG_DISABLE_ASYNC` | Disable async logging | ~9.5 KB |
| `LOG_DISABLE_MPOOL` | Disable memory pool | ~1.8 KB |
| `LOG_DISABLE_RING_QUEUE` | Disable ring buffer queue | ~4.2 KB |
| `LOG_DISABLE_STATS` | Disable performance stats | ~2.0 KB |
| `LOG_DISABLE_FILE_OPS` | Disable file operations | ~2.2 KB |
| `LOG_DISABLE_THREAD_ID` | Disable thread ID | ~0.15 KB |
| `LOG_DISABLE_TS_CACHE` | Disable timestamp cache | ~0.6 KB |
| `LOG_DISABLE_CRASH_MODE` | Disable crash-safe mode | ~2.0 KB |
| `LOG_DISABLE_KV` | Disable key-value metadata | ~6.0 KB |
| `LOG_DISABLE_LIFECYCLE` | Disable fork/exit lifecycle safety | ~1.7 KB |
| `LOG_DISABLE_MEMORY_HANDLER` | Disable the in-memory flight recorder | ~2.3 KB |
| `LOG_DISABLE_FILTER` | Disable rate limiting & dedupe | ~4.4 KB |
| `LOG_DISABLE_NAMED` | Disable named loggers | ~1.6 KB |
| `LOG_MINIMAL` | Disable all optional features | ~31.9 KB |

### Static-mode footprint

With `LOG_STATIC_ALLOC` the ring and handler table are embedded in the context,
so its size is a compile-time choice via `LOG_RING_CAPACITY` (default 4096; must
be a power of two):

| Build | `sizeof(log_handle)` |
|-------|----------------------|
| `LOG_STATIC_ALLOC`, `LOG_RING_CAPACITY=4096` (default) | ~3.61 MiB |
| `LOG_STATIC_ALLOC`, `LOG_RING_CAPACITY=256` | ~247 KiB |
| `LOG_STATIC_ALLOC` + `LOG_MINIMAL`, `LOG_RING_CAPACITY=256` | ~3.8 KiB |

Pick the smallest capacity that absorbs your burst; the default favors throughput
over footprint. With KV enabled each ring slot also embeds `LOG_KV_INLINE_MAX`
(256) bytes of key-value storage — cost `256 × LOG_RING_CAPACITY`; disable it
with `LOG_DISABLE_KV` (e.g. `LOG_MINIMAL`) to reclaim it.

## 📋 Core Features

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
log_handle *ctx = log_create();

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
    log_handle *ctx = (log_handle*)arg;
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "Thread %lu: Message %d", pthread_self(), i);
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

**Output:**
```
2024-01-15T10:30:45.123 INFO [140234567890432] main.c:42: Thread 140234567890432: Message 0
2024-01-15T10:30:45.124 INFO [140234567890432] main.c:42: Thread 140234567890432: Message 1
```

### 5. Async Logging

```c
int main(void) {
    log_handle *ctx = log_create();

    // Enable async mode (ring buffer queue + dedicated writer thread)
    log_set_async(ctx, true);

    // Set queue full policy: FALLBACK_SYNC, DROP, or BLOCK
    log_set_queue_policy(ctx, LOG_QUEUE_FALLBACK_SYNC);

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
    printf("Total: %llu, Async writes: %llu, Queue drops: %llu\n",
           (unsigned long long)stats.total_count,
           (unsigned long long)stats.async_writes,
           (unsigned long long)stats.queue_drops);

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

### 6. Syslog Integration (POSIX Only)

```c
int main(void) {
    log_handle *ctx = log_create();

    // Add syslog handler (POSIX only)
    #ifdef LOG_PLATFORM_POSIX
    int idx = log_add_syslog_handler(ctx, "myapp", LOG_USER, LOG_INFO);
    log_enable_thread_id(ctx, idx, true);
    #endif

    log_ctx_info(ctx, "Application started");
    log_ctx_error(ctx, "Failed to connect to database");

    log_destroy(ctx);
    return 0;
}
```

### 7. Structured Key-Value Metadata

Attach typed fields to a log line. The JSON formatter emits them as top-level
fields; the text formatter appends them as a `key=value` suffix. The message is
emitted **literally** (no `printf` formatting), so it may safely contain `%`.

```c
log_ctx_info_kv(ctx, "user login",
                LOG_KV_STR("user", "alice"),
                LOG_KV_INT("id", 42),
                LOG_KV_DOUBLE("score", 1.5),
                LOG_KV_BOOL("mfa", true));
```

JSON output:

```json
{"time": "...", "level": "INFO", "file": "main.c", "line": 42, "message": "user login", "user": "alice", "id": 42, "score": 1.5, "mfa": true}
```

Text output:

```
2026-09-17T10:30:45.123 INFO  main.c:42: user login user=alice id=42 score=1.5 mfa=true
```

- Pairs live on the caller's stack (no allocation). At most `LOG_KV_MAX_PAIRS`
  (8) are encoded; extra pairs are dropped and counted in `truncated_count`.
- A `NULL` key skips that pair; an empty pair list degrades to a plain message.
- Works on both the sync and async paths; values are escaped for JSON.
- `log_info_kv(msg, ...)`, `log_ctx_error_kv(...)`, etc. exist for all levels.
- Disable with `LOG_DISABLE_KV` (or `LOG_MINIMAL`); the `*_kv` macros then
  degrade to plain message logging.

### 8. Process Lifecycle Safety (POSIX)

Async logging keeps a writer thread and a queue inside the process, which is
fragile across `fork()` and process exit. Two opt-in helpers cover it:

```c
log_set_async(ctx, true);
log_install_atfork(ctx);   /* call once during setup, before spawning threads */
log_install_atexit(ctx);
```

- **`log_install_atfork(ctx)`** installs `pthread_atfork` handlers. Around
  `fork()` the context is quiesced (all locks held); in the child the locks are
  reinitialized and async is downgraded to synchronous, so the child can keep
  logging without deadlocking on locks inherited from threads that no longer
  exist. A normal `exit()` in the child flushes its lines.
- **`log_install_atexit(ctx)`** registers an `atexit` handler that drains a
  still-async queue when the process exits via `exit()` / `return` from `main`.
- Both are idempotent per context and `log_destroy()` unregisters it (up to 8
  contexts).
- POSIX only: on Windows both return `-1` (no `fork`; call
  `log_set_async(ctx, false)` before exiting if you need a flush).
- Pending async entries at the instant of `fork()` are abandoned in the child
  rather than duplicated.

### 9. In-Memory Flight Recorder

A memory-only handler that retains the most recent N formatted lines with no
I/O, stays completely silent in normal operation, and is dumped when you need
to investigate or after a crash:

```c
int idx = log_add_memory_handler(ctx, 256, LOG_WARN);  /* keep last 256 >= WARN */
/* ... run ... */
log_dump_memory_handler(ctx, idx, stderr);             /* oldest -> newest */
```

- Each record is a fully rendered line (prefix + message + KV, including the
  trailing `\n`). Oversized lines are truncated to `LOG_MEMORY_LINE_MAX` and
  counted in `truncated_count`.
- `lines` is clamped to the compile-time cap `LOG_MEMORY_MAX_LINES`;
  `lines <= 0` returns `-1`.
- The dump is a consistent snapshot taken under the context read lock and the
  per-handler store lock, so it never tears or reorders against concurrent
  writers.
- Entries are POD (no pointers). This is wired to A2 crash-safe mode:
  `log_install_crash_handler` `write(2)`s the last N lines directly from the
  fatal-signal handler (after the `==== log: flight recorder tail ====` header).
- Storage is allocated per handler on `log_add_memory_handler()`; contexts that
  never use it pay nothing.
- Trim with `LOG_DISABLE_MEMORY_HANDLER` (or `LOG_MINIMAL`); available on both
  Windows and POSIX.

### 10. Rate Limiting & Duplicate Suppression

Keep a repeated error from flooding the log. Both rules are configured per
level and applied **before** enqueueing, so a storm never fills the async queue:

```c
log_set_rate_limit(ctx, LOG_ERROR, 10);  /* at most 10 errors per second */
log_set_dedupe(ctx, LOG_WARN, 1000);     /* collapse repeats within 1s */
/* ... */
log_flush_suppressed(ctx);               /* emit pending summaries now */
```

- Rate limiting uses a one-second fixed window per level; the first
  `max_per_sec` messages pass, the rest are suppressed until the window resets.
- Dedupe hashes the rendered message (plus KV fields) and suppresses repeats,
  emitting `last message repeated N times` when the window expires or a
  different message arrives.
- Suppressed messages are reported in `log_stats.suppressed_count`; each level
  keeps independent state. `log_destroy` flushes pending summaries.
- Trim with `LOG_DISABLE_FILTER` (or `LOG_MINIMAL`).

### 11. Named Loggers

Give each module its own level switch while sharing the same handlers:

```c
log_handle *net = log_get("net");
log_handle *db  = log_get("db");
log_named_set_level("net", LOG_DEBUG);   /* chatty module */
log_named_set_level("db",  LOG_ERROR);   /* quiet module */

log_ctx_debug(net, "connection accepted");   /* emitted */
log_ctx_warn(db, "slow query");              /* filtered */
```

- `log_get()` creates the logger on first use (once, even under concurrent
  calls) and returns an alias attached to the default context: handlers are
  shared, levels are independent. A named logger is **not** re-filtered by the
  default context's level.
- The registry is a fixed flat table of `LOG_NAMED_MAX` (16) slots; names are
  at most 15 bytes. Invalid/overlong names or a full registry return the
  default context and log a warning (never fail or crash).
- Aliases live until the default context is destroyed. Trim with
  `LOG_DISABLE_NAMED` (or `LOG_MINIMAL`).

## 🔧 API Reference

For complete API documentation, see [API.md](API.md).

### Core Functions

```c
log_handle* log_create(void);
void log_destroy(log_handle *ctx);
log_handle* log_default(void);
void log_log(log_handle *ctx, int level, const char *file, int line, const char *fmt, ...);
```

### Configuration

```c
void log_set_level(log_handle *ctx, int level);
void log_set_quiet(log_handle *ctx, bool enable);
void log_set_format(log_handle *ctx, log_FormatFn fn);
int log_set_async(log_handle *ctx, bool enable);
void log_set_queue_policy(log_handle *ctx, int policy);
void log_set_max_file_size(log_handle *ctx, size_t size);
void log_set_file_prefix(log_handle *ctx, const char *prefix);
void log_set_rate_limit(log_handle *ctx, int level, unsigned max_per_sec);
void log_set_dedupe(log_handle *ctx, int level, unsigned window_ms);
void log_flush_suppressed(log_handle *ctx);
log_handle* log_get(const char *name);
void log_named_set_level(const char *name, int level);
```

### Handler Management

```c
int log_add_handler(log_handle *ctx, log_LogFn fn, void *udata, int level);
int log_add_fp(log_handle *ctx, FILE *fp, int level);
int log_add_file(log_handle *ctx, const char *filename, int level);
int log_add_memory_handler(log_handle *ctx, int lines, int level);
void log_dump_memory_handler(log_handle *ctx, int handler_idx, FILE *out);
void log_remove_handler(log_handle *ctx, int idx);
void log_handler_set_level(log_handle *ctx, int handler_idx, int new_level);
```

## 📊 Performance Statistics

```c
typedef struct log_stats {
    uint64_t total_count;               // Total messages logged
    uint64_t level_counts[LOG_LEVELS]; // Count per level
    uint64_t queue_drops;              // Dropped messages (async)
    uint64_t queue_blocked;            // Blocked count (async)
    uint64_t rotation_count;           // File rotations
    double avg_queue_latency_ms;        // Mean async enqueue->dequeue latency (ms)
    uint64_t async_writes;             // Async write count
    uint64_t sync_writes;              // Sync write count
    uint64_t truncated_count;          // Messages / KV pairs dropped by size limits
    uint64_t suppressed_count;         // Messages dropped by rate limit / dedupe
} log_stats;
```

`log_get_stats` aggregates the per-thread counters of every thread that has
logged to the context, so the totals are process-wide regardless of which
thread calls it.

## 🔒 Thread Safety

All public APIs are thread-safe:

- **Reader-Writer Locks**: Protect configuration changes (pthread_rwlock / SRWLOCK)
- **Ring Buffer Queue**: For async logging — mutex + condvar protected, multi-producer single-consumer, batch-drained by the writer thread
- **Per-Thread Statistics**: Counters are written without contention to a per-thread slot; `log_get_stats` aggregates all registered threads' slots (up to 64 per context) when it takes a snapshot
- **Safe from Multiple Threads**: Can be called concurrently

## 📝 Examples

See [tests/example.c](tests/example.c) for comprehensive examples:

- Basic logging
- Level filtering
- JSON format output
- File rotation
- Async logging
- Thread ID display
- Custom formatter
- Custom handler (memory buffer)
- Performance statistics

## 🧪 Testing

The project includes 170 tests across 6 categories (actual counts as reported by each test runner; static_alloc only builds on Linux with GNU ld):

| Category | Tests | Description |
|----------|-------|-------------|
| core | 79 | Levels, handlers, format, null safety, stats, key-value metadata, boundary, OOB level, flight recorder, rate limit / dedupe, named loggers |
| thread | 15 | Multi-threaded sync/async, config races, stats aggregation, concurrent flight-recorder dump, filter and named-logger concurrency |
| platform | 32 | Syslog, rotation, unicode paths, flush policies, crash loss per durability tier, crash safety (incl. tail dump), fork/exit lifecycle |
| stress | 28 | Queue full, long messages, integrity, crash safety, resources |
| perf | 10 | Throughput, durability tiers, tail latency, and queue-policy overload matrix |
| static_alloc | 6 | Static zero-allocation mode (real counting via `--wrap=malloc`), incl. filter path |

### Run Tests

```bash
# Using CMake + CTest
cd build
cmake --build .
ctest --output-on-failure

# Using Makefile
make run-tests    # Run all categories separately
make run-all      # Run unified test suite
```

## 📄 License

MIT License - See [LICENSE](LICENSE) for details.

## 🙏 Credits

Based on [rxi/log.c](https://github.com/rxi/log.c) (Copyright 2020 rxi)

Enhanced with cross-platform support and additional features in 2026.

## 🤝 Contributing

Contributions are welcome! Please ensure:

1. Code follows C17 standard
2. All functions are documented
3. Tests pass
4. Thread safety is maintained
5. Cross-platform compatibility is preserved
