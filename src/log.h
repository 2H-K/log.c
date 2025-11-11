/**
 * Copyright (c) 2020 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

#define LOG_VERSION "0.1.0"

// 使用C17的静态断言确保结构体大小合理
_Static_assert(sizeof(int) >= 4, "int must be at least 32 bits");

/**
 * @brief 日志事件结构体，包含日志记录所需的所有信息
 */
typedef struct {
  va_list ap;              /**< 可变参数列表，用于格式化日志消息 */
  const char *fmt;         /**< 格式化字符串 */
  const char *file;        /**< 发生日志的文件名 */
  struct tm *time;         /**< 时间信息 */
  void *udata;             /**< 用户数据指针 */
  int line;                /**< 发生日志的行号 */
  int level;               /**< 日志级别 */
} log_Event;

/**
 * @brief 日志回调函数类型定义
 * @param ev 日志事件指针
 */
typedef void (*log_LogFn)(log_Event *ev);

/**
 * @brief 日志锁函数类型定义
 * @param lock 是否加锁
 * @param udata 用户数据指针
 */
typedef void (*log_LockFn)(bool lock, void *udata);

/**
 * @brief 日志级别枚举
 */
enum { LOG_TRACE, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL };

/**
 * @brief 记录TRACE级别的日志
 * @param ... 格式化参数
 */
#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)

/**
 * @brief 记录DEBUG级别的日志
 * @param ... 格式化参数
 */
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

/**
 * @brief 记录INFO级别的日志
 * @param ... 格式化参数
 */
#define log_info(...)  log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)

/**
 * @brief 记录WARN级别的日志
 * @param ... 格式化参数
 */
#define log_warn(...)  log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)

/**
 * @brief 记录ERROR级别的日志
 * @param ... 格式化参数
 */
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

/**
 * @brief 记录FATAL级别的日志
 * @param ... 格式化参数
 */
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

/**
 * @brief 将日志级别转换为字符串表示
 * @param level 日志级别
 * @return 对应级别的字符串表示
 */
const char* log_level_string(int level);

/**
 * @brief 设置日志锁函数
 * @param fn 锁函数指针，若为NULL则不使用锁机制
 * @param udata 用户数据指针，传递给锁函数
 */
void log_set_lock(log_LockFn fn, void *udata);

/**
 * @brief 设置日志输出级别
 * @param level 日志级别，低于此级别的日志将被忽略
 */
void log_set_level(int level);

/**
 * @brief 设置静默模式
 * @param enable 是否启用静默模式，true为启用，false为禁用
 */
void log_set_quiet(bool enable);

/**
 * @brief 添加自定义日志回调函数
 * @param fn 回调函数指针
 * @param udata 用户数据指针，将传递给回调函数
 * @param level 只有等于或高于此级别的日志才会触发回调
 * @return 成功返回0，失败返回-1
 */
int log_add_callback(log_LogFn fn, void *udata, int level);

/**
 * @brief 添加文件流作为日志输出目标
 * @param fp 文件流指针
 * @param level 只有等于或高于此级别的日志才会写入文件
 * @return 成功返回0，失败返回-1
 */
int log_add_fp(FILE *fp, int level);

/**
 * @brief 记录日志的核心函数
 * @param level 日志级别
 * @param file 发生日志的文件名
 * @param line 发生日志的行号
 * @param fmt 格式化字符串
 * @param ... 格式化参数
 */
void log_log(int level, const char *file, int line, const char *fmt, ...);

#endif
