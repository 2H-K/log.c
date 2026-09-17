# 增强型 C 日志库 - 跨平台

一个简单、强大且线程安全的 C11 日志库，具备完整的跨平台支持。

![screenshot](https://cloud.githubusercontent.com/assets/3920290/23831970/a2415e96-0723-11e7-9886-f8f5d2de60fe.png)

## 🚀 特性

- **跨平台支持**: Windows (MSVC/MinGW-w64) & Linux/macOS (GCC/Clang)
- **线程安全**: 读写锁保护配置与并发访问
- **异步日志**: 互斥锁 + 条件变量保护的环形缓冲队列 + 专用写入线程（用于解耦而非吞吐）
- **崩溃安全**: `log_set_crash_safe` 逐行落盘；`log_install_crash_handler` 致命信号时写入标记行（POSIX）
- **生命周期安全**: `log_install_atfork` 在 `fork()` 后子进程重建锁并把异步降级为同步；`log_install_atexit` 退出时排空异步队列（POSIX）
- **持久化策略**: 按 handler 配置 NEVER / EVERY / INTERVAL flush 与独立 fsync 开关
- **静态零分配**: `-DLOG_STATIC_ALLOC` + `log_create_static`，热路径 0 次堆分配
- **日志轮转**: 按大小自动轮转文件（最多 5 个轮转文件）
- **结构化日志**: JSON 格式支持；类型化键值元数据（`LOG_KV_*`）输出为 JSON 顶级字段或 `key=value` 文本后缀
- **线程ID追踪**: 输出中可选显示线程ID
- **Syslog集成**: 原生 syslog 支持（仅 POSIX）
- **动态配置**: 运行时级别/格式更改
- **性能统计**: 内置指标和监控
- **NULL安全**: 健壮的 NULL 字符串处理
- **颜色输出**: 可选彩色终端输出（ANSI 代码）
- **编译时标志**: 可选择性禁用以减小二进制体积

## 📊 性能

实测数字（2026-09，Linux x86_64，GCC -O2，`taskset -c 2` 绑核，sink 为 /dev/null，消息 `"bench msg %d"`；5 次运行取中位数）：

| 模式 | 吞吐量 | 延迟 |
|------|--------|------|
| 同步（单线程） | ~3,400,000 msg/s | ~0.29 µs/msg |
| 同步（8 线程） | ~3,400,000 msg/s | — |
| 异步（单线程） | ~2,000,000 msg/s | ~0.50 µs/msg |
| 异步（8 线程） | ~3,300,000 msg/s | — |

复现命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
taskset -c 2 ./build/test_perf
```

口径说明：

- 数字为**端到端送达**口径：队列满时默认策略 `LOG_QUEUE_FALLBACK_SYNC` 在调用线程上同步写出、不丢消息。基准中队列开得足够大，热路径不会触发该分支。这是**取舍**（用最坏情况下的调用延迟换不丢消息），不是纯优点——见下方策略表。历史版本在队列满时静默丢弃溢出消息，旧文档的 ~5,700,000 msg/s 是丢弃消息后的假吞吐，不可比。
- 异步模式的价值是**解耦**（生产者不被慢 sink 阻塞、可设置 DROP/BLOCK/FALLBACK_SYNC 满载策略），不是吞吐。

## 🧭 范围、非目标与组合

本库是**进程内发射层**，刻意不做传输层或可靠性层——正是这一点让它保持双文件、零依赖、可嵌入任意进程。

**它做：** 级别过滤、文本/JSON 格式化、自定义格式化器与 handler、异步解耦（专用写入线程）、按大小轮转、syslog、flush/fsync 策略、崩溃安全标记输出、静态零分配模式。

**它不做（刻意）：** at-least-once 投递、磁盘 spool/offset 持久化、网络/TLS 传输、跨进程聚合、大块二进制 payload 存储。

**用组合代替扩张范围。** 把字节交给外部采集器（Vector / Filebeat / Fluent Bit / syslog），由它负责缓冲、投递与持久化。缝已经存在：自定义 handler 运行在异步写入线程上，因此在那里阻塞发送**不会**卡住你的应用。

```c
static void collector_handler(log_handle *ctx, log_event *ev) {
    (void)ctx;
    /* 已格式化的整行在 ev->raw_msg；不要重新展开 ev->fmt / ev->ap，
       它们在异步写入线程上不保证有效。 */
    const char *line = ev->raw_msg ? ev->raw_msg : "";
    (void)write(spool_fd, line, strlen(line));   /* 或 send() 到本地 collector */
    (void)write(spool_fd, "\n", 1);
}

log_set_async(ctx, true);
log_add_handler(ctx, collector_handler, NULL, LOG_INFO);
```

大 payload 不要进日志流：只发元数据 + 哈希/路径指针，payload 另存。超长消息会被**截断**（不会拆分），`log_stats.truncated_count` 会计数。

### 队列满策略的取舍

异步解耦只在有界队列未满时成立。按你的"延迟 vs 丢失"需求选策略（默认为 `LOG_QUEUE_FALLBACK_SYNC`）：

| 策略 | 阻塞调用方？ | 会丢消息？ | 最坏调用延迟 | 适用 |
|------|--------------|------------|--------------|------|
| `LOG_QUEUE_FALLBACK_SYNC` | 溢出时阻塞 | 否（同步写出） | sink 延迟，落在调用线程 | 正确性优先于延迟 |
| `LOG_QUEUE_DROP` | 从不阻塞 | 会（`queue_drops`） | 有界 | 尽力而为、延迟关键路径 |
| `LOG_QUEUE_BLOCK` | 溢出时阻塞 | 否 | 持续过载下无上界 | 能承受背压的生产者 |

**没有任何一种策略能同时给出"硬延迟上限"和"保证不丢"**——后者必须来自外部采集器。无论选哪种，都请把 `log_stats.queue_drops` / `queue_blocked` 当作丢失/背压信号来监控。

### 延迟敏感与高价值日志

对于"日志不得干扰请求路径"的服务（交易系统、安全传感器、控制面），同一套通用配置适用：

- 开启异步并保持 sink 快速，使队列不积压；若 sink 可能停顿时，不要在关键路径上依赖 `FALLBACK_SYNC`（见上表）。
- 让 `fsync` 离开热路径（`log_handler_set_fsync(..., false)`）；用 `LOG_FLUSH_INTERVAL` 获得有界的持久化延迟。
- 尽快把字节送出进程交给采集器；把进程内队列当作延迟平滑器，而**不是**持久存储。
- 监控 `truncated_count` 与 `queue_drops`——两者都是静默丢失的指标。
- 小容器场景设置 `LOG_RING_CAPACITY` 和/或启用 `LOG_STATIC_ALLOC`，让 context 适配内存预算（见下方 footprint 表）。

## 📦 快速开始

### 基础用法

```c
#include "log.h"

int main(void) {
    log_handle *ctx = log_create();

    log_ctx_trace(ctx, "详细调试信息");
    log_ctx_info(ctx, "应用程序已启动");
    log_ctx_error(ctx, "连接失败: %s", "超时");

    log_destroy(ctx);
    return 0;
}
```

### 使用默认日志器

```c
log_info("应用程序已启动");
log_error("错误: %s", strerror(errno));
```

## 🛠️ 构建

### CMake（推荐跨平台）

```bash
# 创建构建目录
mkdir build && cd build

# 使用 CMake 配置
# Windows 使用 MSVC
cmake ..

# Windows 使用 MinGW-w64
cmake -G "MinGW Makefiles" ..

# Linux/macOS
cmake ..

# 构建项目
cmake --build .

# 运行测试
ctest --output-on-failure
```

### 编译器选项

```bash
# 启用颜色输出（默认：开启）
cmake -DENABLE_LOG_COLOR=ON ..

# 禁用示例
cmake -DBUILD_EXAMPLES=OFF ..

# 禁用测试
cmake -DBUILD_TESTS=OFF ..
```

### Makefile 构建

项目提供跨平台的 Makefile，自动检测编译器和操作系统：

#### Linux / macOS（GCC / Clang）

```bash
# 编译静态库
make

# 构建并运行所有测试
make run-tests

# 构建并运行统一测试套件
make run-all

# 清理
make clean
```

#### Windows（MinGW-w64）

```bash
mingw32-make

mingw32-make run-tests

mingw32-make clean
```

#### Windows（MSVC）

需从"开发者命令提示符"或 `vcvars64.bat` 环境运行：

```bash
make CC=cl

make CC=cl run-tests

make CC=cl clean
```

#### 编译产出

| 目标 | 说明 |
|------|------|
| `all`（默认） | 编译静态库 `liblogc.a`（GCC）或 `logc.lib`（MSVC） |
| `test_core` | 核心功能测试 |
| `test_thread` | 线程安全测试 |
| `test_platform` | 平台特定测试 |
| `test_stress` | 压力和边界测试 |
| `test_perf` | 性能基准测试 |
| `test_all` | 统一测试运行器（所有类别） |
| `run-tests` | 构建并运行所有测试类别 |
| `run-all` | 构建并运行统一测试套件 |

#### 与应用程序链接

```bash
# 先编译库
make

# 然后链接你的程序
gcc -std=c11 -Wall -Wextra -I./src \
    your_app.c -L. -llogc -o your_app

# 或直接一起编译（无需 Makefile）
gcc -std=c11 -Wall -Wextra -DLOG_USE_COLOR -I./src \
    src/log.c your_app.c -o your_app -lpthread
```

## 🌐 跨平台支持

### 支持的平台
- **Windows**: MSVC 2015+, MinGW-w64
- **Linux**: GCC 4.8+, Clang 3.4+
- **macOS**: Clang, GCC
- **其他 POSIX**: FreeBSD 等

### 平台特定功能

| 功能 | Windows | POSIX |
|------|---------|-------|
| 线程 | Win32 API | pthread |
| 原子操作 | InterlockedXxx | C11 stdatomic |
| Syslog | ❌ 不可用 | ✅ 可用 |
| 高精度时间 | GetSystemTimePreciseAsFileTime | clock_gettime |

## 🎨 颜色输出

颜色输出默认启用，使用 ANSI 转义码：

```c
// 编译时启用/禁用颜色输出
// 通过 CMake: -DENABLE_LOG_COLOR=ON/OFF
// 通过编译器: -DLOG_USE_COLOR

// 颜色映射:
// TRACE: 灰色    (\x1b[90m)
// DEBUG: 青色    (\x1b[36m)
// INFO:  绿色    (\x1b[32m)
// WARN:  黄色    (\x1b[33m)
// ERROR: 红色    (\x1b[31m)
// FATAL: 亮红色 (\x1b[91m)
```

## 🔧 编译时功能标志

禁用可选功能以减小二进制体积（节省值为 .text 实测差值：`gcc -std=c11 -O2 -c src/log.c` 后 `size` 对比，2026-09；随编译器/架构略有浮动）：

| 标志 | 说明 | 节省 |
|------|------|------|
| `LOG_DISABLE_JSON` | 禁用 JSON 格式化 | ~4.7 KB |
| `LOG_DISABLE_SYSLOG` | 禁用 Syslog 支持 | ~1.3 KB |
| `LOG_DISABLE_ASYNC` | 禁用异步日志 | ~8.0 KB |
| `LOG_DISABLE_MPOOL` | 禁用内存池 | ~1.8 KB |
| `LOG_DISABLE_RING_QUEUE` | 禁用环形缓冲区队列 | ~3.7 KB |
| `LOG_DISABLE_STATS` | 禁用性能统计 | ~1.9 KB |
| `LOG_DISABLE_FILE_OPS` | 禁用文件操作 | ~2.2 KB |
| `LOG_DISABLE_THREAD_ID` | 禁用线程ID | ~0.15 KB |
| `LOG_DISABLE_TS_CACHE` | 禁用时间戳缓存 | ~0.6 KB |
| `LOG_DISABLE_CRASH_MODE` | 禁用崩溃安全模式 | ~1.3 KB |
| `LOG_DISABLE_KV` | 禁用键值元数据 | ~6.3 KB |
| `LOG_DISABLE_LIFECYCLE` | 禁用 fork/退出生命周期安全 | ~1.7 KB |
| `LOG_MINIMAL` | 禁用所有可选功能 | ~23.3 KB |

### 静态模式占用

启用 `LOG_STATIC_ALLOC` 时，环与 handler 表内嵌在 context 中，大小由编译期 `LOG_RING_CAPACITY` 决定（默认 4096；必须为 2 的幂）：

| 构建 | `sizeof(log_handle)` |
|------|----------------------|
| `LOG_STATIC_ALLOC`，`LOG_RING_CAPACITY=4096`（默认） | ~3.61 MiB |
| `LOG_STATIC_ALLOC`，`LOG_RING_CAPACITY=256` | ~246 KiB |
| `LOG_STATIC_ALLOC` + `LOG_MINIMAL`，`LOG_RING_CAPACITY=256` | ~4 KiB |

选择能吸收你突发量的最小容量；默认值偏向吞吐而非占用。启用 KV 时每个环槽还内嵌
`LOG_KV_INLINE_MAX`（256）字节的键值存储，开销为 `256 × LOG_RING_CAPACITY`；
可用 `LOG_DISABLE_KV`（如 `LOG_MINIMAL`）回收。

## 📋 核心功能

### 1. 日志级别

六个日志级别，从最详细到最简洁：

| 级别 | 描述 |
|------|------|
| LOG_TRACE | 详细调试信息 |
| LOG_DEBUG | 调试信息 |
| LOG_INFO | 一般信息性消息 |
| LOG_WARN | 警告消息 |
| LOG_ERROR | 错误条件 |
| LOG_FATAL | 严重错误 |

### 2. 文件日志与轮转

```c
log_handle *ctx = log_create();

// 配置轮转（最大 10MB，5 个轮转文件）
log_set_file_prefix(ctx, "app.log");
log_set_max_file_size(ctx, 10 * 1024 * 1024);

// 添加文件处理器
FILE *fp = fopen("app.log", "a");
int idx = log_add_fp(ctx, fp, LOG_INFO);

// 写入日志 - 超过大小时自动轮转
for (int i = 0; i < 10000; i++) {
    log_ctx_info(ctx, "消息 %d", i);
}

log_remove_handler(ctx, idx);
fclose(fp);
log_destroy(ctx);
```

### 3. JSON 格式输出

```c
log_handle *ctx = log_create();

// 设置 JSON 格式
log_enable_json_format(ctx);

// 添加文件处理器
FILE *fp = fopen("logs.json", "w");
log_add_fp(ctx, fp, LOG_INFO);

log_ctx_info(ctx, "用户登录: id=%d, name=%s", 123, "Alice");
log_ctx_error(ctx, "数据库错误: %s", "连接超时");

fclose(fp);
log_destroy(ctx);
```

**输出:**
```json
{"time": "2024-01-15T10:30:45.123", "level": "INFO", "file": "main.c", "line": 42, "message": "用户登录: id=123, name=Alice"}
{"time": "2024-01-15T10:30:45.124", "level": "ERROR", "file": "main.c", "line": 43, "message": "数据库错误: connection timeout"}
```

### 4. 线程安全日志

```c
#include <pthread.h>

void* worker_thread(void *arg) {
    log_handle *ctx = (log_handle*)arg;
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "线程 %lu: 消息 %d", pthread_self(), i);
    }
    return NULL;
}

