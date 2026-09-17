/**
 * test_crash.c - Crash-safe mode tests (A2 roadmap)
 * POSIX only (fork + fatal signals); skipped elsewhere.
 *
 * Verifies two guarantees:
 *  1. crash_safe mode: every line has already reached the kernel when
 *     log_log returns, so even _exit() (no stdio flush) keeps all lines.
 *  2. log_install_crash_handler: a fatal signal leaves a marker line in
 *     the handler files and the process still dies from that signal.
 */

#include "test_harness.h"
#include "log.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>

#define CRASH_LOG_A "/tmp/test_crash_safe.log"
#define CRASH_LOG_B "/tmp/test_crash_marker.log"

static char *read_file_all(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Child body: log 50 lines in crash-safe mode and _exit() WITHOUT any
 * flush. Only the per-line fflush policy makes these lines survive. */
static void test_crash_safe_survives_exit(void) {
    remove(CRASH_LOG_A);

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        log_handle *ctx = log_create();
        int idx = log_add_file(ctx, CRASH_LOG_A, LOG_TRACE);
        if (idx < 0) _exit(43);
        ctx->handlers[0].active = false;   /* silence stderr handler */
        if (log_set_crash_safe(ctx, true) != 0) _exit(44);
        for (int i = 0; i < 50; i++) {
            log_ctx_info(ctx, "pre-exit %d", i);
        }
        _exit(0);   /* deliberate: no atexit, no stdio flush */
    }

    int status = 0;
    TEST_ASSERT_EQ(waitpid(pid, &status, 0), pid, "waitpid");
    TEST_ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exited cleanly");

    char *content = read_file_all(CRASH_LOG_A);
    TEST_ASSERT_NOT_NULL(content, "log file readable");
    TEST_ASSERT(strstr(content, "pre-exit 0") != NULL, "first line survived");
    TEST_ASSERT(strstr(content, "pre-exit 49") != NULL, "last line survived");
    int count = 0;
    for (const char *p = content; (p = strstr(p, "pre-exit ")) != NULL; p++) count++;
    TEST_ASSERT_EQ(count, 50, "all 50 lines survived _exit without flush");
    free(content);

    remove(CRASH_LOG_A);
    TEST_PASS("crash safe survives exit without flush");
}

/* Child body: crash-safe + installed handler, then die from SIGSEGV. */
static void test_crash_marker_on_fatal_signal(void) {
    remove(CRASH_LOG_B);

    pid_t pid = fork();
    TEST_ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        log_handle *ctx = log_create();
        int idx = log_add_file(ctx, CRASH_LOG_B, LOG_TRACE);
        if (idx < 0) _exit(43);
        ctx->handlers[0].active = false;
        if (log_set_crash_safe(ctx, true) != 0) _exit(44);
        if (log_install_crash_handler(ctx) != 0) _exit(45);
        for (int i = 0; i < 50; i++) {
            log_ctx_info(ctx, "pre-crash %d", i);
        }
        raise(SIGSEGV);
        _exit(46);   /* not reached: default disposition kills us */
    }

    int status = 0;
    TEST_ASSERT_EQ(waitpid(pid, &status, 0), pid, "waitpid");
    TEST_ASSERT(WIFSIGNALED(status), "child died from a signal");
    TEST_ASSERT_EQ(WTERMSIG(status), SIGSEGV, "signal was SIGSEGV (core semantics kept)");

    char *content = read_file_all(CRASH_LOG_B);
    TEST_ASSERT_NOT_NULL(content, "log file readable");
    TEST_ASSERT(strstr(content, "pre-crash 49") != NULL, "last pre-crash line present");
    TEST_ASSERT(strstr(content, "fatal signal") != NULL, "crash marker present");
    free(content);

    remove(CRASH_LOG_B);
    TEST_PASS("crash marker on fatal signal");
}

static void test_crash_safe_blocks_async(void) {
    log_handle *ctx = log_create();
    TEST_ASSERT_EQ(log_set_crash_safe(ctx, true), 0, "enable crash safe");
    TEST_ASSERT_EQ(log_set_async(ctx, true), -1, "async refused in crash-safe mode");
    TEST_ASSERT_EQ(log_set_crash_safe(ctx, false), 0, "disable crash safe");
    TEST_ASSERT_EQ(log_set_async(ctx, true), 0, "async allowed again");
    log_set_async(ctx, false);
    log_destroy(ctx);
    TEST_PASS("crash safe blocks async");
}

static void test_crash_safe_enables_flush_policy(void) {
    const char *path = "/tmp/test_crash_policy.log";
    remove(path);

    log_handle *ctx = log_create();
    int idx = log_add_file(ctx, path, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add file handler");
    ctx->handlers[0].active = false;

    TEST_ASSERT_EQ(log_set_crash_safe(ctx, true), 0, "enable");
    TEST_ASSERT_EQ(ctx->handlers[idx].flush_policy, LOG_FLUSH_EVERY,
                   "crash mode switches handlers to per-line flush");

    log_ctx_info(ctx, "visible immediately");
    log_set_crash_safe(ctx, false);

    char *content = read_file_all(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "visible immediately") != NULL,
                "crash-mode line visible without close");
    free(content);

    log_destroy(ctx);
    remove(path);
    TEST_PASS("crash safe enables flush policy");
}

static void test_crash_install_without_files(void) {
    log_handle *ctx = log_create();
    ctx->handlers[0].active = false;
    /* No file handlers: installing must succeed and simply target nothing. */
    TEST_ASSERT_EQ(log_install_crash_handler(ctx), 0, "install with no file targets");
    log_ctx_info(ctx, "still logging");
    log_destroy(ctx);
    TEST_PASS("crash install without files");
}

void test_crash_register(void) {
    test_add(test_crash_safe_survives_exit, "crash_safe_survives_exit");
    test_add(test_crash_marker_on_fatal_signal, "crash_marker_on_fatal_signal");
    test_add(test_crash_safe_blocks_async, "crash_safe_blocks_async");
    test_add(test_crash_safe_enables_flush_policy, "crash_safe_enables_flush_policy");
    test_add(test_crash_install_without_files, "crash_install_without_files");
}

#else /* Windows: signals/fork unavailable */

static void test_crash_skip(void) {
    TEST_SKIP("crash tests require fork/POSIX signals");
}

void test_crash_register(void) {
    test_add(test_crash_skip, "crash_safe_skipped");
}

#endif
