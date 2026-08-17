/**
 * core.c - Core functionality test runner
 * Runs: levels, handlers, format, null safety, stats, boundary
 *
 * Note: Test files are included directly to share static state
 * from test_harness.h. Each category compiles as a single unit.
 */

#include "test_harness.h"

#include "core/test_levels.c"
#include "core/test_handlers.c"
#include "core/test_format.c"
#include "core/test_null.c"
#include "core/test_stats.c"
#include "core/test_boundary.c"

int main(void) {
    extern void test_levels_register(void);
    extern void test_handlers_register(void);
    extern void test_format_register(void);
    extern void test_null_register(void);
    extern void test_stats_register(void);
    extern void test_boundary_register(void);

    test_levels_register();
    test_handlers_register();
    test_format_register();
    test_null_register();
    test_stats_register();
    test_boundary_register();
    return test_run_all() ? 1 : 0;
}
