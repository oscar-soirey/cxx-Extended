/*
 * Sema/Resolve.h
 *
 * Semantic passes: native feature resolution and named argument binding.
 */

#ifndef CXXE_SEMA_RESOLVE_H
#define CXXE_SEMA_RESOLVE_H

#include "Core/Common.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"

/* Argument layout chosen for a call using named arguments. slot[i] is the
   argument node bound to parameter i, or -1 when its default value is used.
   Parameters after param_count are omitted (C++ applies their defaults). */
typedef struct {
    int64_t fn;
    size_t param_count;
    int64_t *slot;
} NamedCallPlan;

int get_member_pattern(const IR *ir, const IRNode *n, const char **member, size_t *member_tok, size_t *close_tok);
int find_custom_decorator_use(const IR *ir, const IRNode *use, int64_t *out_id);
void resolve_native_features(IR *ir);
int resolve_named_args(IR *ir);
int node_is_named_call(const IR *ir, int64_t id);
int find_matching_function(const IR *ir, const IRNode *call, int64_t *out);
/* 1: planned, 0: no visible declaration, -1: declarations exist but none fits (why). */
int plan_named_call(const IR *ir, const IRNode *call, NamedCallPlan *plan, Str *why);
void named_call_plan_free(NamedCallPlan *plan);
char *param_default_text(const IR *ir, const IRNode *fn, size_t index);
int64_t function_param_id(const IR *ir, const IRNode *fn, size_t index);
void check_named_calls(IR *ir);
int64_t function_param_by_name(const IR *ir, const IRNode *fn, const char *name, size_t *index_out);
size_t function_param_count(const IR *ir, const IRNode *fn);

#endif /* CXXE_SEMA_RESOLVE_H */