int main(void) {
    log_handle *ctx = log_create();

    // 为文件处理器启用线程ID
    FILE *fp = fopen("thread.log", "w");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    log_enable_thread_id(ctx, idx, true);

    // 创建线程
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, worker_thread, ctx);
    }

    // 等待完成
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

**输出:**
```
2024-01-15T10:30:45.123 INFO [140234567890432] main.c:42: 线程 140234567890432: 消息 0
2024-01-15T10:30:45.124 INFO [140234567890432] main.c:42: 线程 140234567890432: 消息 1
```

### 5. 异步日志

```c
int main(void) {
    log_handle *ctx = log_create();

    // 启用异步模式（环形缓冲区队列 + 专用写入线程）
    log_set_async(ctx, true);

    // 设置队列满策略：FALLBACK_SYNC、DROP 或 BLOCK
    log_set_queue_policy(ctx, LOG_QUEUE_FALLBACK_SYNC);

    FILE *fp = fopen("async.log", "w");
    log_add_fp(ctx, fp, LOG_INFO);

    // 快速写入大量消息 - 不会阻塞
    for (int i = 0; i < 10000; i++) {
        log_ctx_info(ctx, "异步消息 %d", i);
    }

    // 禁用并等待刷新
    log_set_async(ctx, false);

    // 检查性能统计
    log_stats stats;
    log_get_stats(ctx, &stats);
    printf("总计: %llu, 异步写入: %llu, 队列丢弃: %llu\n",
           (unsigned long long)stats.total_count,
           (unsigned long long)stats.async_writes,
           (unsigned long long)stats.queue_drops);

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

### 6. Syslog 集成（仅 POSIX）

```c
int main(void) {
    log_handle *ctx = log_create();

    // 添加 syslog 处理器（仅 POSIX）
    #ifdef LOG_PLATFORM_POSIX
    int idx = log_add_syslog_handler(ctx, "myapp", LOG_USER, LOG_INFO);
    log_enable_thread_id(ctx, idx, true);
    #endif

    log_ctx_info(ctx, "应用程序已启动");
    log_ctx_error(ctx, "连接数据库失败");

    log_destroy(ctx);
    return 0;
}
```

### 7. 结构化键值元数据

给日志行附加类型化字段：JSON 格式化器输出为顶级字段，文本格式化器追加为
`key=value` 后缀。消息按**字面量**输出（不做 `printf` 格式化），因此可以安全包含 `%`。

```c
log_ctx_info_kv(ctx, "user login",
                LOG_KV_STR("user", "alice"),
                LOG_KV_INT("id", 42),
                LOG_KV_DOUBLE("score", 1.5),
                LOG_KV_BOOL("mfa", true));
