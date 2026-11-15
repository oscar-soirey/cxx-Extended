/*
 * Guard.h
 *
 * Runs compiler code with its diagnostics redirected to a hook and its fatal
 * errors turned into a return value instead of exit().
 */

#ifndef CXXE_LS_GUARD_H
#define CXXE_LS_GUARD_H

#include "Core/Common.h"
#include "Core/Memory.h"

/* Returns 0 on success, 1 when a fatal compiler error was raised. With a
   NULL hook, diagnostics are silently dropped. */
int guard_run(void (*fn)(void *), void *arg, CxxeDiagnosticHook hook, void *ctx);

#endif
