/*
 * CodeGen/CodeGen.h
 *
 * Public C++ code generation entry points.
 */

#ifndef CXXE_CODE_GEN_CODE_GEN_H
#define CXXE_CODE_GEN_CODE_GEN_H

#include "Core/Common.h"
#include "Ir/Ir.h"

void emit_exact(const IR *ir, const char *path);

#endif /* CXXE_CODE_GEN_CODE_GEN_H */
