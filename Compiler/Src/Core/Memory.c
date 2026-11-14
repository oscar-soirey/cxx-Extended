/*
 * Core/Memory.c
 *
 * Fatal diagnostics and checked allocation helpers.
 */

#include "Core/Common.h"
#include "Core/Memory.h"

void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "cxxe: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
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
