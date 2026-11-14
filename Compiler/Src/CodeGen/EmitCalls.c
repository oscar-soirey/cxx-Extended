/*
 * CodeGen/EmitCalls.c
 *
 * Emission of named-argument calls.
 */

#include "Core/Common.h"
#include "CodeGen/CodeGenInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Sema/Resolve.h"

void append_token_bytes(const IR *ir, Str *out, size_t a, size_t b) {
    if (a > b || a >= ir->tokens.n) return;
    if (b >= ir->tokens.n) b = ir->tokens.n - 1;
    if (ir->tokens.v[b].byte_end <= ir->tokens.v[a].byte_start) return;
    str_putn(out, (const char *)ir->source + ir->tokens.v[a].byte_start,
             (size_t)(ir->tokens.v[b].byte_end - ir->tokens.v[a].byte_start));
}

static void put_source_bytes(const IR *ir, Str *out, uint64_t a, uint64_t b) {
    if (b > a && b <= ir->source_size) str_putn(out, (const char *)ir->source + a, (size_t)(b - a));
}

/* Emits the call exactly as written, but still lowers CXXE constructs that
   appear inside its arguments (nested named calls, properties...). */
static void emit_call_verbatim(const IR *ir, const IRNode *call, Str *out) {
    uint64_t cursor = ir->tokens.v[call->first_tok].byte_start;
    int64_t c;
    for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *arg = &ir->nodes[c];
        if (arg->kind != N_CALL_ARG && arg->kind != N_NAMED_ARG) continue;
        if (arg->first_tok > arg->last_tok || arg->last_tok >= ir->tokens.n) continue;
        put_source_bytes(ir, out, cursor, ir->tokens.v[arg->first_tok].byte_start);
        emit_native_token_range(ir, out, (size_t)arg->first_tok, (size_t)arg->last_tok);
        cursor = ir->tokens.v[arg->last_tok].byte_end;
    }
    put_source_bytes(ir, out, cursor, ir->tokens.v[call->last_tok].byte_end);
}

static int call_parens(const IR *ir, const IRNode *call, size_t *open_out, size_t *close_out) {
    size_t i, open = SIZE_MAX;
    int d = 0;
    for (i = (size_t)call->first_tok; i <= (size_t)call->last_tok && i < ir->tokens.n; ++i)
        if (!strcmp(ir->tokens.v[i].text, "(")) { open = i; break; }
    if (open == SIZE_MAX) return 0;
    for (i = open; i <= (size_t)call->last_tok && i < ir->tokens.n; ++i) {
        if (!strcmp(ir->tokens.v[i].text, "(")) d++;
        else if (!strcmp(ir->tokens.v[i].text, ")") && --d == 0) { *open_out = open; *close_out = i; return 1; }
    }
    return 0;
}

static void emit_argument_value(const IR *ir, const IRNode *arg, Str *out) {
    size_t q;
    if (arg->kind == N_NAMED_ARG) {
        for (q = (size_t)arg->first_tok; q < (size_t)arg->last_tok && q < ir->tokens.n; ++q)
            if (!strcmp(ir->tokens.v[q].text, "=")) { emit_native_token_range(ir, out, q + 1, (size_t)arg->last_tok); return; }
        str_put(out, arg->value ? arg->value : "");
        return;
    }
    emit_native_token_range(ir, out, (size_t)arg->first_tok, (size_t)arg->last_tok);
}

void emit_named_call(const IR *ir, const IRNode *call, Str *out) {
    NamedCallPlan plan;
    Str why;
    size_t open, close, idx;
    int r;
    str_init(&why);
    r = plan_named_call(ir, call, &plan, &why);
    if (r < 0) ir_die_at(ir, call->first_tok, "%s", why.data ? why.data : "invalid named arguments");
    str_free(&why);
    if (r == 0 || !call_parens(ir, call, &open, &close)) {
        named_call_plan_free(&plan);
        emit_call_verbatim(ir, call, out);
        return;
    }
    if (open > (size_t)call->first_tok) append_token_bytes(ir, out, (size_t)call->first_tok, open - 1);
    str_ch(out, '(');
    for (idx = 0; idx < plan.param_count; ++idx) {
        if (idx) str_put(out, ", ");
        if (plan.slot[idx] >= 0) {
            emit_argument_value(ir, &ir->nodes[plan.slot[idx]], out);
        } else {
            char *d = param_default_text(ir, &ir->nodes[plan.fn], idx);
            if (!d) ir_die_at(ir, call->first_tok, "no default value visible for parameter %zu", idx + 1);
            str_put(out, d);
            free(d);
        }
    }
    str_ch(out, ')');
    if (close + 1 <= (size_t)call->last_tok) append_token_bytes(ir, out, close + 1, (size_t)call->last_tok);
    named_call_plan_free(&plan);
}
