由于重构过程使用了Trae的AI编程功能，AI写出了很多有漏洞的地方，这里记录下来学习一下（没绷住），有漏洞版本放在[log.c.old.old](./log.c.old)文件中

## 5 个真实漏洞
| 编号 | 标题 | 结果 |
|------|------|------|
| Bug 1 | queue_destroy 空函数，哑结点泄漏 | ✅ **真实漏洞** |
| Bug 2 | MPool 模式下 realloc 失败导致永久丢失 | ✅ **真实漏洞** |
| Bug 3 | file buffer realloc 条件检查了错误的值 | ✅ **真实漏洞** |
| Bug 4 | MPool 模式下 strncpy 写入 NULL 指针 | ✅ **真实漏洞** |
| Bug 5 | strdup 返回值未检查 | ✅ **真实漏洞** |

---

## Bug 1：queue_destroy 是空函数，哑结点永远泄漏

### 背景知识（先搞懂队列结构）

这个日志库有一个"异步模式"——你的程序写日志时，先把日志塞到一个**队列**里，然后另一个**后台线程**从队列里取出来慢慢写文件。这样你的程序不会被写文件拖慢。

这个队列是用**链表**实现的。链表就像一列火车，每节车厢（结点）拉着下一节。但这里有个技巧：队列里有一个**哑结点（dummy node）**，它就像一个"空车头"，不装货，只是为了方便车头和车位操作。

### 漏洞在哪？

