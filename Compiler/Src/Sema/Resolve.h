/*
 * Sema/Resolve.h
 *
 * Semantic passes: native feature resolution and named argument binding.
 */

#ifndef CXXE_SEMA_RESOLVE_H
#define CXXE_SEMA_RESOLVE_H

#include "Core/Common.h"
#include "Ir/Ir.h"

int get_member_pattern(const IR *ir, const IRNode *n, const char **member, size_t *member_tok, size_t *close_tok);
int find_custom_decorator_use(const IR *ir, const IRNode *use, int64_t *out_id);
void resolve_native_features(IR *ir);
int resolve_named_args(IR *ir);
int node_is_named_call(const IR *ir, int64_t id);
int find_matching_function(const IR *ir, const IRNode *call, int64_t *out);
int64_t function_param_by_name(const IR *ir, const IRNode *fn, const char *name, size_t *index_out);
size_t function_param_count(const IR *ir, const IRNode *fn);

#endif /* CXXE_SEMA_RESOLVE_H */
