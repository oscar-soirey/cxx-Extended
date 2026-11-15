/*
 * Driver/Pipeline.c
 *
 * Compilation pipeline: source -> tokens -> IR -> resolved IR.
 */

#include "Core/Common.h"
#include "Driver/Pipeline.h"
#include "Core/FileIo.h"
#include "Core/Memory.h"
#include "Frontend/IncludeResolver.h"
#include "Ir/Ir.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"
#include "Sema/Resolve.h"

void build_ir_from_cpp(IR *ir, const char *path, const char *const *include_dirs, size_t include_dir_n) {
    size_t size = 0;
    uint8_t *bytes = read_bytes(path, &size);
    build_ir_from_memory(ir, path, bytes, size, include_dirs, include_dir_n);
}

void build_ir_from_memory(IR *ir, const char *path, uint8_t *source, size_t size,
                          const char *const *include_dirs, size_t include_dir_n) {
    ir_init(ir);
    ir->path = xstrdup0(path);
    ir->source = source;
    ir->source_size = size;
    lex_cpp(ir->source, ir->source_size, &ir->tokens);
    parse_source(ir);
    {
        /* Properties, constructors and named arguments are recognised while
           parsing, so declarations from included CXXE headers must be known
           before the parse. The first parse finds the includes; when they
           contribute declarations, parse again with them pre-loaded. */
        IR imported;
        ir_init(&imported);
        ir_add_node(&imported, N_TRANSLATION_UNIT, 0, ir->tokens.n ? ir->tokens.n - 1 : 0, -1);
        if (import_included_declarations(&imported, ir, path, include_dirs, include_dir_n)) {
            ir_replace_nodes(ir, &imported);
            parse_source(ir);
        }
        ir_free(&imported);
    }
    resolve_native_features(ir);
    resolve_named_args(ir);
    check_named_calls(ir);
}
