/*
 * Guard.c
 */

#include "Guard.h"

static void drop_diagnostic(void *ctx, int severity, const char *path, unsigned line, unsigned column, const char *message) {
    (void)ctx; (void)severity; (void)path; (void)line; (void)column; (void)message;
}

int guard_run(void (*fn)(void *), void *arg, CxxeDiagnosticHook hook, void *ctx) {
    jmp_buf jb;
    jmp_buf *prev_jmp = g_cxxe_fatal_jmp;
    CxxeDiagnosticHook prev_hook = g_cxxe_diag_hook;
    void *prev_ctx = g_cxxe_diag_ctx;
    volatile int failed = 0;
    g_cxxe_diag_hook = hook ? hook : drop_diagnostic;
    g_cxxe_diag_ctx = ctx;
    g_cxxe_fatal_jmp = &jb;
    if (setjmp(jb) == 0) fn(arg);
    else failed = 1;
    g_cxxe_fatal_jmp = prev_jmp;
    g_cxxe_diag_hook = prev_hook;
    g_cxxe_diag_ctx = prev_ctx;
    return failed;
}
