/**
 * test_named.c - Named logger tests (B4)
 * Platform: All
 *
 * Named loggers attach to the global default context, so these tests mutate
 * that shared context; they restore the stdout handler's active flag and
 * remove the capture handler when done.
 */

#include "test_harness.h"
#include "log.h"

#if LOG_FEATURE_NAMED

#define NAMED_CAP_TEXT 4096

typedef struct {
    int count;
    int len;
    char text[NAMED_CAP_TEXT];
} named_capture;

static void named_capture_fn(log_handle *ctx, log_event *ev) {
    (void)ctx;
    named_capture *c = (named_capture*)ev->udata;
    const char *m = ev->raw_msg ? ev->raw_msg : "";
    size_t n = strlen(m);
    c->count++;
    if (c->len + (int)n + 2 < NAMED_CAP_TEXT) {
        memcpy(c->text + c->len, m, n);
        c->len += (int)n;
        c->text[c->len++] = '\n';
        c->text[c->len] = '\0';
    }
}

/* Attach a capture handler to the default context; returns its index. */
static int named_attach(log_handle *def, named_capture *cap) {
    memset(cap, 0, sizeof(*cap));
    int idx = log_add_handler(def, named_capture_fn, cap, LOG_TRACE);
    return idx;
}

static void test_named_independent_levels(void) {
    log_handle *def = log_default();
    TEST_ASSERT_NOT_NULL(def, "log_default");

    named_capture cap;
    int cap_idx = named_attach(def, &cap);
    TEST_ASSERT(cap_idx >= 0, "add capture handler");

    int saved_active = def->handlers[0].active;
    def->handlers[0].active = false;

    /* Default context is nearly silent; named loggers must not be gated by it. */
    log_set_level(def, LOG_FATAL);
    log_named_set_level("net", LOG_DEBUG);
    log_named_set_level("db", LOG_ERROR);

    log_handle *net = log_get("net");
    log_handle *db = log_get("db");
    TEST_ASSERT(net != NULL && db != NULL, "log_get");
    TEST_ASSERT(net != db, "distinct handles per name");
    TEST_ASSERT(net != def, "named handle is not the default context");

    log_ctx_debug(net, "net-debug");
    log_ctx_info(net, "net-info");
    log_ctx_error(net, "net-error");
    log_ctx_warn(db, "db-warn");     /* below db's ERROR level */
    log_ctx_error(db, "db-error");
    log_ctx_info(def, "def-info");   /* default is FATAL: suppressed */

    TEST_ASSERT(strstr(cap.text, "net-debug") != NULL, "named debug emitted");
    TEST_ASSERT(strstr(cap.text, "net-info") != NULL, "named info emitted");
    TEST_ASSERT(strstr(cap.text, "net-error") != NULL, "named error emitted");
    TEST_ASSERT(strstr(cap.text, "db-error") != NULL, "second name independent (error)");
    TEST_ASSERT(strstr(cap.text, "db-warn") == NULL, "second name independent (warn filtered)");
    TEST_ASSERT(strstr(cap.text, "def-info") == NULL, "default context level still applies");
    TEST_ASSERT_EQ(cap.count, 4, "exactly four lines emitted");

    def->handlers[0].active = saved_active;
    log_set_level(def, LOG_TRACE);
    log_remove_handler(def, cap_idx);
    TEST_PASS("named independent levels");
}

static void test_named_get_idempotent(void) {
    log_handle *a = log_get("svc");
    log_handle *b = log_get("svc");
    TEST_ASSERT(a == b, "same name returns the same handle");

    log_handle *c = log_get("svc-other");
    TEST_ASSERT(c != a, "different names are distinct");

    TEST_PASS("named get idempotent");
}

static void test_named_invalid_names(void) {
    log_handle *def = log_default();

    TEST_ASSERT(log_get(NULL) == def, "NULL name falls back to default");
    TEST_ASSERT(log_get("") == def, "empty name falls back to default");

    /* LOG_NAMED_NAME_MAX includes the NUL; a name of that length is too long. */
    char long_name[LOG_NAMED_NAME_MAX + 8];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    TEST_ASSERT(log_get(long_name) == def, "overlong name falls back to default");

    /* Setters must not crash on bad input. */
    log_named_set_level(NULL, LOG_INFO);
    log_named_set_level("", LOG_INFO);
    log_named_set_level(long_name, LOG_INFO);

    TEST_PASS("named invalid names");
}

/* Keep last: exhausts the registry. */
static void test_named_registry_full(void) {
    log_handle *def = log_default();
    int overflow_seen = 0;

    for (int i = 0; i < LOG_NAMED_MAX + 4; i++) {
        char nm[LOG_NAMED_NAME_MAX];
        snprintf(nm, sizeof(nm), "fill%02d", i);
        if (log_get(nm) == def) overflow_seen = 1;
    }

    TEST_ASSERT(overflow_seen, "registry full falls back to default");
    TEST_ASSERT(log_get(NULL) == def, "still safe when full");

    /* Existing names keep working even when full. */
    log_handle *svc = log_get("svc");
    TEST_ASSERT(svc != NULL, "existing named handle still resolves");

    TEST_PASS("named registry full");
}

void test_named_register(void) {
    test_add(test_named_independent_levels, "named_independent_levels");
    test_add(test_named_get_idempotent, "named_get_idempotent");
    test_add(test_named_invalid_names, "named_invalid_names");
    test_add(test_named_registry_full, "named_registry_full");
}

#else

void test_named_register(void) {
}

#endif /* LOG_FEATURE_NAMED */
