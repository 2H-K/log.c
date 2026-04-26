# 安全漏洞分析报告

根据对 log.c 项目的分析，发现了以下安全漏洞：

## 1. 路径穿越漏洞

```json
{
  "分级": "已确认",
  "漏洞类型": "路径穿越",
  "漏洞位置": "log.c:866-873 (log_set_file_prefix)",
  "置信度": 95,
  "描述": "log_set_file_prefix 直接 strdup 用户传入的 prefix，不做任何路径规范化或验证，如果传入包含 ../ 的字符串，日志文件会写入到预期目录之外",
  "入口": "log_set_file_prefix 的 prefix 参数",
  "数据流追踪": "log_set_file_prefix(ctx, prefix) → free(ctx->file_prefix) → ctx->file_prefix = strdup(prefix) → file_handler_internal/rotate_file 以 ctx->file_prefix 拼路径 → fopen(ctx->file_prefix, \"a\")",
  "调用链": "log_set_file_prefix -> strdup(prefix) -> ctx->file_prefix -> fopen(ctx->file_prefix) / rename(ctx->file_prefix)",
  "防护缺失证据": "无任何路径规范化、黑名单或白名单检查，直接使用用户输入作为文件路径",
  "利用思路": "传入 ../evil.log，日志文件被写入父目录；结合 log_set_file_prefix 可写到系统任意目录",
  "修复建议": "对 prefix 做路径规范化（如 realpath），拒绝包含 .. 的路径，或将文件限定在指定目录下",
  "验证结果": "Windows 11 实测，传入 ..\\..\\windows_test.txt 成功在 build 目录上方创建文件"
}
```

## 2. 竞态条件漏洞

```json
{
  "分级": "已确认",
  "漏洞类型": "竞态条件",
  "漏洞位置": "log.c:875-899 (log_enable_mpool / log_enable_ts_cache)",
  "置信度": 85,
  "描述": "log_enable_mpool 和 log_enable_ts_cache 在加锁前读取 ctx->async_enabled，并在解锁后调用 log_set_async，存在 TOCTOU 竞态",
  "入口": "log_enable_mpool / log_enable_ts_cache",
  "数据流追踪": "bool was_async = ctx->async_enabled → (未加锁) → log_set_async(ctx, false) → rwlock_write_lock → ctx->enable_mpool = enable → rwlock_write_unlock → log_set_async(ctx, true)（已解锁）",
  "调用链": "线程A: log_enable_mpool → 读取 async_enabled=false → 跳过停止异步 → 加锁改 mpool → 解锁 → 不重启异步\n线程B(同时): log_set_async(ctx, true) → 启动异步线程，但线程A的 mpool 状态还没生效",
  "防护缺失证据": "ctx->async_enabled 在无锁下读取，log_set_async（创建/销毁线程、修改共享标志）在锁外调用",
  "利用思路": "多线程同时调用 log_enable_mpool，一个线程读到过时的 async_enabled，导致异步线程状态与内存池设置不一致",
  "修复建议": "将整个操作（含 log_set_async 调用）置于一把锁下，或将 async_enabled 的读取也纳入锁保护",
  "验证结果": "代码审查确认，存在 TOCTOU 窗口"
}
```

## 3. 内存分配问题（含 4 个子 Bug）

```json
{
  "分级": "已确认",
  "漏洞类型": "内存分配问题",
  "漏洞位置": "log.c:300-347 (queue_entry_create), log.c:447-449 (queue_destroy), log.c:866-873 (log_set_file_prefix), log.c:1174-1210 (log_add_syslog_handler)",
  "置信度": 95,
  "描述": "共发现 4 个内存分配相关的 Bug，包括确定性泄漏、空指针写入风险、错误的条件判断和缺少 strdup 返回值检查",
  "子漏洞": [
    {
      "编号": "Bug 1",
      "标题": "queue_destroy 是空函数，哑结点永远泄漏",
      "位置": "log.c:447-449",
      "描述": "queue_init 每次 calloc 一个哑结点(log.c:361)，但 queue_destroy 是 (void)q 空函数，永不释放",
      "影响": "每次 log_create → log_destroy 泄漏一个 log_queue_entry（约 80 字节），长期运行的程序内存持续增长",
      "验证结果": "50000 次 create/destroy 后 PageFile 从 640KB 涨到 6544KB，泄漏约 5.8MB"
    },
    {
      "编号": "Bug 2",
      "标题": "MPool 模式下 realloc 失败导致 entry 永久丢失",
      "位置": "log.c:316-356",
      "描述": "queue_entry_create 中 mpool 模式下 realloc(entry->message) 失败返回 NULL 时，原 512B buffer 泄漏，且 entry 既不放回 mpool 也不 free",
      "影响": "极端内存压力下触发，导致 entry 结构体和原 buffer 永久泄漏"
    },
    {
      "编号": "Bug 3",
      "标题": "entry->file 的 realloc 条件检查的是消息长度而非文件名长度",
      "位置": "log.c:321-322",
      "描述": "if ((size_t)len >= 128) 中的 len 是 vsnprintf 返回的消息体长度，却用来判断是否需要 realloc entry->file",
      "影响": "消息 >128 字节时触发不必要的 file buffer realloc，且 realloc 大小远超实际需要的文件名长度",
      "验证结果": "通过 test_fixes.c 的 test_wrong_realloc_condition 触发该路径"
    },
    {
      "编号": "Bug 4",
      "标题": "MPool 模式下 entry->file 为 NULL 时直接 strncpy 写入空指针",
      "位置": "log.c:335-340",
      "描述": "mpool 模式下 entry->file 的 NULL 检查被 !ctx->enable_mpool 跳过，若 entry->file realloc 失败为 NULL，后续 strncpy(entry->file, ...) 导致 segfault",
      "影响": "条件触发时程序直接崩溃"
    },
    {
      "编号": "Bug 5",
      "标题": "strdup 返回值未检查",
      "位置": "log.c:871, log.c:1195",
      "描述": "log_set_file_prefix 和 log_add_syslog_handler 中 strdup 返回值未做 NULL 检查",
      "影响": "内存耗尽时 strdup 返回 NULL，后续 strcpy/snprintf 等操作使用 NULL 指针导致崩溃"
    }
  ],
  "修复建议": [
    "queue_destroy 中遍历队列释放所有结点（含哑结点）",
    "queue_entry_create 中 realloc 失败时恢复原指针并返回错误",
    "将 file buffer realloc 的条件从消息长度改为文件名实际长度",
    "统一 NULL 检查逻辑，不因 mpool 模式跳过",
    "所有 strdup 调用处检查返回值"
  ],
  "验证结果": "Bug 1 已通过 test_fixes.exe 实测验证，泄漏量可量化；Bug 2-5 经代码审查确认路径可达"
}
```

```
