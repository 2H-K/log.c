/**
 * test_flush.c - Per-handler flush/fsync policy tests (A4 roadmap)
 * Platform: All (POSIX-only assertions guarded)
 */

#include "test_harness.h"
#include "log.h"

#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <time.h>

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Read the whole file into a heap buffer (caller frees). */
static char *read_file(const char *path) {
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
#endif

static void test_flush_every_visible_without_close(void) {
#if !defined(_WIN32) && !defined(_WIN64)
    const char *path = "/tmp/test_flush_every.log";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen writer");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add handler");

    /* Deactivate the default stderr handler to keep output clean. */
    ctx->handlers[0].active = false;

    TEST_ASSERT_EQ(log_handler_set_flush(ctx, idx, LOG_FLUSH_EVERY, 0), 0, "set EVERY");

    log_ctx_info(ctx, "durable line one");
    log_ctx_info(ctx, "durable line two");

    /* No fclose, no fflush by the test: policy must have flushed already. */
    char *content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "durable line one") != NULL, "line one visible");
    TEST_ASSERT(strstr(content, "durable line two") != NULL, "line two visible");
    free(content);

    log_destroy(ctx);
    fclose(fp);
    remove(path);
#endif
    TEST_PASS("flush every visible without close");
}

static void test_flush_default_buffers(void) {
#if !defined(_WIN32) && !defined(_WIN64)
    const char *path = "/tmp/test_flush_never.log";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen writer");
    log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;

    /* Default policy: one short line stays in the stdio buffer. */
    log_ctx_info(ctx, "buffered line");

    char *content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "buffered line") == NULL,
                "default policy keeps the line in userspace buffer");
    free(content);

    log_destroy(ctx);   /* destroy flushes owned... this fp is not owned */
    fclose(fp);         /* fclose flushes: now it must be on disk */

    content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read back after fclose");
    TEST_ASSERT(strstr(content, "buffered line") != NULL, "visible after fclose");
    free(content);
    remove(path);
#endif
    TEST_PASS("flush default buffers");
}

static void test_flush_interval_lazy_sync(void) {
#if !defined(_WIN32) && !defined(_WIN64)
    const char *path = "/tmp/test_flush_interval.log";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen writer");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;

    TEST_ASSERT_EQ(log_handler_set_flush(ctx, idx, LOG_FLUSH_INTERVAL, 50), 0, "set INTERVAL");

    log_ctx_info(ctx, "interval line one");
    /* Sync mode applies INTERVAL lazily on the next write. */
    char *content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "interval line one") == NULL,
                "first line not yet flushed (interval not elapsed)");
    free(content);

    msleep(80);
    log_ctx_info(ctx, "interval line two");

    content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "interval line one") != NULL,
                "line one flushed with line two");
    TEST_ASSERT(strstr(content, "interval line two") != NULL,
                "line two flushed");
    free(content);

    log_destroy(ctx);
    fclose(fp);
    remove(path);
#endif
    TEST_PASS("flush interval lazy sync");
}

static void test_flush_interval_async_timer(void) {
#if !defined(_WIN32) && !defined(_WIN64)
    const char *path = "/tmp/test_flush_interval_async.log";
    remove(path);

    log_handle *ctx = log_create();
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen writer");
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    ctx->handlers[0].active = false;

    TEST_ASSERT_EQ(log_handler_set_flush(ctx, idx, LOG_FLUSH_INTERVAL, 50), 0, "set INTERVAL");
    TEST_ASSERT_EQ(log_set_async(ctx, true), 0, "async on");

    log_ctx_info(ctx, "timer line");

    /* No further writes: the writer thread's interval timer must flush. */
    msleep(200);

    /* Drain the queue before reading (writer may still hold the line). */
    log_set_async(ctx, false);

    char *content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "timer line") != NULL,
                "interval timer flushed without new writes");
    free(content);

    log_destroy(ctx);
    fclose(fp);
    remove(path);
#endif
    TEST_PASS("flush interval async timer");
}

