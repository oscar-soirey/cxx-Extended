/*
 * Core/Memory.c
 *
 * Fatal diagnostics and checked allocation helpers.
 */

#include "Core/Common.h"
#include "Core/Memory.h"

CxxeDiagnosticHook g_cxxe_diag_hook = NULL;
void *g_cxxe_diag_ctx = NULL;
jmp_buf *g_cxxe_fatal_jmp = NULL;

void cxxe_diagnostic(int severity, const char *path, unsigned line, unsigned column, const char *fmt, va_list ap) {
    const char *level = severity == CXXE_DIAG_ERROR ? "error" : "warning";
    if (g_cxxe_diag_hook) {
        char small[1024], *msg = small;
        va_list copy;
        int n;
        va_copy(copy, ap);
        n = vsnprintf(small, sizeof(small), fmt, copy);
        va_end(copy);
        if (n >= (int)sizeof(small)) {
            msg = (char *)malloc((size_t)n + 1);
            if (msg) vsnprintf(msg, (size_t)n + 1, fmt, ap);
            else msg = small;
        }
        g_cxxe_diag_hook(g_cxxe_diag_ctx, severity, path, line, column, msg);
        if (msg != small) free(msg);
    } else {
        if (line) fprintf(stderr, "%s:%u:%u: %s: ", path ? path : "<input>", line, column, level);
        else if (path) fprintf(stderr, "%s: %s: ", path, level);
        else fprintf(stderr, "cxxe: %s: ", level);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    if (severity == CXXE_DIAG_ERROR) {
        if (g_cxxe_fatal_jmp) longjmp(*g_cxxe_fatal_jmp, 1);
        exit(1);
    }
}

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cxxe_diagnostic(CXXE_DIAG_ERROR, NULL, 0, 0, fmt, ap);
    va_end(ap);
    exit(1); /* not reached */
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) die("out of memory (%zu x %zu)", n, s);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory reallocating to %zu bytes", n);
    return q;
}

char *xstrdup0(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *r = (char *)xmalloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

int str_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

char *xstrndup0(const char *s, size_t n) {
    char *r = (char *)xmalloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}
