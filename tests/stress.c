/**
 * stress.c - Stress test runner
 * Runs: queue_full, long_message
 *
 * Note: Test files are included directly to share static state.
 */

#include "test_harness.h"

#include "stress/test_queue_full.c"
#include "stress/test_long_message.c"

int main(void) {
    extern void test_queue_full_register(void);
    extern void test_long_message_register(void);

    test_queue_full_register();
    test_long_message_register();
    return test_run_all() ? 1 : 0;
}
