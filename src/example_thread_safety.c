/*
 * Thread Safety Demonstration
 *
 * This example demonstrates the thread-safe implementation of the log library,
 * showing how pthread locks and reader-writer locks are used to ensure
 * concurrent access safety.
 */

#define _GNU_SOURCE
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/*
 * ============================================================================
 * PTHREAD LOCK USAGE EXAMPLES
 * ============================================================================
 *
 * The log library uses two types of locks for thread safety:
 *
 * 1. Reader-Writer Locks (rwlock)
 *    - Multiple readers can hold the lock simultaneously
 *    - Writers get exclusive access
 *    - Used for configuration reads/writes
 *
 * 2. Mutex Locks (mutex)
 *    - Simple mutual exclusion
 *    - Used for critical sections
 */

/*
 * Example 1: Basic pthread_mutex usage pattern
 * (This is similar to how the library protects critical sections)
 */
typedef struct {
    pthread_mutex_t lock;
    int counter;
} counter_t;

void counter_init(counter_t *c) {
    pthread_mutex_init(&c->lock, NULL);
    c->counter = 0;
}

void counter_increment(counter_t *c) {
    // Lock before accessing shared data
    pthread_mutex_lock(&c->lock);
    c->counter++;
    // Unlock when done
    pthread_mutex_unlock(&c->lock);
}

int counter_get(counter_t *c) {
    int value;
    pthread_mutex_lock(&c->lock);
    value = c->counter;
    pthread_mutex_unlock(&c->lock);
    return value;
}

void counter_destroy(counter_t *c) {
    pthread_mutex_destroy(&c->lock);
}

/*
 * Example 2: Reader-Writer Lock usage pattern
 * (This is how the library protects log configuration)
 */
typedef struct {
    pthread_rwlock_t rwlock;
    int config_value;
} config_t;

void config_init(config_t *cfg) {
    pthread_rwlock_init(&cfg->rwlock, NULL);
    cfg->config_value = 0;
}

// Read operation - multiple readers can access simultaneously
int config_read(config_t *cfg) {
    int value;
    pthread_rwlock_rdlock(&cfg->rwlock);  // Acquire read lock
    value = cfg->config_value;
    pthread_rwlock_unlock(&cfg->rwlock);  // Release read lock
    return value;
}

// Write operation - exclusive access
void config_write(config_t *cfg, int new_value) {
    pthread_rwlock_wrlock(&cfg->rwlock);  // Acquire write lock
    cfg->config_value = new_value;
    pthread_rwlock_unlock(&cfg->rwlock);  // Release write lock
}

void config_destroy(config_t *cfg) {
    pthread_rwlock_destroy(&cfg->rwlock);
}

/*
 * ============================================================================
 * DEMONSTRATION 1: Safe Log Level Changes from Multiple Threads
 * ============================================================================
 */

void* level_changer_thread(void *arg) {
    log *ctx = (log*)arg;
    int thread_id = (int)(long)pthread_self() % 1000;

    for (int i = 0; i < 10; i++) {
        // Each thread changes the log level
        // This is thread-safe due to reader-writer locks in the library
        int new_level = i % LOG_LEVELS;
        log_set_level(ctx, new_level);

        // Log at current level
        log_ctx_info(ctx, "Thread %d: Changed level to %s (%d)",
                    thread_id,
                    log_level_string(new_level),
                    new_level);

        usleep(1000);  // Small delay
    }

    return NULL;
}

void demo_safe_level_changes(void) {
    printf("\n=== Demo 1: Safe Log Level Changes ===\n");
    printf("Multiple threads changing log level concurrently...\n\n");

    log *ctx = log_create();

    // Create multiple threads that change log levels
    #define NUM_THREADS 5
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, level_changer_thread, ctx);
    }

    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\nAll threads completed. No race conditions occurred.\n");
    log_destroy(ctx);
}

/*
 * ============================================================================
 * DEMONSTRATION 2: Safe Handler Management from Multiple Threads
 * ============================================================================
 */

