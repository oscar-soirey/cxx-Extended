/*
 * CodeGen/CodeGen.h
 *
 * Public C++ code generation entry points.
 */

#ifndef CXXE_CODE_GEN_CODE_GEN_H
#define CXXE_CODE_GEN_CODE_GEN_H

#include "Core/Common.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"

/* Generated C++ for `ir` (caller frees `out`). When ir->path is set, #line
   directives map expanded CXXE constructs back to the source lines. */
void emit_cpp_to_str(const IR *ir, Str *out);
void emit_exact(const IR *ir, const char *path);

#endif /* CXXE_CODE_GEN_CODE_GEN_H */
