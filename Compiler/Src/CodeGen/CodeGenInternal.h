/*
 * CodeGen/CodeGenInternal.h
 *
 * Helpers shared between code generation translation units.
 */

#ifndef CXXE_CODE_GEN_CODE_GEN_INTERNAL_H
#define CXXE_CODE_GEN_CODE_GEN_INTERNAL_H

#include "Core/Common.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"

void append_token_bytes(const IR *ir, Str *out, size_t a, size_t b);
void emit_named_call(const IR *ir, const IRNode *call, Str *out);
int class_has_runtime_features(const IR *ir);
void emit_registered_classes(const IR *ir, Str *out);
int node_is_factory_runtime_call(const IRNode *n);
int find_call_close(const IR *ir, const IRNode *call, size_t *open_out, size_t *close_out);
void emit_factory_runtime(const IR *ir, const IRNode *call, Str *out);
void emit_get_member_runtime(const IR *ir, const IRNode *node, Str *out);
const IRNode *find_custom_decorator_def(const IR *ir, const char *name, const char *qualified_name);
int function_has_custom_decorator(const IR *ir, const IRNode *fn);
size_t function_body_open(const IR *ir, const IRNode *fn);
size_t function_name_token(const IR *ir, const IRNode *fn);
void emit_tokens_range(const IR *ir, Str *out, size_t a, size_t b, const char *replace_name, size_t replace_tok);
void emit_source_range_with_properties(const IR *ir, Str *out, uint64_t begin, uint64_t end);
int decorator_template_arg_ranges(const IR *ir, const IRNode *dec, size_t *starts, size_t *ends, size_t cap);
size_t custom_decorator_template_param_count(const IR *ir, const IRNode *cd);
void emit_property(const IR *ir, const IRNode *prop, Str *out);
int property_access_is_write(const IR *ir, const IRNode *node);
void emit_property_access(const IR *ir, const IRNode *node, Str *out);
int node_is_property_assignment(const IR *ir, const IRNode *node, int64_t *prop_id_out, size_t *op_out, size_t *sep_out);
void emit_property_unary(const IR *ir, const IRNode *node, Str *out);
void emit_property_assignment(const IR *ir, const IRNode *node, Str *out);
void emit_function_with_custom_decorators(const IR *ir, const IRNode *fn, Str *out);
void emit_match_statement(const IR *ir, const IRNode *node, Str *out);
void emit_native_token_range(const IR *ir, Str *out, size_t begin_tok, size_t end_tok);

#endif /* CXXE_CODE_GEN_CODE_GEN_INTERNAL_H */
