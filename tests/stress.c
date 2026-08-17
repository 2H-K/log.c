/**
 * stress.c - Stress test runner
 * Runs: queue_full, long_message, integrity, crash_safety, resource_usage
 *
 * Note: Test files are included directly to share static state.
 */

#include "test_harness.h"

#include "stress/test_queue_full.c"
#include "stress/test_long_message.c"
#include "stress/test_integrity.c"
#include "stress/test_crash_safety.c"
#include "stress/test_resource_usage.c"

int main(void) {
    extern void test_queue_full_register(void);
    extern void test_long_message_register(void);
    extern void test_integrity_register(void);
    extern void test_crash_safety_register(void);
    extern void test_resource_usage_register(void);

    test_queue_full_register();
    test_long_message_register();
    test_integrity_register();
    test_crash_safety_register();
    test_resource_usage_register();
    return test_run_all() ? 1 : 0;
}
