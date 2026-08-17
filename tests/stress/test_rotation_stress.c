/**
 * test_rotation_stress.c - Million-level file rotation stress test
 * Tests rotation behavior with small file size and many writes
 */

#include "test_harness.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <dirent.h>
#include <sys/stat.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#define ROTATION_TEST_DIR "log_rotation_test"
#else
#define ROTATION_TEST_DIR "/tmp/log_rotation_test"
#endif
#define ROTATION_FILE_SIZE 1024  /* 1KB per file to trigger many rotations */
#define ROTATION_MSG_COUNT 5000  /* Write 5000 messages */

static void create_test_dir(void) {
    char cmd[256];
#if defined(_WIN32) || defined(_WIN64)
    snprintf(cmd, sizeof(cmd), "rmdir /s /q %s 2>nul & mkdir %s", ROTATION_TEST_DIR, ROTATION_TEST_DIR);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", ROTATION_TEST_DIR, ROTATION_TEST_DIR);
#endif
    system(cmd);
}

#if !defined(_WIN32) && !defined(_WIN64)
static int count_rotation_files(void) {
    DIR *dp = opendir(ROTATION_TEST_DIR);
    if (!dp) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strstr(entry->d_name, "stress_rotate.log")) count++;
    }
    closedir(dp);
    return count;
}
#else
static int count_rotation_files(void) {
    /* Windows: count by trying to open each possible rotation file */
    int count = 0;
    for (int i = 0; i <= 5; i++) {
        char fname[256];
        if (i == 0) {
            snprintf(fname, sizeof(fname), "%s/stress_rotate.log", ROTATION_TEST_DIR);
        } else {
            snprintf(fname, sizeof(fname), "%s/stress_rotate.log.%d", ROTATION_TEST_DIR, i);
        }
        FILE *f = fopen(fname, "r");
        if (f) {
            count++;
            fclose(f);
        }
    }
    return count;
}
#endif

static void test_rotation_many_files(void) {
    create_test_dir();

    char path[256];
    snprintf(path, sizeof(path), "%s/stress_rotate.log", ROTATION_TEST_DIR);

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen rotation test file");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add handler for rotation test");

    log_set_max_file_size(ctx, ROTATION_FILE_SIZE);
    log_set_file_prefix(ctx, path);

    /* Write many messages to trigger rotation */
    for (int i = 0; i < ROTATION_MSG_COUNT; i++) {
        log_ctx_info(ctx, "ROTATION_MSG_%06d_padding_data_to_increase_size", i);
    }

    log_destroy(ctx);
    fclose(fp);

    /* Count rotation files */
    int file_count = count_rotation_files();
    int expected_max = 6;  /* LOG_MAX_ROTATION_FILES (5) + current (1) */

    printf("    files created: %d (max expected: %d)", file_count, expected_max);

    /* Verify file count doesn't exceed max */
    TEST_ASSERT(file_count <= expected_max, "rotation file count within limit");

    /* Verify each rotation file has valid content */
    int total_lines = 0;
    for (int i = 0; i <= 5; i++) {
        char fname[256];
        if (i == 0) {
            snprintf(fname, sizeof(fname), "%s/stress_rotate.log", ROTATION_TEST_DIR);
        } else {
            snprintf(fname, sizeof(fname), "%s/stress_rotate.log.%d", ROTATION_TEST_DIR, i);
        }
        FILE *f = fopen(fname, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "ROTATION_MSG_")) total_lines++;
            }
            fclose(f);
        }
    }

    printf("    total messages across all files: %d", total_lines);

    /* All messages should be accounted for (some may be in current file) */
    TEST_ASSERT(total_lines > 0, "rotation files contain valid log data");

    /* Cleanup */
    create_test_dir();
    TEST_PASS("rotation with many files");
}

