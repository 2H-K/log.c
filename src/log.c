/*
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

#include "log.h"

// 使用C17静态断言确保数组大小合理
_Static_assert(sizeof(int) >= 4, "int must be at least 32 bits");
#define MAX_CALLBACKS 32

/**
 * @brief 回调函数结构体，用于存储回调函数及其相关信息
 */
typedef struct {
  log_LogFn fn;    /**< 回调函数指针 */
  void *udata;     /**< 用户数据指针 */
  int level;       /**< 触发回调所需的最低日志级别 */
} Callback;

/**
 * @brief 全局日志状态结构体
 */
static struct {
  void *udata;              /**< 全局用户数据 */
  log_LockFn lock;          /**< 锁函数指针 */
  int level;                /**< 最低日志级别 */
  bool quiet;               /**< 静默模式标志 */
  Callback callbacks[MAX_CALLBACKS];  /**< 回调函数数组 */
} L;


/**
 * @brief 日志级别字符串映射表
 */
static const char *level_strings[] = {
  "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
/**
 * @brief 各日志级别对应的颜色代码
 */
static const char *level_colors[] = {
  "\x1b[90m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[91m"
};
#endif


/**
 * @brief 输出到标准输出的日志回调函数
 * @param ev 日志事件指针
 */
static void stdout_callback(log_Event *ev) {
  char buf[16];
  // C17: 更明确的缓冲区处理
  size_t len = strftime(buf, sizeof(buf), "%H:%M:%S", ev->time);
  if (len > 0 && len < sizeof(buf)) {
    buf[len] = '\0';
  } else {
    buf[0] = '\0';  // 安全处理
  }
  
#ifdef LOG_USE_COLOR
  fprintf(
    ev->udata, "%s %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
    buf, level_colors[ev->level], level_strings[ev->level],
    ev->file, ev->line);
#else
  fprintf(
    ev->udata, "%s %-5s %s:%d: ",
    buf, level_strings[ev->level], ev->file, ev->line);
#endif
  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}


/**
 * @brief 输出到文件的日志回调函数
 * @param ev 日志事件指针
 */
static void file_callback(log_Event *ev) {
  char buf[64];
  // C17: 更明确的缓冲区处理
  size_t len = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time);
  if (len > 0 && len < sizeof(buf)) {
    buf[len] = '\0';
  } else {
    buf[0] = '\0';  // 安全处理
  }
  
  fprintf(
    ev->udata, "%s %-5s %s:%d: ",
    buf, level_strings[ev->level], ev->file, ev->line);
  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}


/**
 * @brief 获取日志锁
 */
static void lock(void)   {
  if (L.lock) { L.lock(true, L.udata); }
}


/**
 * @brief 释放日志锁
 */
static void unlock(void) {
  if (L.lock) { L.lock(false, L.udata); }
}


/**
 * @brief 将日志级别转换为字符串表示
 * @param level 日志级别
 * @return 对应级别的字符串表示
 */
const char* log_level_string(int level) {
  // C17: 添加边界检查
  if (level < 0 || level >= (int)(sizeof(level_strings)/sizeof(level_strings[0]))) {
    return "UNKNOWN";
  }
  return level_strings[level];
}


/**
 * @brief 设置日志锁函数
 * @param fn 锁函数指针，若为NULL则不使用锁机制
 * @param udata 用户数据指针，传递给锁函数
 */
void log_set_lock(log_LockFn fn, void *udata) {
  L.lock = fn;
  L.udata = udata;
}


/**
 * @brief 设置日志输出级别
 * @param level 日志级别，低于此级别的日志将被忽略
 */
void log_set_level(int level) {
  L.level = level;
}


/**
 * @brief 设置静默模式
 * @param enable 是否启用静默模式，true为启用，false为禁用
 */
void log_set_quiet(bool enable) {
  L.quiet = enable;
}


/**
 * @brief 添加自定义日志回调函数
 * @param fn 回调函数指针
 * @param udata 用户数据指针，将传递给回调函数
 * @param level 只有等于或高于此级别的日志才会触发回调
 * @return 成功返回0，失败返回-1
 */
int log_add_callback(log_LogFn fn, void *udata, int level) {
  // C17: 优化循环，使用更清晰的条件
  for (int i = 0; i < MAX_CALLBACKS; i++) {
    if (L.callbacks[i].fn == NULL) {
      L.callbacks[i] = (Callback) { fn, udata, level };
      return 0;
    }
  }
  return -1;
}


/**
 * @brief 添加文件流作为日志输出目标
 * @param fp 文件流指针
 * @param level 只有等于或高于此级别的日志才会写入文件
 * @return 成功返回0，失败返回-1
 */
int log_add_fp(FILE *fp, int level) {
  return log_add_callback(file_callback, fp, level);
}


/**
 * @brief 初始化日志事件结构体
 * @param ev 日志事件指针
 * @param udata 用户数据指针
 */
static void init_event(log_Event *ev, void *udata) {
  // C17: 更清晰的条件判断
  if (ev->time == NULL) {
    time_t t = time(NULL);
    ev->time = localtime(&t);
  }
  ev->udata = udata;
}


/**
 * @brief 记录日志的核心函数
 * @param level 日志级别
 * @param file 发生日志的文件名
 * @param line 发生日志的行号
 * @param fmt 格式化字符串
 * @param ... 格式化参数
 */
void log_log(int level, const char *file, int line, const char *fmt, ...) {
  // C17: 利用复合字面量初始化结构体
  log_Event ev = {0};  // 初始化为零值
  ev.fmt = fmt;
  ev.file = file;
  ev.line = line;
  ev.level = level;

  lock();

  if (!L.quiet && level >= L.level) {
    init_event(&ev, stderr);
    va_start(ev.ap, fmt);
    stdout_callback(&ev);
    va_end(ev.ap);
  }

  // C17: 优化循环，避免重复计算
  for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn != NULL; i++) {
    Callback *cb = &L.callbacks[i];
    if (level >= cb->level) {
      init_event(&ev, cb->udata);
      va_start(ev.ap, fmt);
      cb->fn(&ev);
      va_end(ev.ap);
    }
  }

  unlock();
}
