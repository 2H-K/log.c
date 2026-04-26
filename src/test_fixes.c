#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static int test_errors = 0;

static void flush_output() {
    fflush(stdout);
    fflush(stderr);
}

// 测试所有日志级别
void test_all_log_levels() {
    printf("\n=== 测试所有日志级别 ===\n");
    flush_output();
    
    log *ctx = log_create();
    if (!ctx) {
        printf("FAIL: Failed to initialize log\n");
        test_errors++;
        flush_output();
        return;
    }
    
    // 测试所有日志级别
    log_ctx_trace(ctx, "这是TRACE级别的日志消息");
    log_ctx_debug(ctx, "这是DEBUG级别的日志消息");
    log_ctx_info(ctx, "这是INFO级别的日志消息");
    log_ctx_warn(ctx, "这是WARN级别的日志消息");
    log_ctx_error(ctx, "这是ERROR级别的日志消息");
    log_ctx_fatal(ctx, "这是FATAL级别的日志消息");
    
    // 测试带参数的日志
    log_ctx_trace(ctx, "TRACE: 参数测试 %d, %s", 123, "字符串");
    log_ctx_debug(ctx, "DEBUG: 参数测试 %f, %d", 3.14, 456);
    log_ctx_info(ctx, "INFO: 参数测试 %s", "测试字符串");
    log_ctx_warn(ctx, "WARN: 参数测试 %d", 789);
    log_ctx_error(ctx, "ERROR: 参数测试 %s, %d", "错误", 999);
    log_ctx_fatal(ctx, "FATAL: 参数测试 %d", -1);
    
    log_destroy(ctx);
    printf("=== 所有日志级别测试完成 ===\n");
    flush_output();
}

// 测试 log_set_async 函数的竞态条件
void test_async_race_condition() {
    printf("\n=== 测试 log_set_async 竞态条件 ===\n");
    flush_output();
    
    log *ctx = log_create();
    if (!ctx) {
        printf("FAIL: Failed to initialize log\n");
        test_errors++;
        flush_output();
        return;
    }
    
    // 测试多次启用/禁用异步模式
    for (int i = 0; i < 10; i++) {
        if (log_set_async(ctx, true) != 0) {
            printf("FAIL: Failed to enable async mode (iteration %d)\n", i);
            test_errors++;
        } else {
            printf("Async mode enabled successfully (iteration %d)\n", i);
        }
        flush_output();
        
        // 写入所有级别的日志
        log_ctx_trace(ctx, "Async TRACE test %d", i);
        log_ctx_debug(ctx, "Async DEBUG test %d", i);
        log_ctx_info(ctx, "Async INFO test %d", i);
        log_ctx_warn(ctx, "Async WARN test %d", i);
        log_ctx_error(ctx, "Async ERROR test %d", i);
        log_ctx_fatal(ctx, "Async FATAL test %d", i);
        
        if (log_set_async(ctx, false) != 0) {
            printf("FAIL: Failed to disable async mode (iteration %d)\n", i);
            test_errors++;
        } else {
            printf("Async mode disabled successfully (iteration %d)\n", i);
        }
        flush_output();
    }
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
    flush_output();
}

