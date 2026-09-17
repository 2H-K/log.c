/**
 * perf.c - Performance benchmark runner
 * Runs: bench_sync, bench_async
 *
 * Note: Test files are included directly to share static state.
 */

#include "test_harness.h"

#include "perf/bench_sync.c"
#include "perf/bench_async.c"
#include "perf/bench_durability.c"
#include "perf/bench_latency.c"

int main(void) {
    extern void bench_sync_register(void);
    extern void bench_async_register(void);
    extern void bench_durability_register(void);
    extern void bench_latency_register(void);

    bench_sync_register();
    bench_async_register();
    bench_durability_register();
    bench_latency_register();
    return test_run_all() ? 1 : 0;
}
