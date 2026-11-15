/*
 * Core/Memory.h
 *
 * Fatal diagnostics and checked allocation helpers.
 */

#ifndef CXXE_CORE_MEMORY_H
#define CXXE_CORE_MEMORY_H

#include "Core/Common.h"

/* Diagnostics. By default they are printed to stderr and errors exit(1).
   An embedding tool (the language server) installs a hook to collect them;
   errors then longjmp to *g_cxxe_fatal_jmp when it is set. */
enum { CXXE_DIAG_ERROR = 1, CXXE_DIAG_WARNING = 2 };
typedef void (*CxxeDiagnosticHook)(void *ctx, int severity, const char *path,
                                   unsigned line, unsigned column, const char *message);
extern CxxeDiagnosticHook g_cxxe_diag_hook;
extern void *g_cxxe_diag_ctx;
extern jmp_buf *g_cxxe_fatal_jmp;

/* line/column are 1-based (column counted in bytes); 0 when unknown. */
void cxxe_diagnostic(int severity, const char *path, unsigned line, unsigned column, const char *fmt, va_list ap);
void die(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t s);
void *xrealloc(void *p, size_t n);
char *xstrdup0(const char *s);
int str_eq(const char *a, const char *b);
char *xstrndup0(const char *s, size_t n);

#endif /* CXXE_CORE_MEMORY_H */