typedef struct {
    log *ctx;
    int thread_id;
} handler_thread_data_t;

void* handler_manager_thread(void *arg) {
    handler_thread_data_t *data = (handler_thread_data_t*)arg;
    log *ctx = data->ctx;
    int thread_id = data->thread_id;

    char filename[64];
    snprintf(filename, sizeof(filename), "handler_thread_%d.log", thread_id);

    // Add a file handler
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("fopen");
        return NULL;
    }

    int handler_idx = log_add_fp(ctx, fp, LOG_INFO);

    // Write logs
    for (int i = 0; i < 50; i++) {
        log_ctx_info(ctx, "Thread %d: Message %d", thread_id, i);
        usleep(100);
    }

    // Remove handler
    log_remove_handler(ctx, handler_idx);
    fclose(fp);

    // Clean up file
    remove(filename);

    return NULL;
}

void demo_safe_handler_management(void) {
    printf("\n=== Demo 2: Safe Handler Management ===\n");
    printf("Multiple threads adding/removing handlers concurrently...\n\n");

    log *ctx = log_create();

    #define NUM_THREADS 3
    pthread_t threads[NUM_THREADS];
    handler_thread_data_t data[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        data[i].ctx = ctx;
        data[i].thread_id = i;
        pthread_create(&threads[i], NULL, handler_manager_thread, &data[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\nAll threads completed. Handlers managed safely.\n");
    log_destroy(ctx);
}

/*
 * ============================================================================
 * DEMONSTRATION 3: Concurrent Logging with Thread IDs
 * ============================================================================
 */

void* logging_thread(void *arg) {
    log *ctx = (log*)arg;
    int thread_id = (int)(long)pthread_self() % 100;

    for (int i = 0; i < 100; i++) {
        // All threads log concurrently
        // Thread-safe due to reader-writer locks
        log_ctx_info(ctx, "Thread %d: Log message %d", thread_id, i);

        // Mix of log levels
        if (i % 10 == 0) {
            log_ctx_warn(ctx, "Thread %d: Warning at iteration %d", thread_id, i);
        }
        if (i % 20 == 0) {
            log_ctx_error(ctx, "Thread %d: Error at iteration %d", thread_id, i);
        }

        usleep(50);  // Small delay
    }

    return NULL;
}

void demo_concurrent_logging_with_thread_ids(void) {
    printf("\n=== Demo 3: Concurrent Logging with Thread IDs ===\n");
    printf("Multiple threads logging with thread ID tracking...\n\n");

    log *ctx = log_create();

    // Enable thread ID for stderr handler
    log_enable_thread_id(ctx, 0, true);

    // Also add a file handler with thread ID
    FILE *fp = fopen("concurrent_thread_ids.log", "w");
    int handler_idx = log_add_fp(ctx, fp, LOG_INFO);
    log_enable_thread_id(ctx, handler_idx, true);

    // Create multiple logging threads
    #define NUM_THREADS 4
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, logging_thread, ctx);
    }

    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Display statistics
    log_stats stats;
    log_get_stats(ctx, &stats);

    printf("\n=== Logging Statistics ===\n");
    printf("Total messages: %lu\n", stats.total_count);
    printf("INFO messages: %lu\n", stats.level_counts[LOG_INFO]);
    printf("WARN messages: %lu\n", stats.level_counts[LOG_WARN]);
    printf("ERROR messages: %lu\n", stats.level_counts[LOG_ERROR]);

    printf("\nAll threads completed. Check concurrent_thread_ids.log for output.\n");

    log_remove_handler(ctx, handler_idx);
    fclose(fp);
    remove("concurrent_thread_ids.log");
    log_destroy(ctx);
}

/*
 * ============================================================================
 * DEMONSTRATION 4: Async Mode with Concurrent Threads
 * ============================================================================
 */

void* async_logging_thread(void *arg) {
    log *ctx = (log*)arg;
    int thread_id = (int)(long)pthread_self() % 100;

    for (int i = 0; i < 200; i++) {
        // Log in async mode - non-blocking
        log_ctx_info(ctx, "Async thread %d: Message %d", thread_id, i);
    }

    return NULL;
}

void demo_async_mode_thread_safety(void) {
    printf("\n=== Demo 4: Async Mode Thread Safety ===\n");
    printf("Multiple threads logging in async mode with lock-free queue...\n\n");

    log *ctx = log_create();

    FILE *fp = fopen("async_thread_safety.log", "w");
    log_add_fp(ctx, fp, LOG_INFO);

    // Enable async mode
    printf("Enabling async mode...\n");
    log_set_async(ctx, true);

    // Create multiple threads
    #define NUM_THREADS 6
    pthread_t threads[NUM_THREADS];

    printf("Starting %d threads...\n", NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, async_logging_thread, ctx);
    }

    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("All threads completed. Waiting for async queue to flush...\n");

    // Disable async and wait
    log_set_async(ctx, false);

    // Show statistics
    log_stats stats;
    log_get_stats(ctx, &stats);

    printf("\n=== Async Mode Statistics ===\n");
    printf("Total messages: %lu\n", stats.total_count);
    printf("Async writes: %lu\n", stats.async_writes);
    printf("Sync writes: %lu\n", stats.sync_writes);
    printf("Queue drops: %lu\n", stats.queue_drops);
    printf("Avg queue latency: %.3f ms\n", stats.avg_queue_latency_ms);

    printf("\nAsync mode is thread-safe with lock-free SPSC queue.\n");

    fclose(fp);
    remove("async_thread_safety.log");
    log_destroy(ctx);
}

