# 增强型 C 日志库 - 跨平台

一个简单、强大且线程安全的 C17 日志库，具备完整的跨平台支持。

![screenshot](https://cloud.githubusercontent.com/assets/3920290/23831970/a2415e96-0723-11e7-9886-f8f5d2de60fe.png)

## 🚀 特性

- **跨平台支持**: Windows (MSVC/MinGW-w64) & Linux/macOS (GCC/Clang)
- **线程安全**: 读写锁保护并发访问
- **异步日志**: 无锁队列实现非阻塞写入
- **日志轮转**: 按大小自动轮转文件
- **结构化日志**: JSON 格式支持
- **线程ID追踪**: 输出中可选显示线程ID
- **Syslog集成**: 原生 syslog 支持（仅 POSIX）
- **动态配置**: 运行时级别/格式更改
- **性能统计**: 内置指标和监控
- **NULL安全**: 健壮的 NULL 字符串处理
- **颜色输出**: 可选彩色终端输出（ANSI 代码）

## 📦 快速开始

### 基础用法

```c
#include "log.h"

int main(void) {
    log *ctx = log_create();

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

# 编译测试程序
make test_fixes

# 清理
make clean
```

#### Windows（MinGW-w64）

```bash
# MinGW 下直接使用 GCC 工具链
mingw32-make

mingw32-make test_fixes

mingw32-make clean
```

#### Windows（MSVC）

需从"开发者命令提示符"或 `vcvars64.bat` 环境运行：

```bash
# 指定 CC=cl 以启用 MSVC 编译路径
make CC=cl

make CC=cl test_fixes

make CC=cl clean
```

#### 编译产出

| 目标 | 说明 |
|------|------|
| `all`（默认） | 编译静态库 `liblogc.a`（GCC）或 `logc.lib`（MSVC） |
| `test_fixes` | 编译测试程序 `test_fixes.exe` |

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
log *ctx = log_create();

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
log *ctx = log_create();

// 设置 JSON 格式
log_set_format(ctx, log_format_json);

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
    log *ctx = (log*)arg;
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "线程 %lu: 消息 %d", pthread_self(), i);
    }
    return NULL;
}

int main(void) {
    log *ctx = log_create();

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
    log *ctx = log_create();

    // 启用异步模式（无锁队列）
    log_set_async(ctx, true);

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
    printf("总计: %lu, 异步写入: %lu, 队列丢弃: %lu\n",
           stats.total_count, stats.async_writes, stats.queue_drops);

    fclose(fp);
    log_destroy(ctx);
    return 0;
}
```

### 6. Syslog 集成（仅 POSIX）

```c
int main(void) {
    log *ctx = log_create();

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

## 🔧 API 参考

完整的 API 文档请参阅 [API.md](API.md)。

### 核心函数

```c
log* log_create(void);
void log_destroy(log *ctx);
log* log_default(void);
void log_log(log *ctx, int level, const char *file, int line, const char *fmt, ...);
```

### 配置

```c
void log_set_level(log *ctx, int level);
void log_set_quiet(log *ctx, bool enable);
void log_set_format(log *ctx, log_FormatFn fn);
int log_set_async(log *ctx, bool enable);
void log_set_max_file_size(log *ctx, size_t size);
void log_set_file_prefix(log *ctx, const char *prefix);
```

### 处理器管理

```c
int log_add_handler(log *ctx, log_LogFn fn, void *udata, int level);
int log_add_fp(log *ctx, FILE *fp, int level);
void log_remove_handler(log *ctx, int idx);
void log_handler_set_level(log *ctx, int handler_idx, int new_level);
void log_handler_set_formatter(log *ctx, int handler_idx, log_FormatFn new_fn);
```

## 📊 性能统计

```c
typedef struct log_stats {
    uint64_t total_count;               // 总日志消息数
    uint64_t level_counts[LOG_LEVELS]; // 各级别计数
    uint64_t queue_drops;              // 丢弃的消息数（异步）
    uint64_t rotation_count;           // 文件轮转次数
    double avg_queue_latency_ms;        // 平均异步延迟
    uint64_t async_writes;             // 异步写入计数
    uint64_t sync_writes;              // 同步写入计数
} log_stats;
```

## 🔒 线程安全

所有公共 API 都是线程安全的：

- **读写锁**: 保护配置更改
- **无锁队列**: 用于异步模式（SPSC）
- **原子操作**: 用于统计信息
- **多线程安全**: 可以并发调用

## 📝 示例

查看 [src/example.c](src/example.c) 获取全面示例：

- 基础日志
- 级别过滤
- JSON 格式输出
- 文件轮转
- 异步日志
- 线程安全
- 动态处理器配置
- 混合格式化
- 性能统计

## 🧪 测试

运行测试套件：

```bash
cd build
cmake --build . --target test_log
./test_log
```

## 📄 许可证

MIT 许可证 - 详情请参阅 [LICENSE](LICENSE)。

## 📈 版本历史

- **2.0.0** (2026): 重大增强
  - 添加跨平台支持（Windows/Linux/macOS）
  - 添加 CMake 构建系统
  - 添加彩色输出支持
  - 增强异步日志（无锁队列）
  - 添加 JSON 格式支持
  - 添加线程ID追踪
  - 添加 syslog 集成
  - 添加性能统计
  - 添加动态配置 API
  - 增强线程安全（读写锁）
  - 添加 NULL 字符串安全

- **1.0.0** (2020): rxi 的原始实现

## 🙏 致谢

基于 [rxi/log.c](https://github.com/rxi/log.c)（Copyright 2020 rxi）

2026 年增强跨平台支持和额外功能。

## 🤝 贡献

欢迎贡献！请确保：

1. 代码遵循 C17 标准
2. 所有函数都有文档
3. 测试通过
4. 保持线程安全
5. 保持跨平台兼容性