static void test_flush_fsync_roundtrip(void) {
#if !defined(_WIN32) && !defined(_WIN64)
    const char *path = "/tmp/test_flush_fsync.log";
    remove(path);

    log_handle *ctx = log_create();
    int idx = log_add_file(ctx, path, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add file handler");
    ctx->handlers[0].active = false;

    TEST_ASSERT_EQ(log_handler_set_fsync(ctx, idx, true), 0, "set fsync");
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, idx, LOG_FLUSH_EVERY, 0), 0, "set EVERY");

    log_ctx_info(ctx, "fsynced line");
    msleep(10);

    char *content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read back");
    TEST_ASSERT(strstr(content, "fsynced line") != NULL, "fsynced line on disk");
    free(content);

    log_destroy(ctx);
    remove(path);
#endif
    TEST_PASS("flush fsync roundtrip");
}

static void test_flush_invalid_args(void) {
    log_handle *ctx = log_create();

    TEST_ASSERT_EQ(log_handler_set_flush(ctx, -1, LOG_FLUSH_EVERY, 0), -1, "bad idx");
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, 99, LOG_FLUSH_EVERY, 0), -1, "idx out of range");
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, 0, 99, 0), -1, "bad policy");
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, 0, LOG_FLUSH_INTERVAL, 0), -1, "zero interval");
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, 0, LOG_FLUSH_INTERVAL, 100), 0, "valid interval");
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, 0, LOG_FLUSH_NEVER, 0), 0, "never ok");
    TEST_ASSERT_EQ(log_handler_set_fsync(ctx, -1, true), -1, "fsync bad idx");
    TEST_ASSERT_EQ(log_handler_set_fsync(ctx, 0, true), 0, "fsync ok");

    log_destroy(ctx);
    TEST_PASS("flush invalid args");
}

static void test_flush_rotation_with_fsync(void) {
#if !defined(_WIN32) && !defined(_WIN64)
    /* Rotation contract: the handler's file path must equal file_prefix;
     * log_rotate renames prefix -> prefix.1 and reopens prefix. */
    const char *path = "/tmp/test_flush_rot";
    remove(path);
    remove("/tmp/test_flush_rot.1");

    log_handle *ctx = log_create();
    log_set_file_prefix(ctx, path);
    int idx = log_add_file(ctx, path, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add file handler");
    ctx->handlers[0].active = false;

    TEST_ASSERT_EQ(log_handler_set_fsync(ctx, idx, true), 0, "set fsync");
    /* fsync applies after a flush, so a flush policy is required too. */
    TEST_ASSERT_EQ(log_handler_set_flush(ctx, idx, LOG_FLUSH_EVERY, 0), 0, "set EVERY");

    log_ctx_info(ctx, "before rotate");
    log_rotate(ctx);
    log_ctx_info(ctx, "after rotate");

    char *content = read_file(path);
    TEST_ASSERT_NOT_NULL(content, "read rotated file");
    TEST_ASSERT(strstr(content, "after rotate") != NULL, "post-rotate line present");
    free(content);

    FILE *old = fopen("/tmp/test_flush_rot.1", "r");
    TEST_ASSERT_NOT_NULL(old, "rotated file exists");
    if (old) fclose(old);

    log_destroy(ctx);
    remove(path);
    remove("/tmp/test_flush_rot.1");
#endif
    TEST_PASS("flush rotation with fsync");
}

void test_flush_register(void) {
    test_add(test_flush_every_visible_without_close, "flush_every_visible");
    test_add(test_flush_default_buffers, "flush_default_buffers");
    test_add(test_flush_interval_lazy_sync, "flush_interval_lazy_sync");
    test_add(test_flush_interval_async_timer, "flush_interval_async_timer");
    test_add(test_flush_fsync_roundtrip, "flush_fsync_roundtrip");
    test_add(test_flush_invalid_args, "flush_invalid_args");
    test_add(test_flush_rotation_with_fsync, "flush_rotation_with_fsync");
}
