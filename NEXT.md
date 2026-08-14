# NEXT.md — 下一步开发计划（可执行版）

> 上一版写成了架构论文，这版砍掉抽象术语，具体到「改哪个函数、改成什么、怎么验收」。
> 项目就 `src/log.c` + `src/log.h` 两个文件，所以计划按**函数**拆解，不按"组件"拆解。
> 目标：让 README 里每个示例**真实可跑**，修掉已知 bug，然后收尾。
>
> **进度：2.x 代码改动已全部完成并通过测试（17 项全绿，gcc + MSVC 零警告）。**

---

## 0. 病根：三个 bug 让 README 一半功能是死的

先看事实（都已在当前代码里核实）：

1. **异步是坏的**。`queue_pop`（`log.c:435`）返回 `head`（哑结点）而不是 `head->next`（数据），
   所以第一次 pop 拿到 `message=NULL` 的空结点；且 off-by-one 会丢最后一条；
   `log_set_async`（`log.c:859`）先建线程**后**置 `async_running=true`，线程可能在置位前读到 false 直接退出。
   实测：`set_async(true)` 后连打 3 条，一条都不输出。

2. **JSON/自定义格式是死的**。`format_fn` 字段只被赋值（`log.c:855/1168/1183`），**从未被任何 handler 调用**；
   `json_handler` 代码完整但从未注册；`log_enable_json_format` 只改 `format_mode`，该字段从未被读。
   更严重：`log_format_json` 返回 `const char*`（`log.h:388`），而 `log_FormatFn` 是返回 `int`（`log.h:166`），
   README 的 `log_set_format(ctx, log_format_json)` **根本编译不过**。

3. **异步二次解析**。生产端已 `vsnprintf` 出完整消息，消费端又把它当 `ev.fmt` 喂给 `vfprintf`，
   写 `log_info("100% done")` 在异步模式下是 UB。

已经修好的（这版不用再动）：路径穿越 `is_path_safe`、`queue_destroy` 哑结点释放、`strdup` 检查。
还没修的要修：realloc 覆盖原指针（Bug 2）、mpool 丢 entry（Bug 4）、TOCTOU（Bug 7）、`log_get_perf_stats` 把 `queue_drops` 误填成 `mpool.allocated`。

---

## 1. 三条设计决策（务实版，各一句理由）

**D1：异步队列改成「互斥锁 + 条件变量」的阻塞队列，放弃无锁。**
理由：当前无锁实现有 3 个 bug，无锁 SPSC 正确性极难保证；整个 logger 本来就挂着 rwlock，
队列再多一把 mutex 代价可忽略。正确性 > 无锁炫技。README 里 "lock-free" 措辞降级为 "异步"。

**D2：消息只 `vsnprintf` 一次，所有 handler 消费同一个纯字符串 `msg`。**
理由：根治二次解析 UB，也消灭 `format_text`/`stdout_handler`/`file_handler_internal`/`json_handler` 四处的重复拼接。

**D3：自定义 formatter 的语义定为「前缀 formatter」，JSON 走独立的 `log_enable_json_format` 开关。**
理由：README 的自定义示例返回的就是前缀（`[LEVEL] file:line - `）；JSON 是整行结构，不该塞进前缀语义。
`log_format_json` 签名改为返回 `int` 与 `log_FormatFn` 一致，避免编译不过。

---

## 2. 具体改动清单（按函数）

> ✅ = 已完成并通过测试

### 2.1 异步队列重写（D1）—— ✅ 完成

把 `log_queue` 结构（`log.h:254`）改成：

```c
typedef struct log_queue {
  log_queue_entry *head;
  log_queue_entry *tail;
  size_t size;
  size_t max_size;
  bool closed;
#if LOG_PLATFORM_POSIX
  pthread_mutex_t mtx;
  pthread_cond_t  cond;
#else
  CRITICAL_SECTION mtx;
  CONDITION_VARIABLE cond;
#endif
} log_queue;
```

四个函数重写为带锁版本：

- `queue_init`：初始化 mutex/cond，建一个哑结点 head=tail。
- `queue_push(q, entry)`：lock → 若 `size >= max_size` 丢弃（返回 false）→ 挂尾、`size++` → signal → unlock。
- `queue_pop(q)`：lock → 若空则 `cond_wait`（并检查 `closed`，收到关闭信号返回 NULL）→ 取 `head->next` 的数据、回收旧 `head` 为哑结点、`size--` → unlock。**不再返回哑结点，不再丢尾。**
- `queue_destroy`：lock → 遍历 free 所有结点 → destroy mutex/cond。