// 测试 log_enable_mpool 和 log_enable_ts_cache 的竞态条件
void test_mpool_ts_cache_race_condition() {
    printf("\n=== 测试 log_enable_mpool 和 log_enable_ts_cache 竞态条件 ===\n");
    flush_output();
    
    log *ctx = log_create();
    if (!ctx) {
        printf("FAIL: Failed to initialize log\n");
        test_errors++;
        flush_output();
        return;
    }
    
    // 启用异步模式
    if (log_set_async(ctx, true) != 0) {
        printf("FAIL: Failed to enable async mode\n");
        test_errors++;
        flush_output();
        log_destroy(ctx);
        return;
    }
    
    // 测试在异步模式下修改内存池设置
    for (int i = 0; i < 5; i++) {
        log_enable_mpool(ctx, true);
        printf("Memory pool enabled (iteration %d)\n", i);
        flush_output();
        
        // 测试所有级别
        log_ctx_trace(ctx, "MPool TRACE test %d", i);
        log_ctx_debug(ctx, "MPool DEBUG test %d", i);
        log_ctx_info(ctx, "MPool INFO test %d", i);
        log_ctx_warn(ctx, "MPool WARN test %d", i);
        log_ctx_error(ctx, "MPool ERROR test %d", i);
        log_ctx_fatal(ctx, "MPool FATAL test %d", i);
        
        log_enable_mpool(ctx, false);
        printf("Memory pool disabled (iteration %d)\n", i);
        flush_output();
        
        log_ctx_trace(ctx, "No MPool TRACE test %d", i);
        log_ctx_debug(ctx, "No MPool DEBUG test %d", i);
        log_ctx_info(ctx, "No MPool INFO test %d", i);
        log_ctx_warn(ctx, "No MPool WARN test %d", i);
        log_ctx_error(ctx, "No MPool ERROR test %d", i);
        log_ctx_fatal(ctx, "No MPool FATAL test %d", i);
    }
    
    // 测试在异步模式下修改时间戳缓存设置
    for (int i = 0; i < 5; i++) {
        log_enable_ts_cache(ctx, true);
        printf("Timestamp cache enabled (iteration %d)\n", i);
        flush_output();
        
        log_ctx_trace(ctx, "TSCache TRACE test %d", i);
        log_ctx_debug(ctx, "TSCache DEBUG test %d", i);
        log_ctx_info(ctx, "TSCache INFO test %d", i);
        log_ctx_warn(ctx, "TSCache WARN test %d", i);
        log_ctx_error(ctx, "TSCache ERROR test %d", i);
        log_ctx_fatal(ctx, "TSCache FATAL test %d", i);
        
        log_enable_ts_cache(ctx, false);
        printf("Timestamp cache disabled (iteration %d)\n", i);
        flush_output();
        
        log_ctx_trace(ctx, "No TSCache TRACE test %d", i);
        log_ctx_debug(ctx, "No TSCache DEBUG test %d", i);
        log_ctx_info(ctx, "No TSCache INFO test %d", i);
        log_ctx_warn(ctx, "No TSCache WARN test %d", i);
        log_ctx_error(ctx, "No TSCache ERROR test %d", i);
        log_ctx_fatal(ctx, "No TSCache FATAL test %d", i);
    }
    
    // 禁用异步模式
    log_set_async(ctx, false);
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
    flush_output();
}

// 测试 log_add_syslog_handler 中的资源泄漏风险
void test_syslog_resource_leak() {
    printf("\n=== 测试 log_add_syslog_handler 资源泄漏 ===\n");
    flush_output();
    
    log *ctx = log_create();
    if (!ctx) {
        printf("FAIL: Failed to initialize log\n");
        test_errors++;
        flush_output();
        return;
    }
    
    // 尝试添加多个 syslog 处理器，直到达到上限
    int max_handlers = 10; // 假设最大处理器数量为 10
    int added = 0;
    
    for (int i = 0; i < max_handlers + 5; i++) {
        int result = log_add_syslog_handler(ctx, "test", 0, 0);
        if (result >= 0) {
            printf("Added syslog handler %d\n", result);
            added++;
        } else {
            printf("Failed to add syslog handler (expected when limit reached)\n");
        }
    }
    
    printf("Added %d syslog handlers\n", added);
    
    // 写入所有级别的日志
    log_ctx_trace(ctx, "Syslog TRACE message");
    log_ctx_debug(ctx, "Syslog DEBUG message");
    log_ctx_info(ctx, "Syslog INFO message");
    log_ctx_warn(ctx, "Syslog WARN message");
    log_ctx_error(ctx, "Syslog ERROR message");
    log_ctx_fatal(ctx, "Syslog FATAL message");
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
    flush_output();
}

