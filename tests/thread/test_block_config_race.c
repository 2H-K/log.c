/**
 * test_block_config_race.c - BLOCK queue policy + concurrent config change
 * Platform: All
 *
 * Regression guard: with LOG_QUEUE_BLOCK, a producer whose queue is full
 * blocks waiting for space. The old code held the ctx read lock across the
 * blocking enqueue; a concurrent configuration change (write lock) then
 * deadlocked: the blocked producer never released the read lock, the async
 * writer thread's read lock was queued behind the pending write lock, the
 * queue was never drained and space_cond was never signalled.
 *
 * A slow sink makes the writer thread hold the read lock long enough to
 * provoke the deadlock deterministically: the config thread must complete
 * while producers are blocked on a full queue.
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

/* ~1ms per message: the writer thread holds the ctx read lock long enough
 * that a full queue plus write-preference makes the config thread starve. */
static void slow_block_sink(log_handle *ctx, log_event *ev) {
    (void)ctx;
    (void)ev;
    uint64_t t0 = test_now_ns();
    while (test_now_ns() - t0 < 1000000ULL) { /* spin ~1ms */ }
}

static volatile int g_block_done = 0;
static log_handle *g_block_ctx = NULL;

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI block_producer(LPVOID arg) {
#else
static void* block_producer(void *arg) {
#endif
    (void)arg;
    int i = 0;
    while (!g_block_done) {
        log_ctx_info(g_block_ctx, "block race msg %d "
                      "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i++);
    }
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI block_config_changer(LPVOID arg) {
#else
static void* block_config_changer(void *arg) {
#endif
    (void)arg;
    /* Give producers time to fill the queue. */
    uint64_t t0 = test_now_ns();
    while (test_now_ns() - t0 < 500000000ULL) { /* spin ~500ms */ }
    log_set_level(g_block_ctx, LOG_WARN);
    log_set_level(g_block_ctx, LOG_TRACE);
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static void test_block_policy_config_race(void) {
    g_block_done = 0;

    log_handle *ctx = log_create();
    TEST_ASSERT_NOT_NULL(ctx, "log_create");
    g_block_ctx = ctx;

    /* Replace the default stderr handler with a slow sink so the writer
     * thread holds the read lock for a long time. */
    log_remove_handler(ctx, 0);
    int idx = log_add_handler(ctx, slow_block_sink, NULL, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add slow sink");

    /* Tiny queue: overflow is guaranteed under the producer burst. */
    log_set_queue_size(ctx, 8);
    log_set_queue_policy(ctx, LOG_QUEUE_BLOCK);
    log_set_async(ctx, true);

    THREAD_T producers[NUM_THREADS];
    THREAD_T config_th;
    THREAD_CREATE(config_th, block_config_changer, NULL);
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_CREATE(producers[i], block_producer, NULL);
    }

    /* The config thread must finish promptly. Wait up to ~3s; if the
     * deadlock regression exists it never returns. */
    uint64_t t0 = test_now_ns();
    THREAD_JOIN(config_th);
    uint64_t elapsed = (test_now_ns() - t0) / 1000000ULL;
    TEST_ASSERT(elapsed < 3000, "config change completed without deadlock");

    g_block_done = 1;
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(producers[i]);
    }

    log_set_async(ctx, false);
    log_destroy(ctx);
    g_block_ctx = NULL;
    TEST_PASS("block policy config race");
}

void test_block_config_race_register(void) {
    test_add(test_block_policy_config_race, "block_policy_config_race");
}
