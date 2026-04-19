#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

// 测试 log_set_async 函数的竞态条件
void test_async_race_condition() {
    printf("\n=== 测试 log_set_async 竞态条件 ===\n");
    
    log *ctx = log_create();
    if (!ctx) {
        printf("Failed to initialize log\n");
        return;
    }
    
    // 测试多次启用/禁用异步模式
    for (int i = 0; i < 10; i++) {
        if (log_set_async(ctx, true) != 0) {
            printf("Failed to enable async mode\n");
        } else {
            printf("Async mode enabled successfully\n");
        }
        
        // 写入一些日志
        log_info(ctx, "Test log message %d", i);
        
        if (log_set_async(ctx, false) != 0) {
            printf("Failed to disable async mode\n");
        } else {
            printf("Async mode disabled successfully\n");
        }
    }
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
}

// 测试 log_enable_mpool 和 log_enable_ts_cache 的竞态条件
void test_mpool_ts_cache_race_condition() {
    printf("\n=== 测试 log_enable_mpool 和 log_enable_ts_cache 竞态条件 ===\n");
    
    log *ctx = log_create();
    if (!ctx) {
        printf("Failed to initialize log\n");
        return;
    }
    
    // 启用异步模式
    if (log_set_async(ctx, true) != 0) {
        printf("Failed to enable async mode\n");
        log_destroy(ctx);
        return;
    }
    
    // 测试在异步模式下修改内存池设置
    for (int i = 0; i < 5; i++) {
        log_enable_mpool(ctx, true);
        printf("Memory pool enabled\n");
        
        log_info(ctx, "Test log with mpool enabled %d", i);
        
        log_enable_mpool(ctx, false);
        printf("Memory pool disabled\n");
        
        log_info(ctx, "Test log with mpool disabled %d", i);
    }
    
    // 测试在异步模式下修改时间戳缓存设置
    for (int i = 0; i < 5; i++) {
        log_enable_ts_cache(ctx, true);
        printf("Timestamp cache enabled\n");
        
        log_info(ctx, "Test log with ts cache enabled %d", i);
        
        log_enable_ts_cache(ctx, false);
        printf("Timestamp cache disabled\n");
        
        log_info(ctx, "Test log with ts cache disabled %d", i);
    }
    
    // 禁用异步模式
    log_set_async(ctx, false);
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
}

// 测试 log_add_syslog_handler 中的资源泄漏风险
void test_syslog_resource_leak() {
    printf("\n=== 测试 log_add_syslog_handler 资源泄漏 ===\n");
    
    log *ctx = log_create();
    if (!ctx) {
        printf("Failed to initialize log\n");
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
    
    // 写入一些日志
    log_info(ctx, "Test syslog message");
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
}

#ifdef _WIN32
// Windows 线程函数
DWORD WINAPI thread_function(LPVOID arg) {
    log *ctx = (log*)arg;
    
    for (int i = 0; i < 100; i++) {
        log_info(ctx, "Thread %lu: test message %d", GetCurrentThreadId(), i);
    }
    
    return 0;
}
#else
// POSIX 线程函数
void* thread_function(void* arg) {
    log *ctx = (log*)arg;
    
    for (int i = 0; i < 100; i++) {
        log_info(ctx, "Thread %lu: test message %d", pthread_self(), i);
    }
    
    return NULL;
}
#endif

// 测试多线程环境下的异步日志
void test_multithread_async() {
    printf("\n=== 测试多线程环境下的异步日志 ===\n");
    
    log *ctx = log_create();
    if (!ctx) {
        printf("Failed to initialize log\n");
        return;
    }
    
    // 启用异步模式
    if (log_set_async(ctx, true) != 0) {
        printf("Failed to enable async mode\n");
        log_destroy(ctx);
        return;
    }
    
    // 创建多个线程
#ifdef _WIN32
    HANDLE threads[5];
    for (int i = 0; i < 5; i++) {
        threads[i] = CreateThread(NULL, 0, thread_function, ctx, 0, NULL);
        if (threads[i] == NULL) {
            printf("Failed to create thread %d\n", i);
        }
    }
    
    // 等待所有线程完成
    WaitForMultipleObjects(5, threads, TRUE, INFINITE);
    for (int i = 0; i < 5; i++) {
        CloseHandle(threads[i]);
    }
#else
    pthread_t threads[5];
    for (int i = 0; i < 5; i++) {
        if (pthread_create(&threads[i], NULL, thread_function, ctx) != 0) {
            printf("Failed to create thread %d\n", i);
        }
    }
    
    // 等待所有线程完成
    for (int i = 0; i < 5; i++) {
        pthread_join(threads[i], NULL);
    }
#endif
    
    // 禁用异步模式
    log_set_async(ctx, false);
    
    log_destroy(ctx);
    printf("=== 测试完成 ===\n");
}

int main() {
    printf("开始测试修复后的功能...\n");
    
    test_async_race_condition();
    test_mpool_ts_cache_race_condition();
    test_syslog_resource_leak();
    test_multithread_async();
    
    printf("\n所有测试完成！\n");
    return 0;
}
