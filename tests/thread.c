/**
 * thread.c - Thread safety test runner
 * Runs: mt_sync, mt_async, config_race
 *
 * Note: Test files are included directly to share static state
 * from test_harness.h. Each category compiles as a single unit.
 */

#include "test_harness.h"

#include "thread/test_mt_sync.c"
#include "thread/test_mt_async.c"
#include "thread/test_config_race.c"

int main(void) {
    extern void test_mt_sync_register(void);
    extern void test_mt_async_register(void);
    extern void test_config_race_register(void);

    test_mt_sync_register();
    test_mt_async_register();
    test_config_race_register();
    return test_run_all() ? 1 : 0;
}
