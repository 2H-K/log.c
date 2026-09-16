/*
 * msvc_oi_smoke.c - Public header regression guard (MSVC only)
 *
 * MSVC's /Oi (implied by /O2) exposes the builtin math function `log`, so the
 * public header must not declare `log` as an ordinary identifier.  The handle
 * type is therefore called `log_handle`.  This TU is compiled with /WX on MSVC
 * so that reintroducing such a name fails CI.
 */

#include "log.h"

int main(void) {
    log_handle *ctx = log_create();
    log_destroy(ctx);
    return 0;
}
