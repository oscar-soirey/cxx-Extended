/*
 * CodeGen/EmitSource.c
 *
 * Token-range emission with property rewriting.
 */

#include "Core/Common.h"
#include "CodeGen/CodeGenInternal.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"
#include "Sema/Resolve.h"
#include "Sema/TypeLookup.h"

const IRNode *find_custom_decorator_def(const IR *ir, const char *name, const char *qualified_name) {
    size_t i;
    IRNode fake;
    memset(&fake, 0, sizeof(fake));
    fake.name = (char *)name;
    fake.qualified = (char *)qualified_name;
    {
        int64_t id = -1;
        if (find_custom_decorator_use(ir, &fake, &id)) return id >= 0 ? &ir->nodes[id] : NULL;
    }
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (n->kind == N_CUSTOM_DECORATOR && n->name && name && strcmp(n->name, name) == 0) return n;
    }
    return NULL;
}

int function_has_custom_decorator(const IR *ir, const IRNode *fn) {
    int64_t c;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *d = &ir->nodes[c];
        if (d->kind == N_DECORATOR && (d->flags & NF_DECORATOR_CUSTOM)) return 1;
    }
    return 0;
}

size_t function_body_open(const IR *ir, const IRNode *fn) {
    size_t i;
    for (i = (size_t)fn->first_tok; i <= (size_t)fn->last_tok && i < ir->tokens.n; ++i)
        if (strcmp(ir->tokens.v[i].text, "{") == 0) return i;
    return SIZE_MAX;
}

size_t function_name_token(const IR *ir, const IRNode *fn) {
    size_t i;
    if (!fn->name) return SIZE_MAX;
    for (i = (size_t)fn->first_tok; i <= (size_t)fn->last_tok && i < ir->tokens.n; ++i) {
        if (strcmp(ir->tokens.v[i].text, fn->name) == 0) {
            if (i + 1 < ir->tokens.n && (strcmp(ir->tokens.v[i + 1].text, "(") == 0 || strcmp(ir->tokens.v[i + 1].text, "<") == 0)) return i;
        }
    }
    return SIZE_MAX;
}

static int property_read_context(const IR *ir, size_t i, size_t b) {
    size_t n = i + 2;
    if (n > b || n >= ir->tokens.n) return 1;
    /* Writes are emitted by the dedicated property-assignment path. Do not
       turn their left-hand side into a getter here. */
    if (!strcmp(ir->tokens.v[n].text, "=") || !strcmp(ir->tokens.v[n].text, "+=") ||
        !strcmp(ir->tokens.v[n].text, "-=") || !strcmp(ir->tokens.v[n].text, "*=") ||
        !strcmp(ir->tokens.v[n].text, "/=") || !strcmp(ir->tokens.v[n].text, "%=") ||
        !strcmp(ir->tokens.v[n].text, "++") || !strcmp(ir->tokens.v[n].text, "--")) return 0;
    return 1;
}

void emit_tokens_range(const IR *ir, Str *out, size_t a, size_t b, const char *replace_name, size_t replace_tok) {
    size_t i;
    if (a > b || b >= ir->tokens.n) return;
    for (i = a; i <= b; ++i) {
        const char *text = ir->tokens.v[i].text;
        str_put(out, ir->tokens.v[i].trivia);
        if (i == replace_tok && replace_name) {
            str_put(out, replace_name);
            continue;
        }
        if (strcmp(text, "dynamic") == 0) {
            str_put(out, "stde::DynamicValue");
            continue;
        }
        if ((strcmp(text, ".") == 0 || strcmp(text, "->") == 0) &&
            i > a && i + 1 <= b && i + 1 < ir->tokens.n &&
            ir->tokens.v[i + 1].kind == TK_IDENTIFIER && property_read_context(ir, i, b)) {
            int64_t pid = -1;
            if (resolve_property_for_member_ir(ir, i, ir->tokens.v[i - 1].text,
                                                ir->tokens.v[i + 1].text, &pid)) {
                str_put(out, text);
                str_put(out, "get_");
                str_put(out, ir->nodes[pid].name ? ir->nodes[pid].name : ir->tokens.v[i + 1].text);
                str_put(out, "()");
                i++;
                continue;
            }
        }
        str_put(out, text);
    }
}

/* Emit a source-byte interval while applying CXXE's transparent property
   getter rewrite. Using byte offsets here lets us handle property reads in
   arbitrary expressions, including variable initializers and call arguments,
   even when the parser did not create a dedicated N_PROPERTY_ACCESS node. */

void emit_source_range_with_properties(const IR *ir, Str *out, uint64_t begin, uint64_t end) {
    size_t i;
    uint64_t pos = begin;
    if (begin >= end) return;
    for (i = 0; i < ir->tokens.n; ++i) {
        const Token *t = &ir->tokens.v[i];
        if (t->kind == TK_EOF) break;
        if (t->byte_end <= begin) continue;
        if (t->byte_start >= end) break;
        if (t->byte_start > pos) {
            uint64_t gap_end = t->byte_start < end ? t->byte_start : end;
            if (gap_end > pos) str_putn(out, (const char *)ir->source + pos, (size_t)(gap_end - pos));
        }
        if (strcmp(t->text, "dynamic") == 0) {
            str_put(out, "stde::DynamicValue");
            if (t->byte_end > pos) pos = t->byte_end;
            continue;
        }
        if ((strcmp(t->text, ".") == 0 || strcmp(t->text, "->") == 0) &&
            i + 1 < ir->tokens.n && ir->tokens.v[i + 1].kind == TK_IDENTIFIER &&
            ir->tokens.v[i + 1].byte_start < end) {
            int64_t pid = -1;
            if (resolve_property_for_member_ir(ir, i, ir->tokens.v[i - 1].text,
                                                ir->tokens.v[i + 1].text, &pid)) {
                str_put(out, t->text);
                str_put(out, "get_");
                str_put(out, ir->nodes[pid].name ? ir->nodes[pid].name : ir->tokens.v[i + 1].text);
                str_put(out, "()");
                pos = ir->tokens.v[i + 1].byte_end;
                ++i;
                continue;
            }
        }
        if (t->byte_end > pos) {
            uint64_t token_begin = t->byte_start > pos ? t->byte_start : pos;
            uint64_t token_end = t->byte_end < end ? t->byte_end : end;
            if (token_end > token_begin) str_putn(out, (const char *)ir->source + token_begin, (size_t)(token_end - token_begin));
        }
        if (t->byte_end > pos) pos = t->byte_end;
        if (pos >= end) break;
    }
    if (pos < end) str_putn(out, (const char *)ir->source + pos, (size_t)(end - pos));
}