看创建队列时（[queue_init](file:///d:/Projects/log.c/src/log.c#L361)）：

```c
static void queue_init(log_queue *q, size_t max_size) {
  log_queue_entry *dummy = calloc(1, sizeof(log_queue_entry));  // ① 申请一块内存放哑结点
  q->head = dummy;   // ② 队头指向它
  q->tail = dummy;   // ③ 队尾也指向它
```

这里用 `calloc` 申请了内存，哑结点就躺在堆（heap）里了。

再看销毁队列时（[queue_destroy](file:///d:/Projects/log.c/src/log.c#L447-L449)）：

```c
static void queue_destroy(log_queue *q) {
  (void)q;  // ← 啥也没干！！！这行等于：我就看看，我不碰它
}
```

`(void)q` 这句在 C 语言里是个"消警告"技巧 —— 意思是"我知道这个参数没用，编译器你别报警告了"。**但它没有释放任何内存！**

然后看销毁日志上下文时（[log_destroy](file:///d:/Projects/log.c/src/log.c#L775)）：

```c
queue_destroy(&ctx->queue);  // 调用的是上面的空函数
// ... 其他清理 ...
free(ctx);  // 释放 ctx 自己
```

`free(ctx)` 释放了 `ctx` 这个结构体本身，但 `ctx->queue.head` 指向的那个哑结点是**单独用 calloc 申请的**，不跟着 ctx 一起释放。

### 触发过程（一步步来）

```
1. log_create() → queue_init() → calloc 申请哑结点（80字节左右）
2. log_destroy() → queue_destroy() → (void)q 啥也不干 → free(ctx)
                                                  ↑
                                           哑结点还躺在堆里！
3. 哑结点变成"孤儿"——没有人知道它的地址了，永远没法 free
```

**后果**：程序每执行一次 `log_create` + `log_destroy`，就泄漏约 80 字节。测试验证：执行 50000 次后，内存从 640KB 涨到 6544KB，泄漏了约 5.8MB。

**一句话总结：借了钱不还，借一次 80 块，借 5 万次就欠了 5.8MB。**

---

## Bug 2：MPool 模式下 realloc 失败导致 entry 永久丢失

### 背景知识

MPool（内存池）是本库的一个优化功能。它提前申请好一批 entry（日志条目），用完了不释放，放回池子里下次再用，减少频繁 malloc/free 的开销。

`mpool_alloc` 申请一个 entry 时，里面包含：
- `entry->message`：初始 `malloc(512)`（512 字节的消息缓冲区）
- `entry->file`：初始 `malloc(128)`（128 字节的文件名缓冲区）

### 漏洞在哪？

看 [queue_entry_create](file:///d:/Projects/log.c/src/log.c#L316-L325) 的核心部分：

```c
if (ctx->enable_mpool) {
    if ((size_t)len >= 512) {
      entry->message = realloc(entry->message, len + 1);  // ① 消息太长？扩容
    }
    if ((size_t)len >= 128) {
      entry->file = realloc(entry->file, len + 1);        // ② 文件名字段扩容
    }
}
```

这里调用了 `realloc`。`realloc` 的工作方式是：
- **如果扩容成功**：返回新地址，原来的内存自动释放
- **如果扩容失败（内存不够）**：返回 **NULL**，原来的内存**不动**（不释放！）

问题是：`entry->message = realloc(entry->message, len + 1);`

如果 `realloc` 返回 NULL，`entry->message` 就被改成了 NULL，而**原来的 512 字节缓冲区再也找不到了**。

### 触发过程

```
realloc 前：
  entry->message → [512 字节的缓冲区]  （在堆里）
  entry->file    → [128 字节的缓冲区]  （在堆里）

realloc 失败后：
  entry->message = NULL
                    ↑
        原来的 512 字节还在堆里，但没人知道地址了 → 泄漏
```

然后后面的错误检查是这样写的（[log.c:327](file:///d:/Projects/log.c/src/log.c#L327)）：

```c
if (!entry->message || (!entry->file && !ctx->enable_mpool)) {
    free(entry->message);          // free(NULL)，无害
    if (!ctx->enable_mpool) free(entry->file);   // 跳过，因为 mpool 模式
    if (!ctx->enable_mpool) free(entry);         // 跳过，因为 mpool 模式
    return NULL;                   // 返回 NULL
}
```

所以：
1. `entry->message` 是 NULL，进了 if
2. `free(NULL)` — 没事，free NULL 是安全的
3. 因为是 mpool 模式，不 free `entry`，也不 free `entry->file`
4. 返回 NULL

**entry 本身**没被释放，也没被放回 mpool 空闲链表。它是从 `mpool_alloc` 拿出来的，现在不还回去了，等于从池子里弄丢了。

### 后果

两个泄漏：
1. 原来的 512 字节消息缓冲区泄漏（`realloc` 失败但旧内存没释放）
2. entry 结构体本身从池子里永久丢失

**一句话总结：你把东西借出去，人家说"扩个容"，结果扩容失败，东西也不还你了。东西和原来的壳子都丢了。**

---

## Bug 3：entry->file 的 realloc 条件检查了消息长度

### 漏洞在哪？

看 [queue_entry_create](file:///d:/Projects/log.c/src/log.c#L321-L323)：

```c
int len = vsnprintf(NULL, 0, ev->fmt, args_copy);  // len = 格式化后的【消息】长度

if (ctx->enable_mpool) {
    if ((size_t)len >= 512) {
      entry->message = realloc(entry->message, len + 1);  // 正确：扩消息缓冲区
    }
    if ((size_t)len >= 128) {           // ← 这检查的是【消息长度】，不是文件名长度！
      entry->file = realloc(entry->file, len + 1);  // 却用来扩文件名缓冲区
    }
}
```

逻辑应该是什么？
- `entry->message` 不够大 → 扩 `entry->message`（条件应该是 `len >= 512`，正确）
- `entry->file` 不够大 → 扩 `entry->file`（条件应该是 `strlen(ev->file) >= 128`，但代码写成了 `len >= 128`）

`len` 是消息长度，`ev->file` 是文件名（比如 `"main.c"` 只有 6 个字符）。这两个毫无关系！

### 触发过程

假设你写了一条 200 字节的日志消息：
```
len = 200  （消息长度）
ev->file = "main.c"  （文件名长度 = 6）
```

`(size_t)200 >= 128` → **true！** 于是代码对 `entry->file` 执行 `realloc(entry->file, 201)`。

但是 `entry->file` 原本的 128 字节完全够放 "main.c"（才 6 个字节）！这个 realloc 完全没必要。

更糟糕的是：如果消息很长（比如 5000 字节），realloc 会申请 5001 字节给文件名缓冲区，严重浪费内存。

### 后果

每次日志消息超过 128 字节，都会触发不必要的文件缓冲区扩容，浪费性能和内存。

**一句话总结：你说"天气真好"，代码却以为你要写一篇 5000 字的作文，赶紧把笔记本封面换成了 5000 页的大本子——实际上你只需要巴掌大一张纸。**

---

## Bug 4：MPool 模式下 entry->file 为 NULL 时 strncpy 写入空指针

### 背景知识

**NULL 指针** 就是指向地址 0 的指针。往 NULL 指针写数据，操作系统会直接杀掉你的程序（段错误 / segfault / 程序崩溃）。

### 漏洞在哪？

先看 Bug 2 和 Bug 3 的连锁反应：

Bug 2 说 `realloc` 可以把 `entry->file` 变成 NULL。
Bug 3 说 `realloc` 触发的条件不对。

把它们结合起来：

```c
// 第 1 步：如果 len >= 128（消息太长），就 realloc entry->file
if ((size_t)len >= 128) {
    entry->file = realloc(entry->file, len + 1);  // 可能返回 NULL
}

// 第 2 步：检查 entry 是否有效（关键漏洞就在这里！）
if (!entry->message || (!entry->file && !ctx->enable_mpool)) {
    // ↑ 注意这个条件：!ctx->enable_mpool
    // 意思是："只有没开 mpool 的时候，才检查 entry->file 是不是 NULL"
    // 如果开了 mpool，即使 entry->file 是 NULL，也不管！
    ...
    return NULL;
}

// 第 3 步：直接写入
if (ctx->enable_mpool) {
    strncpy(entry->file, ev->file ? ev->file : "", 127);  // ← 如果 entry->file 是 NULL，这里直接崩溃！
    entry->file[127] = '\0';
}
```

### 触发过程

```
条件：
1. 开启了 mpool（ctx->enable_mpool = true）
2. 日志消息超过 128 字节（触发 Bug 3 的错误条件）
3. 内存紧张，realloc 返回 NULL

执行流程：
  entry->file = realloc(entry->file, len + 1)  // 返回 NULL
  entry->file = NULL

  if (!entry->message || (!entry->file && !ctx->enable_mpool))
  // !entry->file 是 true（NULL）
  // !ctx->enable_mpool 是 false（因为开启了 mpool）
  // true && false = false → 不进 if，继续执行！

  strncpy(entry->file, ev->file ? ev->file : "", 127)
  // entry->file 是 NULL
  // strncpy(NULL, ..., 127) → 往地址 0 写数据 → 程序崩溃！
```

### 后果

**程序直接崩溃（segmentation fault）**。这属于"拒绝服务"级别的漏洞——攻击者如果能让程序在内存不足时写一条超过 128 字节的日志，程序就挂了。

**一句话总结：保安看见你没戴工作证就放你进了门，结果你要去的办公室根本不存在，一脚踩空从 100 楼摔了下去。**

---

## Bug 5：strdup 返回值未检查

### 背景知识

`strdup` = string duplicate（字符串复制）。它的工作：
1. 内部调用 `malloc` 申请一块大小刚好的内存
2. 把源字符串复制进去
3. 返回新内存的地址

如果内存不够，`malloc` 失败，`strdup` 返回 **NULL**。

### 漏洞在哪？

看 [log_set_file_prefix](file:///d:/Projects/log.c/src/log.c#L866-L873)：

```c
void log_set_file_prefix(log *ctx, const char *prefix) {
  rwlock_write_lock(&ctx->rwlock);
  free(ctx->file_prefix);                     // ① 释放旧的
  ctx->file_prefix = strdup(prefix ? prefix : "log");  // ② 复制新的（没检查返回值！）
  rwlock_write_unlock(&ctx->rwlock);
}
```

如果 `strdup` 返回 NULL，`ctx->file_prefix` 就变成了 NULL。然后后面所有用到 `ctx->file_prefix` 的地方都会出问题，比如 [file_handler_internal](file:///d:/Projects/log.c/src/log.c#L698) 里的 `fopen(ctx->file_prefix, "a")` → `fopen(NULL, "a")` → 崩溃。

再看 [log_add_syslog_handler](file:///d:/Projects/log.c/src/log.c#L1195)：

```c
free(ctx->syslog_ident);
ctx->syslog_ident = strdup(ident);  // 没检查！
```

同样的问题。

### 触发过程

```
1. 程序内存即将耗尽
2. 调用 log_set_file_prefix(ctx, "myapp.log")
3. strdup 内部 malloc 失败 → 返回 NULL
4. ctx->file_prefix = NULL
5. 下次写日志时，fopen(NULL, "a") → 崩溃
```

### 后果

极端内存压力下程序崩溃。

**一句话总结：你让快递员去取个包裹，快递员说"我没钱打车去"就直接空手回来了，你还以为他拿到了包裹，直接拆快递——拆了个寂寞，手被划了。**

---

仅仅在Windows平台验证了部分bug，在[test_bug.c](./test_bug.c)中。

## 总结表

| Bug | 类型 | 根本原因 | 触发条件 | 后果 |
|-----|------|---------|---------|------|
| **1** | 内存泄漏 | `queue_destroy` 是空函数 | 每次 `log_create` → `log_destroy` | 每次泄漏 ~80 字节，长期运行内存暴涨 |
| **2** | 内存泄漏 | `realloc` 失败后覆盖原指针 | MPool 模式 + 大消息 + 内存不足 | 512B 缓冲区 + entry 结构体永久泄漏 |
| **3** | 逻辑错误 | 用消息长度判断文件缓冲区扩容 | 消息 > 128 字节 | 不必要的内存分配，浪费性能 |
| **4** | 空指针写入 | NULL 检查被 mpool 模式跳过 | Bug 2/3 同时触发 + mpool 模式 | **程序直接崩溃** |
| **5** | 空指针解引用 | `strdup` 返回值未检查 | 内存不足时调 `log_set_file_prefix` 或 `log_add_syslog_handler` | **程序直接崩溃** |
        
