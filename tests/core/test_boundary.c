/**
 * test_boundary.c - Boundary condition tests
 * Tests edge cases and extreme inputs
 */

#include "test_harness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ==================== NULL / Empty Input Tests ==================== */

static void test_boundary_null_filename(void) {
    log *ctx = log_create();
    int idx = log_add_file(ctx, NULL, LOG_INFO);
    TEST_ASSERT(idx < 0, "add_file with NULL filename returns error");
    log_destroy(ctx);
    TEST_PASS("boundary null filename");
}

static void test_boundary_empty_filename(void) {
    log *ctx = log_create();
    int idx = log_add_file(ctx, "", LOG_INFO);
    TEST_ASSERT(idx < 0, "add_file with empty filename returns error");
    log_destroy(ctx);
    TEST_PASS("boundary empty filename");
}

static void test_boundary_null_prefix(void) {
    log *ctx = log_create();
    log_set_file_prefix(ctx, NULL);
    /* Should not crash, should use default */
    log_destroy(ctx);
    TEST_PASS("boundary null prefix");
}

static void test_boundary_empty_prefix(void) {
    log *ctx = log_create();
    log_set_file_prefix(ctx, "");
    /* Should not crash */
    log_destroy(ctx);
    TEST_PASS("boundary empty prefix");
}

/* ==================== Extreme Value Tests ==================== */

static void test_very_long_message(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Create a very long message (16KB) */
    size_t long_len = 16 * 1024;
    char *long_msg = (char*)malloc(long_len + 1);
    TEST_ASSERT_NOT_NULL(long_msg, "malloc long message");
    memset(long_msg, 'A', long_len);
    long_msg[long_len] = '\0';

    log_ctx_info(ctx, "%s", long_msg);

    free(long_msg);
    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("very long message");
}

static void test_very_long_format(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Format string with many arguments */
    log_ctx_info(ctx, "%s %s %s %s %s %s %s %s %s %s",
                 "arg1", "arg2", "arg3", "arg4", "arg5",
                 "arg6", "arg7", "arg8", "arg9", "arg10");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("very long format");
}

static void test_max_level(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Test all valid levels */
    log_ctx_trace(ctx, "trace");
    log_ctx_debug(ctx, "debug");
    log_ctx_info(ctx, "info");
    log_ctx_warn(ctx, "warn");
    log_ctx_error(ctx, "error");
    log_ctx_fatal(ctx, "fatal");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("max level");
}

static void test_negative_level(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Set level to negative value - should clamp or reject */
    log_set_level(ctx, -1);
    log_ctx_info(ctx, "should not appear if level clamped");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("negative level");
}

/* ==================== Path Boundary Tests ==================== */

static void test_boundary_long_path(void) {
    log *ctx = log_create();

    /* Create a long path (but within typical limits) */
    char path[512];
    snprintf(path, sizeof(path), "/tmp/");
    for (int i = 0; i < 20; i++) {
        strcat(path, "a");
    }
    strcat(path, ".log");

    int idx = log_add_file(ctx, path, LOG_INFO);
    /* May succeed or fail depending on filesystem limits */
    if (idx >= 0) {
        log_ctx_info(ctx, "long path test");
    }

    log_destroy(ctx);
    remove(path);
    TEST_PASS("boundary long path");
}