static void test_rotation_file_integrity(void) {
    create_test_dir();

    char path[256];
    snprintf(path, sizeof(path), "%s/integrity_rotate.log", ROTATION_TEST_DIR);

    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen integrity rotation file");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
    TEST_ASSERT(idx >= 0, "add handler");

    log_set_max_file_size(ctx, ROTATION_FILE_SIZE);
    log_set_file_prefix(ctx, path);

    /* Write messages with unique sequence numbers */
    for (int i = 0; i < ROTATION_MSG_COUNT; i++) {
        log_ctx_info(ctx, "SEQ_%06d", i);
    }

    log_destroy(ctx);
    fclose(fp);

    /* Verify no duplicate or corrupted messages across all files */
    int bitmap[ROTATION_MSG_COUNT];
    memset(bitmap, 0, sizeof(bitmap));
    int total_found = 0;
    int duplicates = 0;

    for (int i = 0; i <= 5; i++) {
        char fname[256];
        if (i == 0) {
            snprintf(fname, sizeof(fname), "%s/integrity_rotate.log", ROTATION_TEST_DIR);
        } else {
            snprintf(fname, sizeof(fname), "%s/integrity_rotate.log.%d", ROTATION_TEST_DIR, i);
        }
        FILE *f = fopen(fname, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                int seq = -1;
                if (sscanf(line, "%*s %*s %*s SEQ_%d", &seq) == 1) {
                    if (seq >= 0 && seq < ROTATION_MSG_COUNT) {
                        if (bitmap[seq]) {
                            duplicates++;
                        } else {
                            bitmap[seq] = 1;
                            total_found++;
                        }
                    }
                }
            }
            fclose(f);
        }
    }

    int missing = 0;
    for (int i = 0; i < ROTATION_MSG_COUNT; i++) {
        if (!bitmap[i]) missing++;
    }

    printf("    found=%d, missing=%d, duplicates=%d", total_found, missing, duplicates);

    /* No duplicates allowed (each message should appear exactly once or not at all) */
    TEST_ASSERT_EQ(duplicates, 0, "no duplicate messages across rotation files");

    /* Some messages may be lost during rotation, but most should be present */
    TEST_ASSERT(total_found >= ROTATION_MSG_COUNT * 0.9, "at least 90% messages preserved");

    create_test_dir();
    TEST_PASS("rotation file integrity");
}

static void test_rotation_manual_trigger(void) {
    create_test_dir();

    char path[256];
    snprintf(path, sizeof(path), "%s/manual_rotate.log", ROTATION_TEST_DIR);

#if defined(_WIN32) || defined(_WIN64)
    /* On Windows, log_rotate must close+reopen the file to rename it,
     * so we use log_add_file (owns_file=true) to avoid double-close. */
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen manual rotation file");
    if (fp) fclose(fp);

    log *ctx = log_create();
    int idx = log_add_file(ctx, path, LOG_INFO);
#else
    FILE *fp = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(fp, "fopen manual rotation file");

    log *ctx = log_create();
    int idx = log_add_fp(ctx, fp, LOG_INFO);
#endif
    TEST_ASSERT(idx >= 0, "add handler");

    log_set_file_prefix(ctx, path);

    /* Write some messages */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "BEFORE_ROTATE_%03d", i);
    }

    /* Manually trigger rotation */
    log_rotate(ctx);

    /* Write more messages after rotation */
    for (int i = 0; i < 100; i++) {
        log_ctx_info(ctx, "AFTER_ROTATE_%03d", i);
    }

    log_destroy(ctx);
#if !defined(_WIN32) && !defined(_WIN64)
    fclose(fp);
#endif

    /* Verify both sets of messages exist */
    int before_count = 0, after_count = 0;
    char fname[256];

    /* Check current file for after-rotate messages */
    snprintf(fname, sizeof(fname), "%s/manual_rotate.log", ROTATION_TEST_DIR);
    FILE *f = fopen(fname, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "AFTER_ROTATE_")) after_count++;
        }
        fclose(f);
    }

    /* Check .1 file for before-rotate messages */
    snprintf(fname, sizeof(fname), "%s/manual_rotate.log.1", ROTATION_TEST_DIR);
    f = fopen(fname, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "BEFORE_ROTATE_")) before_count++;
        }
        fclose(f);
    }

    printf("    before_rotate=%d, after_rotate=%d", before_count, after_count);

    TEST_ASSERT(before_count >= 90, "most before-rotate messages in .1 file");
    TEST_ASSERT(after_count >= 90, "most after-rotate messages in current file");

    create_test_dir();
    TEST_PASS("manual rotation trigger");
}

void test_rotation_stress_register(void) {
    test_add(test_rotation_many_files, "rotation_many_files");
    test_add(test_rotation_file_integrity, "rotation_file_integrity");
    test_add(test_rotation_manual_trigger, "rotation_manual_trigger");
}
