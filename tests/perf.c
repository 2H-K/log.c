/**
 * perf.c - Performance benchmark runner
 * Runs: bench_sync, bench_async
 *
 * Note: Test files are included directly to share static state.
 */

#include "test_harness.h"

#include "perf/bench_sync.c"
#include "perf/bench_async.c"

int main(void) {
    extern void bench_sync_register(void);
    extern void bench_async_register(void);

    bench_sync_register();
    bench_async_register();
    return test_run_all() ? 1 : 0;
}
