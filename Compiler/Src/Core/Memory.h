/*
 * Core/Memory.h
 *
 * Fatal diagnostics and checked allocation helpers.
 */

#ifndef CXXE_CORE_MEMORY_H
#define CXXE_CORE_MEMORY_H

#include "Core/Common.h"

void die(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *xrealloc(void *p, size_t n);
char *xstrdup0(const char *s);
int str_eq(const char *a, const char *b);
char *xstrndup0(const char *s, size_t n);

#endif /* CXXE_CORE_MEMORY_H */
