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

static void emit_unresolved_named_call(const IR *ir, const IRNode *call, Str *out) {
    size_t open = SIZE_MAX, close = SIZE_MAX, i;
    if (call->first_tok >= ir->tokens.n || call->last_tok >= ir->tokens.n) return;
    for (i = (size_t)call->first_tok; i <= (size_t)call->last_tok; ++i) {
        if (strcmp(ir->tokens.v[i].text, "(") == 0) { open = i; break; }
    }
    if (open == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    {
        int d = 0;
        for (i = open; i <= (size_t)call->last_tok; ++i) {
            if (strcmp(ir->tokens.v[i].text, "(") == 0) d++;
            else if (strcmp(ir->tokens.v[i].text, ")") == 0 && --d == 0) { close = i; break; }
        }
    }
    if (close == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    if (open > (size_t)call->first_tok) append_token_bytes(ir, out, (size_t)call->first_tok, open - 1);
    str_ch(out, '(');
    {
        int64_t c; int emitted = 0;
        for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
            const IRNode *arg = &ir->nodes[c];
            if (arg->kind != N_CALL_ARG && arg->kind != N_NAMED_ARG) continue;
            if (emitted) str_put(out, ", ");
            if (arg->kind == N_NAMED_ARG) str_put(out, arg->value ? arg->value : "");
            else append_token_bytes(ir, out, (size_t)arg->first_tok, (size_t)arg->last_tok);
            emitted = 1;
        }
    }
    str_ch(out, ')');
    if (close + 1 <= (size_t)call->last_tok) append_token_bytes(ir, out, close + 1, (size_t)call->last_tok);
}

static size_t call_argument_nodes(const IR *ir, const IRNode *call, int64_t **out) {
    size_t n = 0, cap = 0;
    int64_t c;
    int64_t *v = NULL;
    for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind != N_CALL_ARG && ir->nodes[c].kind != N_NAMED_ARG) continue;
        if (n == cap) { cap = cap ? cap * 2 : 8; v = (int64_t *)xrealloc(v, cap * sizeof(*v)); }
        v[n++] = c;
    }
    *out = v;
    return n;
}

void emit_named_call(const IR *ir, const IRNode *call, Str *out) {
    int64_t fn_id;
    const IRNode *fn = NULL;
    size_t open = SIZE_MAX, close = SIZE_MAX;
    size_t i;
    if (!find_matching_function(ir, call, &fn_id)) {
        emit_unresolved_named_call(ir, call, out);
        return;
    }
    fn = &ir->nodes[fn_id];
    for (i = (size_t)call->first_tok; i <= (size_t)call->last_tok && i < ir->tokens.n; ++i) {
        if (strcmp(ir->tokens.v[i].text, "(") == 0) { open = i; break; }
    }
    if (open == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    {
        int d = 0;
        for (i = open; i <= (size_t)call->last_tok && i < ir->tokens.n; ++i) {
            if (strcmp(ir->tokens.v[i].text, "(") == 0) d++;
            else if (strcmp(ir->tokens.v[i].text, ")") == 0 && --d == 0) { close = i; break; }
        }
    }
    if (close == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    if (open > (size_t)call->first_tok) append_token_bytes(ir, out, (size_t)call->first_tok, open - 1);
    str_ch(out, '(');
    {
        int64_t *args = NULL; size_t an = call_argument_nodes(ir, call, &args), idx;
        size_t pc = function_param_count(ir, fn);
        int *used = (int *)xcalloc(pc ? pc : 1, sizeof(*used));
        int64_t *chosen = (int64_t *)xcalloc(pc ? pc : 1, sizeof(*chosen));
        for (idx = 0; idx < pc; ++idx) chosen[idx] = -1;
        size_t next_pos = 0;
        int impossible = 0;
        for (idx = 0; idx < an; ++idx) {
            const IRNode *arg = &ir->nodes[args[idx]];
            if (arg->kind == N_NAMED_ARG) {
                size_t pi;
                if (function_param_by_name(ir, fn, arg->name, &pi) >= 0 && pi < pc && !used[pi]) {
                    chosen[pi] = args[idx]; used[pi] = 1;
                } else impossible = 1;
            } else {
                while (next_pos < pc && used[next_pos]) next_pos++;
                if (next_pos < pc) { chosen[next_pos] = args[idx]; used[next_pos] = 1; next_pos++; }
                else impossible = 1;
            }
        }
        if (impossible) {
            /* Keep the CXXE call semantics best-effort without destroying
               source already emitted before this call. */
            Str fallback;
            str_init(&fallback);
            emit_unresolved_named_call(ir, call, &fallback);
            str_putn(out, fallback.data ? fallback.data : "", fallback.len);
            str_free(&fallback);
        } else {
            int emitted = 0;
            for (idx = 0; idx < pc; ++idx) {
                const IRNode *arg;
                if (chosen[idx] < 0) {
                    int64_t param_id = -1; size_t pi2 = 0; int64_t c;
                    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
                        if (ir->nodes[c].kind == N_PARAM_DECL) {
                            if (pi2 == idx) { param_id = c; break; }
                            pi2++;
                        }
                    }
                    if (param_id >= 0) {
                        const IRNode *param = &ir->nodes[param_id]; size_t eq = SIZE_MAX, q;
                        for (q = (size_t)param->first_tok; q <= (size_t)param->last_tok && q < ir->tokens.n; ++q) {
                            if (strcmp(ir->tokens.v[q].text, "=") == 0) { eq = q; break; }
                        }
                        if (eq != SIZE_MAX && eq < (size_t)param->last_tok) {
                            if (emitted) str_put(out, ", ");
                            append_token_bytes(ir, out, eq + 1, (size_t)param->last_tok);
                            emitted = 1; continue;
                        }
                    }
                    continue;
                }
                arg = &ir->nodes[chosen[idx]];
                if (emitted) str_put(out, ", ");
                if (arg->kind == N_NAMED_ARG) {
                    size_t eq = SIZE_MAX, q;
                    for (q = (size_t)arg->first_tok; q <= (size_t)arg->last_tok && q < ir->tokens.n; ++q) {
                        if (!strcmp(ir->tokens.v[q].text, "=")) { eq = q; break; }
                    }
                    if (eq != SIZE_MAX && eq < (size_t)arg->last_tok) {
                        emit_native_token_range(ir, out, eq + 1, (size_t)arg->last_tok);
                    } else {
                        str_put(out, arg->value ? arg->value : "");
                    }
                } else {
                    emit_native_token_range(ir, out, (size_t)arg->first_tok, (size_t)arg->last_tok);
                }
                emitted = 1;
            }
        }
        free(chosen); free(used); free(args);
    }
    str_ch(out, ')');
    if (close + 1 <= (size_t)call->last_tok)
        append_token_bytes(ir, out, close + 1, (size_t)call->last_tok);
}
