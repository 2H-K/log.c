/**
 * all.c - Unified test runner
 * Runs all tests from all categories
 *
 * Note: Test files are included directly to share static state.
 */

#include "test_harness.h"

/* Core tests */
#include "core/test_levels.c"
#include "core/test_handlers.c"
#include "core/test_format.c"
#include "core/test_null.c"
#include "core/test_stats.c"
#include "core/test_boundary.c"

/* Thread tests */
#include "thread/test_mt_sync.c"
#include "thread/test_mt_async.c"
#include "thread/test_config_race.c"

/* Platform tests */
#include "platform/test_syslog.c"
#include "platform/test_rotation.c"
#include "platform/test_unicode_path.c"

/* Stress tests */
#include "stress/test_queue_full.c"
#include "stress/test_long_message.c"
#include "stress/test_integrity.c"
#include "stress/test_crash_safety.c"
#include "stress/test_resource_usage.c"
#include "stress/test_rotation_stress.c"
#include "stress/test_malloc_detection.c"

/* Perf tests */
#include "perf/bench_sync.c"
#include "perf/bench_async.c"

int main(void) {
    /* Core */
    extern void test_levels_register(void);
    extern void test_handlers_register(void);
    extern void test_format_register(void);
    extern void test_null_register(void);
    extern void test_stats_register(void);
    extern void test_boundary_register(void);

    /* Thread */
    extern void test_mt_sync_register(void);
    extern void test_mt_async_register(void);
    extern void test_config_race_register(void);

    /* Platform */
    extern void test_syslog_register(void);
    extern void test_rotation_register(void);
    extern void test_unicode_path_register(void);

    /* Stress */
    extern void test_queue_full_register(void);
    extern void test_long_message_register(void);
    extern void test_integrity_register(void);
    extern void test_crash_safety_register(void);
    extern void test_resource_usage_register(void);
    extern void test_rotation_stress_register(void);
    extern void test_malloc_detection_register(void);

    /* Perf */
    extern void bench_sync_register(void);
    extern void bench_async_register(void);

    test_levels_register();
    test_handlers_register();
    test_format_register();
    test_null_register();
    test_stats_register();
    test_boundary_register();

    test_mt_sync_register();
    test_mt_async_register();
    test_config_race_register();

    test_syslog_register();
    test_rotation_register();
    test_unicode_path_register();

    test_queue_full_register();
    test_long_message_register();
    test_integrity_register();
    test_crash_safety_register();
    test_resource_usage_register();
    test_rotation_stress_register();
    test_malloc_detection_register();

    bench_sync_register();
    bench_async_register();

    return test_run_all() ? 1 : 0;
}