新增 `queue_shutdown(q)`：置 `closed=true`、broadcast，让消费线程退出（配合 D4 的关闭流程）。

**验收**：`set_async(true)` 后打 N 条 → `set_async(false)` → 输出恰好 N 条，顺序不变，无泄漏。

### 2.2 统一消息格式化（D2）—— ✅ 完成（`format_message` + `format_prefix` helper）

在 `log.c` 加一个静态函数，所有 text 类 handler 共用：

```c
/* 把 ev 的 fmt+ap 一次性格式化成纯消息字符串，返回 malloc 出来的 buf（调用方 free） */
static char* format_message(log_event *ev);
```

- `stdout_handler`（`log.c:540`）：改成 `char *msg = format_message(ev); fprintf("%s ... %s\n", ..., msg); free(msg);`，
  删掉 `vfprintf`。
- `file_handler_internal`（`log.c:580`）：同上，改用 `msg` 拼行写文件。
- `file_handler_wrapper`（`log.c:621`）：fallback 分支同样改用 `msg`。

这样同步和异步共用同一个 `format_message`，消息永远只格式化一次。

### 2.3 异步路径改造 —— ✅ 完成（entry 存纯消息 `msg` + `raw_msg` 避免二次解析）

`log_queue_entry`（`log.h:212`）**去掉 `message` 存完整行**，改为存元数据 + 纯消息：

```c
typedef struct log_queue_entry {
  char *msg;        /* format_message() 的结果，纯消息，无前缀 */
  char *file;
  int line;
  int level;
  double timestamp;
  struct log_queue_entry *next;
} log_queue_entry;
```

- `queue_entry_create`（`log.c:300`）：改成 `entry->msg = format_message(ev)` + 拷 file/line/level/timestamp。
  **顺手修 Bug 2**：`realloc` 先存旧指针，失败则保留旧值、回退 entry 并返回 NULL，不覆盖、不泄漏。
  **顺手修 Bug 4**：mpool 模式下 entry 校验失败也要 `mpool_free` 回池，不能直接丢。
- `async_writer_thread`（`log.c:688`）：改成遍历 handlers，用 `entry->msg` 拼行输出（**用 `%s`，绝不再当格式串**）：

```c
/* text 类 handler */
fprintf(fp, "%s %-5s %s:%d: %s\n", time_buf, level_strings[entry->level],
        entry->file, entry->line, entry->msg);
/* json 类 handler：对 entry->msg 转义后拼 JSON */
```

**验收**：异步模式下 `log_info("100% done")` 输出原样，不崩溃。

### 2.4 修 `log_set_async` 启动竞态 —— ✅ 完成（先置位再建线程 + shutdown/reopen）

改成：**先置 `async_running=true`，再建线程**（POSIX 用 `pthread_create` 前先置位；
Windows 同样先置位再 `CreateThread`）。关闭路径：先置 `async_running=false` + `queue_shutdown`，
再 `LOG_THREAD_JOIN`。删掉 `ctx->async_enabled` 的裸读，全部纳入锁。

**验收**：`set_async(true)` 紧跟第一条 `log_info` 不丢。

### 2.5 接线 JSON（D3）—— ✅ 完成（`log_format_json` 返回 int；`kind` 字段 + 真切换）

- **改 `log_format_json` 签名**（`log.c:1125`、`log.h:388`）为返回 `int`（写入字节数），与 `log_FormatFn` 一致。
- **`log_handler` 加 `int kind` 字段**（`log.h:288`），取值 `HANDLER_STDOUT / HANDLER_FILE / HANDLER_SYSLOG / HANDLER_CUSTOM`。
  `log_create` 的 stderr → `STDOUT`；`log_add_fp` → `FILE`；`log_add_syslog_handler` → `SYSLOG`；`log_add_handler` → `CUSTOM`。
- **`log_enable_json_format`（`log.c:1199`）真正生效**：遍历 handlers，把 `STDOUT`/`FILE` 类 handler 的 `fn` 换成 `json_handler`；
  **`log_enable_text_format`（`log.c:1193`）按 `kind` 恢复** `stdout_handler` / `file_handler_wrapper`。
- 删掉 `json_handler_wrapper`（`log.c:1310`）死代码。

**验收**：`log_enable_json_format(ctx)` 后 stderr 和文件都输出合法 JSON；`log_enable_text_format` 能切回文本。

### 2.6 自定义 formatter 生效（D3）—— ✅ 完成（`format_prefix` 调用 `ctx->format_fn`，删 `format_mode`）

