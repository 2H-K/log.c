/**
 * test_named_mt.c - Concurrent named-logger creation tests (B4)
 * Platform: All
 *
 * The registry lives on the global default context; concurrent log_get() of
 * the same name must create exactly one alias.
 */

#include "test_harness.h"
#include "log.h"
#include "thread/test_thread.h"

#if LOG_FEATURE_NAMED

typedef struct {
    const char *name;
    log_handle *result;
} named_mt_arg;

#if defined(_WIN32) || defined(_WIN64)
static DWORD WINAPI named_getter(LPVOID arg) {
#else
static void* named_getter(void *arg) {
#endif
    named_mt_arg *a = (named_mt_arg*)arg;
    a->result = log_get(a->name);
#if defined(_WIN32) || defined(_WIN64)
    return 0;
#else
    return NULL;
#endif
}

static void test_named_get_concurrent(void) {
    log_handle *def = log_default();
    TEST_ASSERT_NOT_NULL(def, "log_default");

    named_mt_arg args[NUM_THREADS];
    THREAD_T threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].name = "concurrent-name";
        args[i].result = NULL;
        THREAD_CREATE(threads[i], named_getter, &args[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        THREAD_JOIN(threads[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        TEST_ASSERT_NOT_NULL(args[i].result, "log_get returned a handle");
        TEST_ASSERT(args[i].result == args[0].result,
                    "all threads share one alias");
    }

    /* The registry must hold exactly one entry for the name. */
    int matches = 0;
    for (int i = 0; i < def->named_count; i++) {
        if (strcmp(def->named[i].name, "concurrent-name") == 0) matches++;
    }
    TEST_ASSERT_EQ(matches, 1, "created exactly once");

    TEST_PASS("named log_get concurrent creation");
}

void test_named_mt_register(void) {
    test_add(test_named_get_concurrent, "named_get_concurrent");
}

#else

void test_named_mt_register(void) {
}

#endif /* LOG_FEATURE_NAMED */
