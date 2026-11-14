/*
 * CodeGen/Emitter.c
 *
 * Native emission driver: emit_cpp / emit_exact.
 */

#include "Core/Common.h"
#include "CodeGen/CodeGenInternal.h"
#include "CodeGen/CodeGen.h"
#include "Core/FileIo.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Sema/Resolve.h"

/* CXXE headers (.hppe) are translated to .hpp next to the generated
   sources, so includes of them are redirected. */
static int include_targets_cxxe_header(const IRNode *n) {
    return n->kind == N_INCLUDE && n->value && strstr(n->value, ".hppe") != NULL;
}

static void emit_include_directive(const IR *ir, const IRNode *n, Str *out) {
    const char *t = ir->tokens.v[n->first_tok].text, *hit;
    while ((hit = strstr(t, ".hppe")) != NULL) {
        str_putn(out, t, (size_t)(hit - t));
        str_put(out, ".hpp");
        t = hit + 5;
    }
    str_put(out, t);
}

static int node_needs_native_emit(const IR *ir, const IRNode *n) {
    if (include_targets_cxxe_header(n)) return 1;
    if (n->kind == N_DECORATOR) return 1;
    if (n->kind == N_CUSTOM_DECORATOR) return 1;
    if (n->kind == N_PROPERTY) return 1;
    if (n->kind == N_UNARY_EXPR && n->name && (!strcmp(n->name,"++") || !strcmp(n->name,"--"))) return 1;
    if (n->kind == N_PROPERTY_ACCESS) return !property_access_is_write(ir, n);
    if (n->kind == N_ASSIGN_EXPR) return node_is_property_assignment(ir, n, NULL, NULL, NULL) ||
        (n->name && (!strcmp(n->name,"++") || !strcmp(n->name,"--")));
    if ((n->kind == N_FUNCTION || n->kind == N_METHOD) && function_has_custom_decorator(ir, n)) return 1;
    if (n->kind == N_GET_MEMBER) return 1;
    if (n->kind == N_FUNC_CALL && node_is_factory_runtime_call(n)) return 1;
    if (n->kind == N_FUNC_CALL && node_is_named_call(ir, (int64_t)(n - ir->nodes))) return 1;
    if (n->kind == N_MATCH_STMT) return 1;
    return 0;
}

void emit_native_token_range(const IR *ir, Str *out, size_t begin_tok, size_t end_tok) {
    size_t i, native_count = 0, cursor_tok = begin_tok;
    size_t *native_ids = NULL;
    if (begin_tok > end_tok || begin_tok >= ir->tokens.n) return;
    if (end_tok >= ir->tokens.n) end_tok = ir->tokens.n - 1;

    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!node_needs_native_emit(ir, n)) continue;
        if (n->flags & NF_EXTERNAL_DECL) continue;
        if (n->first_tok < begin_tok || n->last_tok > end_tok || n->first_tok > n->last_tok) continue;
        native_ids = (size_t *)xrealloc(native_ids, (native_count + 1) * sizeof(*native_ids));
        native_ids[native_count++] = i;
    }

    for (i = 1; i < native_count; ++i) {
        size_t key = native_ids[i], j = i;
        while (j > 0) {
            size_t prev = native_ids[j - 1];
            if (ir->nodes[prev].first_tok < ir->nodes[key].first_tok ||
                (ir->nodes[prev].first_tok == ir->nodes[key].first_tok &&
                 ir->nodes[prev].last_tok <= ir->nodes[key].last_tok)) break;
            native_ids[j] = prev;
            --j;
        }
        native_ids[j] = key;
    }

    for (i = 0; i < native_count; ++i) {
        const IRNode *n = &ir->nodes[native_ids[i]];
        size_t first_tok = (size_t)n->first_tok;
        size_t last_tok = (size_t)n->last_tok;
        uint64_t bs;
        if (first_tok < cursor_tok) continue;
        if (first_tok > end_tok || last_tok > end_tok) continue;
        if (first_tok > cursor_tok) {
            uint64_t gb = ir->tokens.v[cursor_tok].byte_start;
            uint64_t ge = ir->tokens.v[first_tok - 1].byte_end;
            if (ge > gb) emit_source_range_with_properties(ir, out, gb, ge);
        }
        bs = ir->tokens.v[first_tok].byte_start;
        if (include_targets_cxxe_header(n)) {
            emit_include_directive(ir, n, out);
        } else if (n->kind == N_DECORATOR || n->kind == N_CUSTOM_DECORATOR) {
            /* CXXE-only syntax disappears. */
        } else if (n->kind == N_MATCH_STMT) {
            emit_match_statement(ir, n, out);
        } else if (n->kind == N_PROPERTY) {
            emit_property(ir, n, out);
        } else if (n->kind == N_UNARY_EXPR) {
            emit_property_unary(ir, n, out);
        } else if ((n->kind == N_FUNCTION || n->kind == N_METHOD) && function_has_custom_decorator(ir, n)) {
            emit_function_with_custom_decorators(ir, n, out);
        } else if (n->kind == N_PROPERTY_ACCESS) {
            emit_property_access(ir, n, out);
        } else if (n->kind == N_ASSIGN_EXPR) {
            emit_property_assignment(ir, n, out);
        } else if (n->kind == N_GET_MEMBER) {
            emit_get_member_runtime(ir, n, out);
        } else if (node_is_factory_runtime_call(n)) {
            emit_factory_runtime(ir, n, out);
        } else {
            emit_named_call(ir, n, out);
        }
        (void)bs;
        cursor_tok = last_tok + 1;
    }
    if (cursor_tok <= end_tok) {
        uint64_t gb = ir->tokens.v[cursor_tok].byte_start;
        uint64_t ge = ir->tokens.v[end_tok].byte_end;
        if (ge > gb) emit_source_range_with_properties(ir, out, gb, ge);
    }
    free(native_ids);
}

