/**
 * test_lifecycle.c - Process lifecycle safety (B6 roadmap)
 * POSIX only (fork + atexit); skipped elsewhere.
 *
 * Verifies:
 *  1. log_install_atfork downgrades async to sync in the child, so the child
 *     can keep logging after fork() and those lines survive a normal exit()
 *     (an async queue would have no writer thread in the child).
 *  2. log_install_atexit drains a still-async context when the process exits
 *     through exit(), without any manual log_set_async(false).
 *  3. log_destroy() unregisters the context, so an exit after teardown never
 *     touches freed memory.
 */

#include "test_harness.h"
#include "log.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>

#if LOG_FEATURE_LIFECYCLE

static int lifecycle_count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') n++;
    }
    fclose(f);
    return n;
}

static void test_lifecycle_atfork_child_logs(void) {
    const char *path = "/tmp/test_lifecycle_fork.log";
    remove(path);

    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    int idx = log_add_file(ctx, path, LOG_TRACE);
    TEST_ASSERT(idx >= 0, "log_add_file");
    ctx->handlers[0].active = false;   /* silence default stderr handler */
    TEST_ASSERT_EQ(log_set_async(ctx, true), 0, "async on");
    TEST_ASSERT_EQ(log_install_atfork(ctx), 0, "install atfork");

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        /* exit() flushes stdio; if async were still enabled these lines would
         * sit in a queue whose writer thread does not exist in the child. */
        for (int i = 0; i < 20; i++) {
            log_ctx_info(ctx, "child %d", i);
        }
        exit(0);
    }

    int status = 0;
    TEST_ASSERT_EQ(waitpid(pid, &status, 0), pid, "waitpid");
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exited cleanly");

    /* Parent is still usable after the fork. */
    log_set_async(ctx, false);
    log_ctx_info(ctx, "parent after fork");
    log_destroy(ctx);

    int lines = lifecycle_count_lines(path);
    TEST_ASSERT_EQ(lines, 21, "child lines written synchronously + parent line");
    remove(path);

    TEST_PASS("atfork: child logs synchronously");
}

static void test_lifecycle_atexit_flushes(void) {
    const char *path = "/tmp/test_lifecycle_atexit.log";
    remove(path);

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        log_handle *ctx = log_create();
        int idx = log_add_file(ctx, path, LOG_TRACE);
        if (idx < 0) _exit(41);
        ctx->handlers[0].active = false;
        if (log_set_async(ctx, true) != 0) _exit(42);
        if (log_install_atexit(ctx) != 0) _exit(43);
        for (int i = 0; i < 50; i++) {
            log_ctx_info(ctx, "exit %d", i);
        }
        /* No log_destroy / log_set_async(false): atexit must drain. */
        exit(0);
    }

    int status = 0;
    TEST_ASSERT_EQ(waitpid(pid, &status, 0), pid, "waitpid");
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "helper exited cleanly");

    int lines = lifecycle_count_lines(path);
    TEST_ASSERT_EQ(lines, 50, "atexit drained the async queue");
    remove(path);

    TEST_PASS("atexit flushes async queue");
}

static void test_lifecycle_destroy_unregisters(void) {
    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        log_handle *ctx = log_create();
        FILE *fp = fopen(TEST_DEV_NULL, "w");
        if (!ctx || !fp) _exit(44);
        log_add_fp(ctx, fp, LOG_INFO);
        if (log_install_atexit(ctx) != 0) _exit(45);
        if (log_install_atfork(ctx) != 0) _exit(46);
        log_destroy(ctx);      /* must unregister from both registries */
        fclose(fp);
        exit(0);               /* atexit handler must skip the freed context */
    }

    int status = 0;
    TEST_ASSERT_EQ(waitpid(pid, &status, 0), pid, "waitpid");
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "exit after destroy is safe");

    TEST_PASS("destroy unregisters lifecycle context");
}

void test_lifecycle_register(void) {
    test_add(test_lifecycle_atfork_child_logs, "lifecycle_atfork_child_logs");
    test_add(test_lifecycle_atexit_flushes, "lifecycle_atexit_flushes");
    test_add(test_lifecycle_destroy_unregisters, "lifecycle_destroy_unregisters");
}

#else  /* !LOG_FEATURE_LIFECYCLE */
void test_lifecycle_register(void) {}
#endif

#else  /* !POSIX */
void test_lifecycle_register(void) {}
#endif