```

JSON 输出：

```json
{"time": "...", "level": "INFO", "file": "main.c", "line": 42, "message": "user login", "user": "alice", "id": 42, "score": 1.5, "mfa": true}
```

文本输出：

```
2026-09-17T10:30:45.123 INFO  main.c:42: user login user=alice id=42 score=1.5 mfa=true
```

- 键值对在调用者栈上组装（零分配）。最多编码 `LOG_KV_MAX_PAIRS`（8）对；超出部分丢弃并计入 `truncated_count`。
- `key` 为 `NULL` 时跳过该对；空列表退化为普通消息。
- 同步与异步路径均支持；字符串值会做 JSON 转义。
- 各日志级别都有对应宏：`log_info_kv(msg, ...)`、`log_ctx_error_kv(...)` 等。
- 用 `LOG_DISABLE_KV`（或 `LOG_MINIMAL`）裁剪；此时 `*_kv` 宏退化为普通消息日志。

### 8. 进程生命周期安全（POSIX）

异步日志在进程内保留写入线程与队列，在 `fork()` 与进程退出时很脆弱。提供两个可选辅助函数：

```c
log_set_async(ctx, true);
log_install_atfork(ctx);   /* 初始化阶段调用一次，须在创建线程之前 */
log_install_atexit(ctx);
```

- **`log_install_atfork(ctx)`** 安装 `pthread_atfork` 处理器。`fork()` 前后将 context 静默（持有全部锁）；子进程中重建锁并把异步降级为同步，因此子进程可继续写日志，不会因继承自已消失线程的锁而死锁。子进程正常 `exit()` 会刷出其日志行。
- **`log_install_atexit(ctx)`** 注册 `atexit` 处理器，在进程经 `exit()` / `main` 返回退出时排空仍处于异步状态的队列。
- 两者对同一 context 幂等；`log_destroy()` 会注销（最多 8 个 context）。
- 仅 POSIX：Windows 下两者返回 `-1`（没有 `fork`；需要 flush 请在退出前调用 `log_set_async(ctx, false)`）。
- 注意：`fork()` 瞬间仍在队列中的异步条目在子进程中被放弃，不会重复写出。

## 🔧 API 参考

完整的 API 文档请参阅 [API.md](API.md)。

### 核心函数

```c
log_handle* log_create(void);
void log_destroy(log_handle *ctx);
log_handle* log_default(void);
void log_log(log_handle *ctx, int level, const char *file, int line, const char *fmt, ...);
```

### 配置

```c
void log_set_level(log_handle *ctx, int level);
void log_set_quiet(log_handle *ctx, bool enable);
void log_set_format(log_handle *ctx, log_FormatFn fn);
int log_set_async(log_handle *ctx, bool enable);
void log_set_queue_policy(log_handle *ctx, int policy);
void log_set_max_file_size(log_handle *ctx, size_t size);
void log_set_file_prefix(log_handle *ctx, const char *prefix);
```

### 处理器管理

```c
int log_add_handler(log_handle *ctx, log_LogFn fn, void *udata, int level);
int log_add_fp(log_handle *ctx, FILE *fp, int level);
int log_add_file(log_handle *ctx, const char *filename, int level);
void log_remove_handler(log_handle *ctx, int idx);
void log_handler_set_level(log_handle *ctx, int handler_idx, int new_level);
```

## 📊 性能统计

```c
typedef struct log_stats {
    uint64_t total_count;               // 总日志消息数
    uint64_t level_counts[LOG_LEVELS]; // 各级别计数
    uint64_t queue_drops;              // 丢弃的消息数（异步）
    uint64_t queue_blocked;            // 阻塞次数（异步）
    uint64_t rotation_count;           // 文件轮转次数
    double avg_queue_latency_ms;        // 异步入队→出队的平均延迟（毫秒）
    uint64_t async_writes;             // 异步写入计数
    uint64_t sync_writes;              // 同步写入计数
} log_stats;
```

`log_get_stats` 会聚合所有曾向该 context 写入日志的线程的计数器，因此无论哪个
线程调用，得到的都是全进程口径的统计。

## 🔒 线程安全

所有公共 API 都是线程安全的：

- **读写锁**: 保护配置更改（pthread_rwlock / SRWLOCK）
- **环形缓冲队列**: 用于异步日志——互斥锁 + 条件变量保护，多生产者单消费者；写入线程批量出队
- **每线程统计**: 计数器无竞争地写入各自的线程槽；`log_get_stats` 快照时聚合所有已注册线程的槽（每 context 上限 64 个）
- **多线程安全**: 可以并发调用

## 📝 示例

查看 [tests/example.c](tests/example.c) 获取全面示例：

- 基础日志
- 级别过滤
- JSON 格式输出
- 文件轮转
- 异步日志
- 线程ID显示
- 自定义格式化器
- 自定义处理器（内存缓冲区）
- 性能统计

## 🧪 测试

项目包含 137 项测试，分为 6 个类别（以各 runner 输出的实测数为准；static_alloc 仅在 Linux + GNU ld 下构建）：

| 类别 | 测试数 | 说明 |
|------|--------|------|
| core | 59 | 级别、处理器、格式、NULL安全、统计、键值元数据、边界、越界级别 |
| thread | 10 | 多线程同步/异步、配置竞态、统计聚合 |
| platform | 28 | Syslog、轮转、Unicode路径、flush策略、崩溃安全、fork/退出生命周期 |
| stress | 28 | 队列满、长消息、完整性、崩溃安全、资源 |
| perf | 7 | 吞吐量和延迟基准测试 |
| static_alloc | 5 | 静态零分配模式（`--wrap=malloc` 真实计数） |

### 运行测试

```bash
# 使用 CMake + CTest
cd build
cmake --build .
ctest --output-on-failure

