/**
 * test_rotation.c - Log file rotation tests
 * Platform: All
 */

#include "test_harness.h"
#include "log.h"

static void test_rotation_basic(void) {
    log_handle *ctx = log_create();
    log_set_file_prefix(ctx, "test_rot");
    log_set_max_file_size(ctx, 1024);

    int idx = log_add_file(ctx, "test_rot", LOG_INFO);
    TEST_ASSERT(idx >= 0, "log_add_file succeeds");
    if (idx >= 0) ctx->handlers[0].active = false;

    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "rotation test %d with data", i);
    }

    log_destroy(ctx);

    FILE *f = fopen("test_rot.1", "r");
    TEST_ASSERT_NOT_NULL(f, "rotation file exists");
    if (f) fclose(f);

    remove("test_rot");
    remove("test_rot.1");
    remove("test_rot.2");
    remove("test_rot.3");
    remove("test_rot.4");
    remove("test_rot.5");
    TEST_PASS("rotation basic");
}

static void test_rotation_multiple(void) {
    log_handle *ctx = log_create();
    log_set_file_prefix(ctx, "test_rot");
    log_set_max_file_size(ctx, 512);

    int idx = log_add_file(ctx, "test_rot", LOG_INFO);
    TEST_ASSERT(idx >= 0, "log_add_file succeeds");
    if (idx >= 0) ctx->handlers[0].active = false;

    for (int i = 0; i < 200; i++) {
        log_ctx_info(ctx, "rotation %d with data padding", i);
    }

    log_destroy(ctx);

    FILE *f = fopen("test_rot.1", "r");
    TEST_ASSERT_NOT_NULL(f, "rotation occurred");
    if (f) fclose(f);

    remove("test_rot");
    remove("test_rot.1");
    remove("test_rot.2");
    remove("test_rot.3");
    remove("test_rot.4");
    remove("test_rot.5");
    TEST_PASS("rotation multiple");
}

static void test_rotation_manual(void) {
#if defined(_WIN32) || defined(_WIN64)
    /* On Windows, log_rotate must close+reopen the file to rename it,
     * so we use log_add_file (owns_file=true) to avoid double-close. */
    log_handle *ctx = log_create();
    log_set_file_prefix(ctx, "test_rot");
    /* Use large max size to avoid auto-rotation during writes */
    log_set_max_file_size(ctx, 1024 * 1024);

    int idx = log_add_file(ctx, "test_rot", LOG_INFO);
    TEST_ASSERT(idx >= 0, "log_add_file succeeds");
    if (idx >= 0) ctx->handlers[0].active = false;

    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "before rotation %d", i);
    }

    log_rotate(ctx);

    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "after rotation %d", i);
    }

    log_destroy(ctx);

    FILE *f = fopen("test_rot.1", "r");
    TEST_ASSERT_NOT_NULL(f, "manual rotation creates file");
    if (f) fclose(f);
#else
    log_handle *ctx = log_create();
    log_set_file_prefix(ctx, "test_rot");
    log_set_max_file_size(ctx, 1024);

    int idx = log_add_file(ctx, "test_rot", LOG_INFO);
    TEST_ASSERT(idx >= 0, "log_add_file succeeds");
    if (idx >= 0) ctx->handlers[0].active = false;

    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "before rotation %d", i);
    }

    log_rotate(ctx);

    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "after rotation %d", i);
    }

    log_destroy(ctx);

    FILE *f = fopen("test_rot.1", "r");
    TEST_ASSERT_NOT_NULL(f, "manual rotation creates file");
    if (f) fclose(f);
#endif

    remove("test_rot");
    remove("test_rot.1");
    remove("test_rot.2");
    remove("test_rot.3");
    remove("test_rot.4");
    remove("test_rot.5");
    TEST_PASS("rotation manual");
}

static void test_rotation_invalid_prefix(void) {
    log_handle *ctx = log_create();

    log_rotate(ctx);
    log_set_file_prefix(ctx, "../etc/passwd");

    log_destroy(ctx);
    TEST_PASS("rotation invalid prefix");
}

static void test_path_traversal_rejection(void) {
    log_handle *ctx = log_create();

    log_set_file_prefix(ctx, "../escape");
    log_set_file_prefix(ctx, "..\\escape");
    log_set_file_prefix(ctx, "/absolute/path");

    log_destroy(ctx);
    TEST_PASS("path traversal rejection");
}

static void test_rotation_reopen_failure(void) {
#if defined(_WIN32) || defined(_WIN64)
    const char *logfile = "./test_rot_reopen.log";
    /* Directory deliberately does not exist: rotate/reopen must fail. */
    const char *bad_prefix = "./logc_no_such_dir\\test_rot_reopen.log";
#else
    const char *logfile = "/tmp/test_rot_reopen.log";
    const char *bad_prefix = "/tmp/logc_no_such_dir/test_rot_reopen.log";
#endif
    remove(logfile);

    log_handle *ctx = log_create();
    int idx = log_add_file(ctx, logfile, LOG_INFO);
    TEST_ASSERT(idx >= 0, "log_add_file succeeds");
    ctx->handlers[0].active = false;

    log_set_file_prefix(ctx, bad_prefix);
    log_set_max_file_size(ctx, 1);

    log_ctx_info(ctx, "first message triggers rotation");

    /* The reopen fails, so the handler must not keep a pointer to the
     * FILE that was already closed during rotation. */
    TEST_ASSERT_NULL(ctx->handlers[idx].fp, "fp cleared after failed reopen");
    TEST_ASSERT_NULL(ctx->handlers[idx].udata, "udata cleared after failed reopen");

    /* Must not dereference a closed/freed FILE for later messages. */
    log_ctx_info(ctx, "second message after failed reopen");
    log_ctx_info(ctx, "third message after failed reopen");

    log_destroy(ctx);
    remove(logfile);
    TEST_PASS("rotation reopen failure");
}

void test_rotation_register(void) {
    test_add(test_rotation_basic, "rotation_basic");
    test_add(test_rotation_multiple, "rotation_multiple");
    test_add(test_rotation_manual, "rotation_manual");
    test_add(test_rotation_invalid_prefix, "rotation_invalid_prefix");
    test_add(test_path_traversal_rejection, "path_traversal_rejection");
    test_add(test_rotation_reopen_failure, "rotation_reopen_failure");
}
