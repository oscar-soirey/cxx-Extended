/*
 * Driver/Pipeline.h
 *
 * Compilation pipeline: source -> tokens -> IR -> resolved IR.
 */

#ifndef CXXE_DRIVER_PIPELINE_H
#define CXXE_DRIVER_PIPELINE_H

#include "Core/Common.h"
#include "Ir/Ir.h"

void build_ir_from_cpp(IR *ir, const char *path, const char *const *include_dirs, size_t include_dir_n);
/* Same pipeline on in-memory text; `source` (malloc'd) is owned by the IR.
   `path` is used for diagnostics, #line directives and relative includes. */
void build_ir_from_memory(IR *ir, const char *path, uint8_t *source, size_t size,
                          const char *const *include_dirs, size_t include_dir_n);

#endif /* CXXE_DRIVER_PIPELINE_H */