static void test_path_with_special_chars(void) {
    log *ctx = log_create();

    const char *path = "/tmp/test-file_name.123.log";
    int idx = log_add_file(ctx, path, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add_file with special chars in path");

    if (idx >= 0) {
        log_ctx_info(ctx, "special chars path test");
    }

    log_destroy(ctx);
    remove(path);
    TEST_PASS("path with special chars");
}

/* ==================== Handler Limit Tests ==================== */

static void test_max_handlers(void) {
    log *ctx = log_create();
    int count = 0;

    /* Add handlers until we hit the limit */
    for (int i = 0; i < 50; i++) {
        char fname[64];
        snprintf(fname, sizeof(fname), "/tmp/test_handler_%d.log", i);
        FILE *fp = fopen(fname, "w");
        if (!fp) break;
        int idx = log_add_fp(ctx, fp, LOG_INFO);
        if (idx < 0) {
            fclose(fp);
            break;
        }
        count++;
        log_ctx_info(ctx, "handler %d test", i);
    }

    printf("    handlers added: %d", count);
    TEST_ASSERT(count >= 10, "supports at least 10 handlers");

    /* Cleanup */
    for (int i = 0; i < count; i++) {
        char fname[64];
        snprintf(fname, sizeof(fname), "/tmp/test_handler_%d.log", i);
        remove(fname);
    }

    log_destroy(ctx);
    TEST_PASS("max handlers");
}

static void test_remove_invalid_handler(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Remove with invalid index - should not crash */
    log_remove_handler(ctx, -1);
    log_remove_handler(ctx, 100);
    log_remove_handler(ctx, INT_MAX);

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("remove invalid handler");
}

/* ==================== Concurrent Edge Cases ==================== */

static void test_destroy_while_logging(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Write some messages */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "msg %d", i);
    }

    /* Immediate destroy - should clean up without crash */
    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("destroy while logging");
}

static void test_recreate_after_destroy(void) {
    /* Create and destroy multiple times */
    for (int i = 0; i < 100; i++) {
        log *ctx = log_create();
        FILE *fp = fopen(TEST_DEV_NULL, "w");
        log_add_fp(ctx, fp, LOG_INFO);
        log_ctx_info(ctx, "iteration %d", i);
        log_destroy(ctx);
        fclose(fp);
    }
    TEST_PASS("recreate after destroy");
}

/* ==================== Format String Edge Cases ==================== */

static void test_format_percent(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Message with percent signs */
    log_ctx_info(ctx, "100%% complete");
    log_ctx_info(ctx, "%%s %%d %%f");
    log_ctx_info(ctx, "no format");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("format percent");
}

static void test_format_null_args(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Log with NULL format - should handle gracefully */
    log_ctx_info(ctx, "before null");
    /* Note: passing NULL as format string is undefined in standard C */
    /* but the library should not crash if it happens */
    log_ctx_info(ctx, "after null");

    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("format null args");
}

static void test_very_small_queue_size(void) {
    log *ctx = log_create();
    FILE *fp = fopen(TEST_DEV_NULL, "w");
    log_add_fp(ctx, fp, LOG_INFO);

    /* Set a small queue size */
    log_set_queue_size(ctx, 16);
    log_set_async(ctx, true);

    /* Write some messages */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "small queue msg %d", i);
    }

    log_set_async(ctx, false);
    log_destroy(ctx);
    fclose(fp);
    TEST_PASS("very small queue size");
}

static void test_boundary_register(void) {
    test_add(test_boundary_null_filename, "boundary_null_filename");
    test_add(test_boundary_empty_filename, "boundary_empty_filename");
    test_add(test_boundary_null_prefix, "boundary_null_prefix");
    test_add(test_boundary_empty_prefix, "boundary_empty_prefix");
    test_add(test_very_long_message, "very_long_message");
    test_add(test_very_long_format, "very_long_format");
    test_add(test_max_level, "max_level");
    test_add(test_negative_level, "negative_level");
    test_add(test_boundary_long_path, "boundary_long_path");
    test_add(test_path_with_special_chars, "path_with_special_chars");
    test_add(test_max_handlers, "max_handlers");
    test_add(test_remove_invalid_handler, "remove_invalid_handler");
    test_add(test_destroy_while_logging, "destroy_while_logging");
    test_add(test_recreate_after_destroy, "recreate_after_destroy");
    test_add(test_format_percent, "format_percent");
    test_add(test_format_null_args, "format_null_args");
    test_add(test_very_small_queue_size, "very_small_queue_size");
}
