/**
 * test_syslog.c - Syslog integration tests
 * Platform: POSIX only (Linux, macOS, BSD)
 * Windows: Tests are skipped
 */

#include "test_harness.h"
#include "log.h"

#if TEST_ON_POSIX()

#include <syslog.h>
/* LOG_CRIT might be defined as macro in log.h, undefine for syslog.h */
#ifdef LOG_CRIT
#undef LOG_CRIT
#endif

static void test_syslog_add_handler(void) {
    log *ctx = log_create();
    int idx = log_add_syslog_handler(ctx, "test_logc", LOG_USER, LOG_DEBUG);
    TEST_ASSERT(idx >= 0, "syslog handler added");
    log_destroy(ctx);
    TEST_PASS("syslog add handler");
}

static void test_syslog_log_message(void) {
    log *ctx = log_create();
    int idx = log_add_syslog_handler(ctx, "test_logc", LOG_USER, LOG_DEBUG);
    TEST_ASSERT(idx >= 0, "syslog handler added");

    log_ctx_trace(ctx, "syslog trace");
    log_ctx_info(ctx, "syslog info");
    log_ctx_error(ctx, "syslog error");

    log_destroy(ctx);
    TEST_PASS("syslog log message");
}

static void test_syslog_level_mapping(void) {
    /* Use actual syslog priority values */
    TEST_ASSERT_EQ(log_level_to_syslog(LOG_TRACE), 7, "trace->LOG_DEBUG(7)");
    TEST_ASSERT_EQ(log_level_to_syslog(LOG_DEBUG), 7, "debug->LOG_DEBUG(7)");
    TEST_ASSERT_EQ(log_level_to_syslog(LOG_INFO), 6, "info->LOG_INFO(6)");
    TEST_ASSERT_EQ(log_level_to_syslog(LOG_WARN), 4, "warn->LOG_WARNING(4)");
    TEST_ASSERT_EQ(log_level_to_syslog(LOG_ERROR), 3, "error->LOG_ERR(3)");
    TEST_ASSERT_EQ(log_level_to_syslog(LOG_FATAL), 2, "fatal->LOG_CRIT(2)");
    TEST_PASS("syslog level mapping");
}

void test_syslog_register(void) {
    test_add(test_syslog_add_handler, "syslog_add_handler");
    test_add(test_syslog_log_message, "syslog_log_message");
    test_add(test_syslog_level_mapping, "syslog_level_mapping");
}

#else

static void test_syslog_skip_posix(void) { TEST_SKIP("syslog not available on Windows"); }
static void test_syslog_skip_msg(void) { TEST_SKIP("syslog not available on Windows"); }
static void test_syslog_skip_level(void) { TEST_SKIP("syslog not available on Windows"); }

void test_syslog_register(void) {
    test_add(test_syslog_skip_posix, "syslog_add_handler");
    test_add(test_syslog_skip_msg, "syslog_log_message");
    test_add(test_syslog_skip_level, "syslog_level_mapping");
}

#endif
