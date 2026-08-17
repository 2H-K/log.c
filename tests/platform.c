/**
 * platform.c - Platform-specific test runner
 * Runs: syslog (POSIX), rotation
 *
 * Note: Test files are included directly to share static state.
 */

#include "test_harness.h"

#include "platform/test_syslog.c"
#include "platform/test_rotation.c"

int main(void) {
    extern void test_syslog_register(void);
    extern void test_rotation_register(void);

    test_syslog_register();
    test_rotation_register();
    return test_run_all() ? 1 : 0;
}