- `log_set_format`（`log.c:853`）保持不变（存 `ctx->format_fn`）。
- 在 `stdout_handler` 和 `file_handler_internal` 里，拼行前先调 `ctx->format_fn(ctx, ev, prefix_buf, sizeof)` 得到前缀，
  若返回值 > 0 用自定义前缀替换默认前缀；`format_fn` 为空或返回 ≤0 时回退默认。消息仍用 `%s` 追加 `msg`。
- 删除 `format_mode` 字段（`log.h:188`、`log.c:757`）——它只增困惑，没人读。

**验收**：API.md 里的 `custom_format` 示例能改前缀；`log_set_format` 传 NULL 恢复默认。

### 2.7 修剩余安全项 —— ✅ 完成（TOCTOU 用 `ctx->mutex` 串行化；`queue_drops` 修正）

- **Bug 7 TOCTOU**：`log_enable_mpool`（`log.c:952`）/`log_enable_ts_cache`（`log.c:972`）——
  把 `async_enabled` 的读取与 `log_set_async` 的调用整体放进写锁（先在锁内读 `was_async`，
  需要重启异步时在锁内完成 stop/start 或明确临时解锁序列，保证无窗口）。
- **`log_get_perf_stats`（`log.c:988`）**：删掉 `stats->queue_drops = ctx->mpool.allocated` 这行，
  改为真实复制 `ctx->stats.queue_drops`。
- 全文扫一遍 `realloc`/`strdup`/`malloc`，统一「申请 → 校验 → 失败回滚」。

---

## 3. 测试（补一个零依赖单测文件）—— ✅ 完成

新增 `test/test_suite.c`（17 个用例，全绿），手写 assert 宏，`main` 返回非零即失败。
已接入 CMake（`BUILD_TESTS` + CTest `test_suite`）和 Makefile（`make test_suite`）。

覆盖用例（每条对应 README 一节）：

| # | 用例 | 对应功能 |
|---|------|---------|
| 1 | 六级别过滤、quiet、per-handler 级别 | 级别/动态配置 |
| 2 | 文本格式行内容精确匹配（时间/级别/file:line/消息） | 基础 |
| 3 | `log_enable_json_format` 输出合法 JSON（含引号/换行转义） | JSON |
| 4 | 自定义 `log_set_format` 前缀生效 | 自定义格式 |
| 5 | 文件轮转：写超阈值触发 `.1`~`.5`，`log_rotate` 手动轮转 | 轮转 |
| 6 | 异步：N 条消息 == N 条输出、顺序一致、队列满丢弃计数正确 | 异步 |
| 7 | 异步下 `log_info("100%% done")` 原样输出 | 二次解析回归 |
| 8 | 多线程并发写（8 线程 × 1 万条）无崩溃、总条数正确 | 线程安全 |
| 9 | NULL 字符串、超长消息（>512B）、`%` 特殊字符 | NULL 安全 |
| 10 | 路径穿越：`log_set_file_prefix("../evil")` 被拒绝 | 安全回归 |

**验收**：`ctest` 全绿；`test_suite` 在 Windows(MinGW+MSVC) 和 Linux 都通过。

---

## 4. 收尾清单

- [x] `test/test_suite.c` 已补（17 项全绿），接入 CMake + Makefile。
- [x] `-Wall -Wextra -Wpedantic`（MSVC `/W4`）**零警告**：已清掉 `json_handler_wrapper`、`format_timestamp_cached`、`thread_buf`、`log_sleep_ms` 四个 unused，gcc 与 MSVC 均零警告。
- [x] `src/example.c`、`src/performance_test.c` 已补（编译运行验证通过）。
- [x] README / README_cn / API.md 与实现逐条对齐（"lock-free"→"异步"，JSON 用 `log_enable_json_format`，`log_format_json` 签名改 `int`）。
- [x] AUDIT.MD 已加「修复状态汇总表」，7 项全部标已修复并附验证方式。
- [x] ASan 在本机 MinGW 无 `libasan`，改用 `test_bug` 手动泄漏测试：50000 次 create/destroy 泄漏从 5.8MB 降至 44KB（噪声级），未再触发 heap-corruption（`0xC0000374` 复现已消失）。
- [ ] 打 tag，LICENSE 归属核对（保留 rxi 版权）。

---

## 5. 做完的判据（就这三条，满足即收手）

1. 异步不丢消息、消息不二次解析、JSON/自定义格式真实可用；
2. `ctest` 全绿、零编译警告、无泄漏（sanitizer 零报告）；
3. README 每个示例不改一行就能编译运行且行为正确。

超出这三条的（KV 结构化字段、环形缓冲取证、域过滤、FATAL 钩子、Windows Event Log）**一律不做**，
那是另一个项目的量级，这个库到「正确 + 安全 + 文档兑现」就该停。
