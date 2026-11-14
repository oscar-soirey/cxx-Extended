/*
 * Driver/Pipeline.c
 *
 * Compilation pipeline: source -> tokens -> IR -> resolved IR.
 */

#include "Core/Common.h"
#include "Driver/Pipeline.h"
#include "Core/FileIo.h"
#include "Frontend/IncludeResolver.h"
#include "Ir/Ir.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"
#include "Sema/Resolve.h"

void build_ir_from_cpp(IR *ir, const char *path, const char *const *include_dirs, size_t include_dir_n) {
    ir_init(ir);
    ir->source = read_bytes(path, &ir->source_size);
    lex_cpp(ir->source, ir->source_size, &ir->tokens);
    parse_source(ir);
    discover_included_decorators(ir, path, include_dirs, include_dir_n);
    resolve_native_features(ir);
    resolve_named_args(ir);
}