static void emit_cpp(const IR *ir, const char *path) {
    Str out;
    size_t cursor = 0, i, native_count = 0;
    size_t *native_ids = NULL;
    int runtime_features = class_has_runtime_features(ir);
    str_init(&out);

    /* Native nodes are created by several parser passes. In particular,
       property-assignment nodes are intentionally discovered before generic
       expression nodes, so IR creation order is not source order. The source
       cursor below can only be correct when native emission is ordered by the
       covered token range. */
    for (i = 0; i < ir->node_count; ++i) {
        if (!node_needs_native_emit(ir, &ir->nodes[i])) continue;
        if (ir->nodes[i].flags & NF_EXTERNAL_DECL) continue;
        if (ir->nodes[i].first_tok >= ir->tokens.n || ir->nodes[i].last_tok >= ir->tokens.n ||
            ir->nodes[i].first_tok > ir->nodes[i].last_tok) continue;
        native_ids = (size_t *)xrealloc(native_ids, (native_count + 1) * sizeof(*native_ids));
        native_ids[native_count++] = i;
    }
    for (i = 1; i < native_count; ++i) {
        size_t key = native_ids[i], j = i;
        while (j > 0) {
            size_t prev = native_ids[j - 1];
            if (ir->nodes[prev].first_tok < ir->nodes[key].first_tok ||
                (ir->nodes[prev].first_tok == ir->nodes[key].first_tok &&
                 ir->nodes[prev].last_tok <= ir->nodes[key].last_tok)) break;
            native_ids[j] = prev;
            --j;
        }
        native_ids[j] = key;
    }
    if (runtime_features) {
        str_put(&out, "#include \"cxxe_runtime.hpp\"\nnamespace cxxe_generated { static void register_all(); }\n");
    }
    for (i = 0; i < native_count; ++i) {
        const IRNode *n = &ir->nodes[native_ids[i]];
        {
            size_t first_tok = (size_t)n->first_tok;
            size_t last_tok = (size_t)n->last_tok;
            if (n->kind == N_FUNC_CALL && n->qualified && strcmp(n->qualified, "stde::factory_find") == 0) {
                size_t open = SIZE_MAX, close = SIZE_MAX, j;
                int d = 0;
                if (first_tok > 0 && strcmp(ir->tokens.v[first_tok - 1].text, "new") == 0 && find_call_close(ir, n, &open, &close)) {
                    j = close + 1;
                    if (j + 1 < ir->tokens.n && strcmp(ir->tokens.v[j].text, "(") == 0 && strcmp(ir->tokens.v[j + 1].text, ")") == 0) {
                        (void)d;
                        first_tok--;
                        last_tok = j + 1;
                    }
                }
            }
            uint64_t bs = ir->tokens.v[first_tok].byte_start;
            uint64_t be = ir->tokens.v[last_tok].byte_end;
            if (bs < cursor) continue;
            if (bs > ir->source_size || be > ir->source_size || be < bs) continue;
            emit_source_range_with_properties(ir, &out, cursor, bs);
            if (include_targets_cxxe_header(n)) { emit_include_directive(ir, n, &out); }
            else if (n->kind == N_DECORATOR || n->kind == N_CUSTOM_DECORATOR) { /* CXXE-only syntax. */ }
            else if (n->kind == N_MATCH_STMT) { emit_match_statement(ir, n, &out); }
            else if (n->kind == N_PROPERTY) { emit_property(ir, n, &out); }
            else if (n->kind == N_UNARY_EXPR) { emit_property_unary(ir, n, &out); }
            else if ((n->kind == N_FUNCTION || n->kind == N_METHOD) && function_has_custom_decorator(ir, n)) {
                emit_function_with_custom_decorators(ir, n, &out);
            }
            else if (n->kind == N_PROPERTY_ACCESS) {
                emit_property_access(ir, n, &out);
            }
            else if (n->kind == N_ASSIGN_EXPR) {
                emit_property_assignment(ir, n, &out);
            }
            else if (n->kind == N_GET_MEMBER) {
                emit_get_member_runtime(ir, n, &out);
            }
            else if (node_is_factory_runtime_call(n)) emit_factory_runtime(ir, n, &out);
            else emit_named_call(ir, n, &out);
            cursor = (size_t)be;
        }
    }
    if (cursor < ir->source_size) emit_source_range_with_properties(ir, &out, cursor, (uint64_t)ir->source_size);
    if (runtime_features) emit_registered_classes(ir, &out);
    write_bytes(path, out.data ? out.data : "", out.len);
    free(native_ids);
    str_free(&out);
}

void emit_exact(const IR *ir, const char *path) {
    emit_cpp(ir, path);
}