# 使用 Makefile
make run-tests    # 分别运行所有类别
make run-all      # 运行统一测试套件
```

## 🔄 从 2.x 迁移

3.0.0 把不透明句柄类型从 `log` 改名为 `log_handle`：

```c
/* 2.x 之前的写法 */
log *ctx = log_create();

/* 3.0 的写法 */
log_handle *ctx = log_create();
```

只改了类型名。所有 `log_*` 函数和 `LOG_*` 宏名称不变，所以对独立的 `log` 令牌做一次文本替换即可：

```sh
sed -i 's/\blog\b/log_handle/g' your_source.c
```

改名原因：`log` 与 C 数学函数 `log()` 共用 ordinary identifier 命名空间。MSVC 在 `/Oi`（由 `/O2` 隐含）下会启用该内置函数，而 `<math.h>`/`<cmath>` 在任何平台都会暴露它，因此名为 `log` 的公共类型无法与它们共存。

## 📄 许可证

MIT 许可证 - 详情请参阅 [LICENSE](LICENSE)。

## 📈 版本历史

- **3.0.0** (2026): 破坏性变更 —— 不透明句柄类型由 `log` 改名为 `log_handle`
  - `log` 与数学内置函数 `log` 冲突（MSVC 经 `/O2` 隐含的 `/Oi`、`<math.h>`、C++ `<cmath>`）
  - 所有 `log_*` 函数与 `LOG_*` 宏不变，仅类型名变更
  - 新增 GitHub Actions CI（Linux GCC/Clang、macOS Clang、Windows MSVC/MinGW）
  - 新增 MSVC `/O2 /Oi /WX` 公共头回归门禁

- **2.0.1** (2026): 代码质量改进
  - 清理编译警告（未使用变量、格式字符串）
  - 移除死代码（arena 分配器、未使用函数）
  - 添加全面的 example.c 演示所有功能
  - 修复格式字符串可移植性（uint64_t 打印）

- **2.0.0** (2026): 重大增强
  - 添加跨平台支持（Windows/Linux/macOS）
  - 添加 CMake 构建系统
  - 添加彩色输出支持
  - 增强异步日志（环形缓冲队列 + 专用写入线程）
  - 添加 JSON 格式支持
  - 添加线程ID追踪
  - 添加 syslog 集成
  - 添加性能统计
  - 添加动态配置 API
  - 增强线程安全（读写锁）
  - 添加 NULL 字符串安全
  - 添加编译时功能标志

- **1.0.0** (2020): rxi 的原始实现

## 🙏 致谢

基于 [rxi/log.c](https://github.com/rxi/log.c)（Copyright 2020 rxi）

2026 年增强跨平台支持和额外功能。

## 🤝 贡献

欢迎贡献！请确保：

1. 代码遵循 C11 标准
2. 所有函数都有文档
3. 测试通过
4. 保持线程安全
5. 保持跨平台兼容性
