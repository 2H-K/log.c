/**
 * test_harness.h - Minimal test framework for log.c
 *
 * Usage:
   static void test_foo(void) {
       TEST_ASSERT(1 + 1 == 2, "math works");
       TEST_PASS("foo passed");
   }
   static void test_bar(void) {
       TEST_ASSERT(2 + 2 == 4, "math works");
   }

   // In the runner file:
   int main(void) {
       test_add(test_foo, "test_foo");
       test_add(test_bar, "test_bar");
       return test_run_all() ? 1 : 0;
   }
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

/* Platform defines must come before any system headers */
#if !defined(_WIN32) && !defined(_WIN64)
  #if !defined(_GNU_SOURCE)
    #define _GNU_SOURCE
  #endif
  #if !defined(_POSIX_C_SOURCE)
    #define _POSIX_C_SOURCE 199309L
  #endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Timing ==================== */

static inline uint64_t test_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (uint64_t)((double)cnt.QuadPart / (double)freq.QuadPart * 1e9);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ==================== Test Case ==================== */

typedef void (*test_func_t)(void);

typedef struct {
    const char *name;
    test_func_t func;
} test_case;

#define MAX_TESTS 256

static test_case g_tests[MAX_TESTS];
static int g_test_count = 0;
static int g_passed = 0;
static int g_failed = 0;
static int g_skipped = 0;

static void test_add(test_func_t func, const char *name) {
    if (g_test_count < MAX_TESTS) {
        g_tests[g_test_count].name = name;
        g_tests[g_test_count].func = func;
        g_test_count++;
    }
}

/* ==================== Assertions ==================== */

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL [%s:%d] %s: %s\n", __FILE__, __LINE__, msg, #cond); \
        g_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "  FAIL [%s:%d] %s: %ld != %ld\n", __FILE__, __LINE__, msg, \
                (long)(a), (long)(b)); \
        g_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_STR_EQ(a, b, msg) do { \
    const char *_a = (a); \
    const char *_b = (b); \
    if ((_a == NULL && _b != NULL) || (_a != NULL && _b == NULL) || \
        (_a != NULL && _b != NULL && strcmp(_a, _b) != 0)) { \
        fprintf(stderr, "  FAIL [%s:%d] %s: \"%s\" != \"%s\"\n", __FILE__, __LINE__, msg, \
                _a ? _a : "(null)", _b ? _b : "(null)"); \
        g_failed++; \
        return; \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr, msg) \
    TEST_ASSERT((ptr) != NULL, msg)

#define TEST_ASSERT_NULL(ptr, msg) \
    TEST_ASSERT((ptr) == NULL, msg)

#define TEST_PASS(msg) do { \
    g_passed++; \
    printf("  PASS: %s\n", msg); \
} while(0)

#define TEST_SKIP(msg) do { \
    g_skipped++; \
    printf("  SKIP: %s\n", msg); \
} while(0)

/* ==================== Test Runner ==================== */

static int test_run_all(void) {
    printf("\n=== Running %d tests ===\n\n", g_test_count);

    for (int i = 0; i < g_test_count; i++) {
        printf("%s ... ", g_tests[i].name);
        fflush(stdout);

        int prev_failed = g_failed;
        int prev_passed = g_passed;
        g_tests[i].func();

        if (g_failed == prev_failed && g_passed == prev_passed) {
            g_passed++;
            printf("  PASS (implicit)\n");
        }
    }

    printf("\n=== Results ===\n");
    printf("  Total:   %d\n", g_test_count);
    printf("  Passed:  %d\n", g_passed);
    printf("  Failed:  %d\n", g_failed);
    printf("  Skipped: %d\n", g_skipped);

    return g_failed;
}

/* ==================== Platform Detection ==================== */

#define TEST_PLATFORM_POSIX 1
#define TEST_PLATFORM_WINDOWS 2

#if defined(_WIN32) || defined(_WIN64)
  #define TEST_PLATFORM TEST_PLATFORM_WINDOWS
#else
  #define TEST_PLATFORM TEST_PLATFORM_POSIX
#endif

#define TEST_ON_POSIX() (TEST_PLATFORM == TEST_PLATFORM_POSIX)
#define TEST_ON_WINDOWS() (TEST_PLATFORM == TEST_PLATFORM_WINDOWS)

/* ==================== File Helpers ==================== */

#define TEST_TMP_FILE "test_tmp.log"

static void test_cleanup_files(void) {
    remove(TEST_TMP_FILE);
    remove("test_rot");
    remove("test_rot.1");
    remove("test_rot.2");
    remove("test_rot.3");
    remove("test_rot.4");
    remove("test_rot.5");
    remove("test_tmp.json");
    remove("test_h1.txt");
    remove("test_h2.txt");
}

#ifdef __cplusplus
}
#endif

#endif /* TEST_HARNESS_H */
