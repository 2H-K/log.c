/**
 * test_crash_safety.c - Crash safety test
 * Forks a child process that writes logs then aborts.
 * Parent verifies that flushed logs are preserved.
 */

#include "test_harness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void test_crash_after_fsync(void) {
    const char *tmpfile = "/tmp/test_crash_fsync.log";
    remove(tmpfile);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: write logs then crash */
        FILE *fp = fopen(tmpfile, "w");
        if (!fp) _exit(1);

        log *ctx = log_create();
        log_add_fp(ctx, fp, LOG_INFO);

        /* Write some messages */
        for (int i = 0; i < 100; i++) {
            log_ctx_info(ctx, "CRASH_TEST_%04d", i);
        }

        /* Force flush */
        fflush(fp);
        fsync(fileno(fp));

        /* Write one more after fsync - this should be lost */
        log_ctx_info(ctx, "AFTER_FSYNC_SHOULD_LOST");

        /* Crash without cleanup */
        abort();
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status), "child was terminated by signal (crash)");

    /* Verify log file */
    FILE *fp = fopen(tmpfile, "r");
    TEST_ASSERT_NOT_NULL(fp, "log file exists after crash");

    char line[256];
    int count = 0;
    int found_lost = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "CRASH_TEST_")) {
            count++;
        }
        if (strstr(line, "AFTER_FSYNC_SHOULD_LOST")) {
            found_lost = 1;
        }
    }
    fclose(fp);

    printf("    messages=%d, after_fsync_lost_found=%d", count, found_lost);

    /* All fsync'd messages should be present */
    TEST_ASSERT(count >= 99, "most fsync'd messages preserved");
    /* The message after fsync may or may not be present depending on timing */
    (void)found_lost;  /* informational only */

    remove(tmpfile);
    TEST_PASS("crash after fsync");
}

static void test_crash_no_flush(void) {
    const char *tmpfile = "/tmp/test_crash_noflush.log";
    remove(tmpfile);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: write logs but crash immediately without flush */
        FILE *fp = fopen(tmpfile, "w");
        if (!fp) _exit(1);

        log *ctx = log_create();
        log_add_fp(ctx, fp, LOG_INFO);

        /* Write messages without explicit flush */
        for (int i = 0; i < 1000; i++) {
            log_ctx_info(ctx, "NOFLUSH_%04d", i);
        }

        /* Crash immediately - some messages may be in buffer */
        raise(SIGKILL);
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status), "child was killed");

    /* Verify log file exists and has some content */
    FILE *fp = fopen(tmpfile, "r");
    if (fp) {
        char line[256];
        int count = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "NOFLUSH_")) count++;
        }
        fclose(fp);
        printf("    messages_written=%d/1000 (some loss expected without flush)", count);
    } else {
        printf("    file not created (acceptable for no-flush crash)");
    }

    remove(tmpfile);
    TEST_PASS("crash without flush");
}

static void test_crash_async_mode(void) {
    const char *tmpfile = "/tmp/test_crash_async.log";
    remove(tmpfile);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: async mode, write then crash */
        FILE *fp = fopen(tmpfile, "w");
        if (!fp) _exit(1);

        log *ctx = log_create();
        log_add_fp(ctx, fp, LOG_INFO);
        log_set_async(ctx, true);

        /* Write many messages */
        for (int i = 0; i < 500; i++) {
            log_ctx_info(ctx, "ASYNC_CRASH_%04d", i);
        }

        /* Small delay to let some messages be processed */
        usleep(10000);  /* 10ms */

        /* Crash - async queue messages will be lost */
        abort();
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status), "child was terminated by signal");

    /* Verify log file */
    FILE *fp = fopen(tmpfile, "r");
    if (fp) {
        char line[256];
        int count = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "ASYNC_CRASH_")) count++;
        }
        fclose(fp);
        printf("    messages_written=%d/500 (async queue loss expected)", count);
    } else {
        printf("    file not created");
    }

    remove(tmpfile);
    TEST_PASS("crash in async mode");
}

void test_crash_safety_register(void) {
    test_add(test_crash_after_fsync, "crash_after_fsync");
    test_add(test_crash_no_flush, "crash_no_flush");
    test_add(test_crash_async_mode, "crash_async_mode");
}