#ifdef _WIN32
// Windows 线程函数
DWORD WINAPI thread_function(LPVOID arg) {
    log *ctx = (log*)arg;
    
    for (int i = 0; i < 100; i++) {
        // 每个线程写入所有级别的日志
        log_ctx_trace(ctx, "Thread %lu: TRACE message %d", GetCurrentThreadId(), i);
        log_ctx_debug(ctx, "Thread %lu: DEBUG message %d", GetCurrentThreadId(), i);
        log_ctx_info(ctx, "Thread %lu: INFO message %d", GetCurrentThreadId(), i);
        log_ctx_warn(ctx, "Thread %lu: WARN message %d", GetCurrentThreadId(), i);
        log_ctx_error(ctx, "Thread %lu: ERROR message %d", GetCurrentThreadId(), i);
        log_ctx_fatal(ctx, "Thread %lu: FATAL message %d", GetCurrentThreadId(), i);
    }
    
    return 0;
}
#else
// POSIX 线程函数
void* thread_function(void* arg) {
    log *ctx = (log*)arg;
    
    for (int i = 0; i < 100; i++) {
        // 每个线程写入所有级别的日志
        log_ctx_trace(ctx, "Thread %lu: TRACE message %d", pthread_self(), i);
        log_ctx_debug(ctx, "Thread %lu: DEBUG message %d", pthread_self(), i);
        log_ctx_info(ctx, "Thread %lu: INFO message %d", pthread_self(), i);
        log_ctx_warn(ctx, "Thread %lu: WARN message %d", pthread_self(), i);
        log_ctx_error(ctx, "Thread %lu: ERROR message %d", pthread_self(), i);
        log_ctx_fatal(ctx, "Thread %lu: FATAL message %d", pthread_self(), i);
    }
    
    return NULL;
}
#endif

// 测试多线程环境下的异步日志
void test_multithread_async() {
    printf("\n=== 测试多线程环境下的异步日志 ===\n");
    flush_output();
    
    log *ctx = log_create();
    if (!ctx) {
        printf("FAIL: Failed to initialize log\n");
        test_errors++;
        flush_output();
        return;
    }
    
    // 启用异步模式
    if (log_set_async(ctx, true) != 0) {
        printf("FAIL: Failed to enable async mode\n");
        test_errors++;
        flush_output();
        log_destroy(ctx);
        return;
    }
    
    // 创建多个线程
#ifdef _WIN32
    HANDLE threads[5];
    int thread_count = 0;
    for (int i = 0; i < 5; i++) {
        threads[i] = CreateThread(NULL, 0, thread_function, ctx, 0, NULL);
        if (threads[i] == NULL) {
            printf("FAIL: Failed to create thread %d\n", i);
            test_errors++;
        } else {
            thread_count++;
        }
    }
    flush_output();
    
    // 等待所有线程完成
    WaitForMultipleObjects(thread_count, threads, TRUE, INFINITE);
    for (int i = 0; i < thread_count; i++) {
        CloseHandle(threads[i]);
    }
    printf("All %d threads completed\n", thread_count);
#else
    pthread_t threads[5];
    int thread_count = 0;
    for (int i = 0; i < 5; i++) {
        if (pthread_create(&threads[i], NULL, thread_function, ctx) != 0) {
            printf("FAIL: Failed to create thread %d\n", i);
            test_errors++;
        } else {
            thread_count++;
        }
    }
    flush_output();
    
    // 等待所有线程完成
    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("All %d threads completed\n", thread_count);
#endif
    flush_output();
    
    // 禁用异步模式
    log_set_async(ctx, false);
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
    flush_output();
}

int main() {
    printf("开始测试修复后的功能...\n");
    flush_output();
    
    test_all_log_levels();
    test_async_race_condition();
    test_mpool_ts_cache_race_condition();
    test_syslog_resource_leak();
    test_multithread_async();
    
    printf("\n所有测试完成！\n");
    if (test_errors > 0) {
        printf("检测到 %d 个测试失败！\n", test_errors);
    } else {
        printf("所有测试通过！\n");
    }
    flush_output();
    return test_errors > 0 ? 1 : 0;
}