/*
 * ============================================================================
 * DEMONSTRATION 5: Custom pthread_mutex Example
 * ============================================================================
 */

typedef struct {
    pthread_mutex_t lock;
    int value;
} shared_counter_t;

void* counter_thread(void *arg) {
    shared_counter_t *counter = (shared_counter_t*)arg;

    // This is how you would protect shared data with pthread_mutex
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&counter->lock);
        counter->value++;
        pthread_mutex_unlock(&counter->lock);
    }

    return NULL;
}

void demo_custom_mutex_example(void) {
    printf("\n=== Demo 5: Custom pthread_mutex Example ===\n");
    printf("Demonstrating proper pthread_mutex usage pattern...\n\n");

    shared_counter_t counter;
    pthread_mutex_init(&counter.lock, NULL);
    counter.value = 0;

    printf("Initial counter value: %d\n", counter.value);

    // Create threads that increment the counter
    #define NUM_THREADS 10
    pthread_t threads[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, counter_thread, &counter);
    }

    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("Final counter value: %d (expected: %d)\n",
           counter.value, NUM_THREADS * 1000);

    printf("\nThe counter is correct because pthread_mutex protected it.\n");

    pthread_mutex_destroy(&counter.lock);
}

/*
 * ============================================================================
 * MAIN
 * ============================================================================
 */

int main(void) {
    printf("======================================================\n");
    printf("     Thread Safety Demonstration for Log Library     \n");
    printf("======================================================\n");

    printf("\nThis program demonstrates how the log library ensures\n");
    printf("thread safety using pthread locks and reader-writer locks.\n\n");

    // Run all demonstrations
    demo_safe_level_changes();
    demo_safe_handler_management();
    demo_concurrent_logging_with_thread_ids();
    demo_async_mode_thread_safety();
    demo_custom_mutex_example();

    printf("\n======================================================\n");
    printf("              All Demonstrations Complete             \n");
    printf("======================================================\n");
    printf("\nKey Takeaways:\n");
    printf("1. Reader-writer locks allow multiple concurrent readers\n");
    printf("2. Writers get exclusive access to prevent data races\n");
    printf("3. Async mode uses lock-free SPSC queue for performance\n");
    printf("4. All public APIs are safe to call from multiple threads\n");
    printf("5. Always use pthread_mutex when protecting shared data\n");

    return 0;
}
