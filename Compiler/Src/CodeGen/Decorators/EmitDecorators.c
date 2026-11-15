/*
 * CodeGen/Decorators/EmitDecorators.c
 *
 * Custom decorator expansion.
 */

#include "Core/Common.h"
#include "CodeGen/CodeGenInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Lexer.h"
#include "Lexer/Token.h"

static size_t split_top_args(const IR *ir, size_t a, size_t b, size_t *starts, size_t *ends, size_t cap) {
    size_t count = 0, start = a, i;
    int par = 0, br = 0, cur = 0, angle = 0;
    if (a > b) return 0;
    for (i = a; i <= b; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (strcmp(t, "(") == 0) par++;
        else if (strcmp(t, ")") == 0 && par) par--;
        else if (strcmp(t, "[") == 0) br++;
        else if (strcmp(t, "]") == 0 && br) br--;
        else if (strcmp(t, "{") == 0) cur++;
        else if (strcmp(t, "}") == 0 && cur) cur--;
        else if (strcmp(t, "<") == 0) angle++;
        else if (strcmp(t, ">") == 0 && angle) angle--;
        else if (strcmp(t, ",") == 0 && par == 0 && br == 0 && cur == 0 && angle == 0) {
            if (start <= i - 1 && count < cap) { starts[count] = start; ends[count] = i - 1; count++; }
            start = i + 1;
        }
    }
    if (start <= b && count < cap) { starts[count] = start; ends[count] = b; count++; }
    return count;
}

static int decorator_invocation_args(const IR *ir, const IRNode *dec, size_t *starts, size_t *ends, size_t cap, size_t *count_out) {
    size_t a = (size_t)dec->first_tok, b = (size_t)dec->last_tok, i, open = SIZE_MAX, close = SIZE_MAX;
    if (a > b || b >= ir->tokens.n) return 0;
    for (i = a + 1; i <= b; ++i) {
        if (strcmp(ir->tokens.v[i].text, "(") == 0) { open = i; break; }
    }
    if (open == SIZE_MAX) { if (count_out) *count_out = 0; return 1; }
    {
        int d = 0;
        for (i = open; i <= b; ++i) {
            if (strcmp(ir->tokens.v[i].text, "(") == 0) d++;
            else if (strcmp(ir->tokens.v[i].text, ")") == 0 && --d == 0) { close = i; break; }
        }
    }
    if (close == SIZE_MAX) return 0;
    if (count_out) *count_out = split_top_args(ir, open + 1, close - 1, starts, ends, cap);
    return 1;
}

static int custom_decorator_template_param_ids(const IR *ir, const IRNode *cd, int64_t *ids, size_t cap) {
    size_t n = 0; int64_t c, pc;
    for (c = cd->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *tpl = &ir->nodes[c];
        if (tpl->kind != N_TEMPLATE) continue;
        for (pc = tpl->first_child; pc >= 0; pc = ir->nodes[pc].next_sibling) {
            const IRNode *p = &ir->nodes[pc];
            if (p->kind == N_TEMPLATE_TYPE_PARAM || p->kind == N_TEMPLATE_NON_TYPE_PARAM || p->kind == N_TEMPLATE_TEMPLATE_PARAM) {
                if (ids && n < cap) ids[n] = pc;
                n++;
            }
        }
        break;
    }
    return (int)n;
}

int decorator_template_arg_ranges(const IR *ir, const IRNode *dec, size_t *starts, size_t *ends, size_t cap) {
    int64_t c, a; size_t n = 0;
    for (c = dec->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *tpl = &ir->nodes[c];
        if (tpl->kind != N_DECORATOR_TEMPLATE) continue;
        for (a = tpl->first_child; a >= 0; a = ir->nodes[a].next_sibling) {
            if (ir->nodes[a].kind == N_DECORATOR_TEMPLATE_ARG) {
                if (starts && ends && n < cap) { starts[n] = (size_t)ir->nodes[a].first_tok; ends[n] = (size_t)ir->nodes[a].last_tok; }
                n++;
            }
        }
        return (int)n;
    }
    return 0;
}

size_t custom_decorator_template_param_count(const IR *ir, const IRNode *cd) {
    return (size_t)custom_decorator_template_param_ids(ir, cd, NULL, 0);
}

static int custom_decorator_target_param_ids(const IR *ir, const IRNode *cd, int64_t *ids, size_t cap) {
    int64_t c, target = -1;
    size_t n = 0;
    for (c = cd->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind == N_DECORATOR_TARGET) { target = c; break; }
    }
    if (target < 0) return 0;
    for (c = ir->nodes[target].first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind == N_PARAM_DECL) {
            if (ids && n < cap) ids[n] = c;
            n++;
        }
    }
    return (int)n;
}

static size_t custom_decorator_param_ids(const IR *ir, const IRNode *cd, int64_t *ids, size_t cap) {
    size_t n = 0; int64_t c;
    for (c = cd->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind == N_DECORATOR_TARGET) break;
        if (ir->nodes[c].kind == N_PARAM_DECL) {
            if (ids && n < cap) ids[n] = c;
            n++;
        }
    }
    return n;
}

static int custom_decorator_is_variadic(const IR *ir, const IRNode *cd) {
    int64_t c;
    for (c = cd->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind == N_DECORATOR_TARGET)
            return (ir->nodes[c].flags & NF_DECORATOR_VARIADIC) != 0;
    }
    return 0;
}

static void emit_actual_function_args(const IR *ir, const IRNode *fn, Str *out) {
    int first = 1;
    int64_t c;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *p = &ir->nodes[c];
        if (p->kind != N_PARAM_DECL) continue;
        if (!first) str_put(out, ", ");
        first = 0;
        if (p->name && *p->name) str_put(out, p->name);
        else {
            /* A parameter without a source name cannot be forwarded by name.
               C++ function definitions normally have names when a decorator
               uses func(...); keep the generated call valid for the common
               unnamed-parameter case by omitting that argument. */
        }
    }
}

static const char *actual_function_param_name(const IR *ir, const IRNode *fn, size_t index) {
    size_t n = 0; int64_t c;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind != N_PARAM_DECL) continue;
        if (n++ == index) return ir->nodes[c].name;
    }
    return NULL;
}

static size_t find_token_matching_paren(const IR *ir, size_t open, size_t end_exclusive) {
    size_t i;
    int depth = 0;
    if (open >= ir->tokens.n || strcmp(ir->tokens.v[open].text, "(") != 0) return SIZE_MAX;
    for (i = open; i < end_exclusive && i < ir->tokens.n; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (strcmp(t, "(") == 0) depth++;
        else if (strcmp(t, ")") == 0) {
            if (--depth == 0) return i;
        }
    }
    return SIZE_MAX;
}

static size_t find_matching_paren_tv(const TokenVec *tv, size_t open, size_t end_exclusive) {
    size_t i; int d = 0;
    if (!tv || open >= tv->n || strcmp(tv->v[open].text, "(") != 0) return SIZE_MAX;
    for (i = open; i < end_exclusive && i < tv->n; ++i) {
        if (strcmp(tv->v[i].text, "(") == 0) d++;
        else if (strcmp(tv->v[i].text, ")") == 0 && --d == 0) return i;
    }
    return SIZE_MAX;
}

static size_t external_body_open(const TokenVec *tv) {
    size_t i;
    if (!tv) return SIZE_MAX;
    for (i = 0; i < tv->n; ++i) if (strcmp(tv->v[i].text, "{") == 0) return i;
    return SIZE_MAX;
}

static void emit_custom_decorator_body_external(const IR *ir, const IRNode *cd, const IRNode *fn, const IRNode *dec, const char *current_callee, Str *out) {
    TokenVec tv; size_t i, bo, bc, dec_starts[64], dec_ends[64], dec_count = 0; int64_t dparams[64], tparams[64];
    size_t dpn, d, tstarts[64], tends[64], tpn = 0, tac = 0;
    memset(&tv, 0, sizeof(tv));
    if (!cd->value) return;
    lex_cpp((const uint8_t *)cd->value, strlen(cd->value), &tv);
    bo = external_body_open(&tv);
    if (bo == SIZE_MAX) { tokens_free(&tv); return; }
    bc = SIZE_MAX;
    if (tv.n) {
        size_t q = tv.n;
        while (q > 0) {
            --q;
            if (tv.v[q].kind != TK_EOF) { bc = q; break; }
        }
    }
    if (bc == SIZE_MAX) { tokens_free(&tv); return; }
    if (bc > bo && strcmp(tv.v[bc].text, "}") == 0) bc--;
    dpn = custom_decorator_param_ids(ir, cd, dparams, 64);
    if (!decorator_invocation_args(ir, dec, dec_starts, dec_ends, 64, &dec_count)) ir_die_at(ir, dec->first_tok, "malformed custom decorator invocation '%s'", dec->name ? dec->name : "<unnamed>");
    if (dec_count != dpn) ir_die_at(ir, dec->first_tok, "decorator '%s' expects %zu argument(s), got %zu", cd->name ? cd->name : "<unnamed>", dpn, dec_count);
    tpn = (size_t)custom_decorator_template_param_ids(ir, cd, tparams, 64);
    tac = (size_t)decorator_template_arg_ranges(ir, dec, tstarts, tends, 64);

    for (i = bo + 1; i <= bc; ++i) {
        const Token *t = &tv.v[i]; int replaced = 0;
        if (t->kind == TK_IDENTIFIER) {
            if (strcmp(t->text, "func") == 0 && i + 1 <= bc && strcmp(tv.v[i + 1].text, "(") == 0) {
                size_t close = find_matching_paren_tv(&tv, i + 1, bc + 1);
                str_put(out, t->trivia); str_put(out, current_callee);
                if (close != SIZE_MAX) {
                    if (custom_decorator_is_variadic(ir, cd)) {
                        str_ch(out, '('); emit_actual_function_args(ir, fn, out); str_ch(out, ')');
                        i = close;
                    } else {
                        /* For a fixed func(...) signature, the normal parameter-name mapping
                           remains available through the generic emitter below; leave the call
                           untouched rather than guessing semantics. */
                    }
                }
                replaced = 1;
            }
            if (!replaced) {
                size_t ti;
                for (ti = 0; ti < tpn && ti < tac; ++ti) {
                    const char *tp_name = ir->nodes[tparams[ti]].name;
                    if (tp_name && strcmp(t->text, tp_name) == 0) {
                        str_put(out, t->trivia);
                        emit_tokens_range(ir, out, tstarts[ti], tends[ti], NULL, SIZE_MAX);
                        replaced = 1;
                        break;
                    }
                }
            }
            if (!replaced) {
                for (d = 0; d < dpn; ++d) {
                    const char *pn = ir->nodes[dparams[d]].name;
                    if (pn && strcmp(t->text, pn) == 0) {
                        str_put(out, t->trivia); str_ch(out, '(');
                        emit_tokens_range(ir, out, dec_starts[d], dec_ends[d], NULL, SIZE_MAX);
                        str_ch(out, ')'); replaced = 1; break;
                    }
                }
            }
            if (!replaced) {
                int64_t ids[128]; int tn = custom_decorator_target_param_ids(ir, cd, ids, 128); size_t ti;
                for (ti = 0; ti < (size_t)tn; ++ti) {
                    if (ir->nodes[ids[ti]].name && strcmp(ir->nodes[ids[ti]].name, t->text) == 0) {
                        const char *actual = actual_function_param_name(ir, fn, ti);
                        if (actual) { str_put(out, t->trivia); str_put(out, actual); replaced = 1; }
                        break;
                    }
                }
            }
        }
        if (!replaced) { str_put(out, t->trivia); str_put(out, t->text); }
    }
    tokens_free(&tv);
}

static void emit_custom_decorator_body(const IR *ir, const IRNode *cd, const IRNode *fn, const IRNode *dec, const char *current_callee, Str *out) {
    if (cd->flags & NF_EXTERNAL_DECL) {
        emit_custom_decorator_body_external(ir, cd, fn, dec, current_callee, out);
        return;
    }
    size_t body_open = function_body_open(ir, cd);
    size_t body_close = (size_t)cd->last_tok;
    size_t dec_starts[64], dec_ends[64], dec_count = 0;
    int64_t dparams[64], tparams[64];
    size_t tstarts[64], tends[64], tpn = 0, tac = 0;
    size_t dpn = custom_decorator_param_ids(ir, cd, dparams, 64);
    size_t i;
    if (body_open == SIZE_MAX || body_open >= body_close) return;
    if (body_close > body_open && strcmp(ir->tokens.v[body_close].text, "}") == 0) body_close--;
    if (!decorator_invocation_args(ir, dec, dec_starts, dec_ends, 64, &dec_count)) ir_die_at(ir, dec->first_tok, "malformed custom decorator invocation '%s'", dec->name ? dec->name : "<unnamed>");
    if (dec_count != dpn) ir_die_at(ir, dec->first_tok, "decorator '%s' expects %zu argument(s), got %zu", cd->name ? cd->name : "<unnamed>", dpn, dec_count);
    tpn = (size_t)custom_decorator_template_param_ids(ir, cd, tparams, 64);
    tac = (size_t)decorator_template_arg_ranges(ir, dec, tstarts, tends, 64);

    for (i = body_open + 1; i <= body_close; ++i) {
        const Token *t = &ir->tokens.v[i];
        size_t di;
        if (t->kind == TK_IDENTIFIER) {
            int replaced = 0;
            if (strcmp(t->text, "func") == 0 && i + 1 <= body_close && strcmp(ir->tokens.v[i + 1].text, "(") == 0) {
                str_put(out, t->trivia);
                str_put(out, current_callee);
                if (custom_decorator_is_variadic(ir, cd)) {
                    size_t close = find_token_matching_paren(ir, i + 1, body_close + 1);
                    if (close == SIZE_MAX) ir_die_at(ir, dec->first_tok, "malformed func(...) in custom decorator '%s'", cd->name ? cd->name : "<unnamed>");
                    str_ch(out, '(');
                    emit_actual_function_args(ir, fn, out);
                    str_ch(out, ')');
                    i = close;
                }
                replaced = 1;
            }
            if (!replaced) {
                size_t ti;
                for (ti = 0; ti < tpn && ti < tac; ++ti) {
                    const char *tp_name = ir->nodes[tparams[ti]].name;
                    if (tp_name && strcmp(t->text, tp_name) == 0) {
                        str_put(out, t->trivia);
                        emit_tokens_range(ir, out, tstarts[ti], tends[ti], NULL, SIZE_MAX);
                        replaced = 1;
                        break;
                    }
                }
            }
            if (!replaced) {
                for (di = 0; di < dpn; ++di) {
                    const char *pn = ir->nodes[dparams[di]].name;
                    if (pn && strcmp(t->text, pn) == 0) {
                        size_t s = dec_starts[di], e = dec_ends[di];
                        str_put(out, t->trivia);
                        str_ch(out, '(');
                        emit_tokens_range(ir, out, s, e, NULL, SIZE_MAX);
                        str_ch(out, ')');
                        replaced = 1;
                        break;
                    }
                }
            }
            if (!replaced) {
                int64_t found_target = -1; size_t target_index = 0;
                int64_t ids[128]; int tn = custom_decorator_target_param_ids(ir, cd, ids, 128);
                for (target_index = 0; target_index < (size_t)tn; ++target_index) {
                    if (ir->nodes[ids[target_index]].name && strcmp(ir->nodes[ids[target_index]].name, t->text) == 0) { found_target = ids[target_index]; break; }
                }
                if (found_target >= 0) {
                    const char *actual = actual_function_param_name(ir, fn, target_index);
                    if (actual) {
                        str_put(out, t->trivia);
                        str_put(out, actual);
                        replaced = 1;
                    }
                }
            }
            if (!replaced) {
                str_put(out, t->trivia);
                str_put(out, t->text);
            }
        } else {
            str_put(out, t->trivia);
            str_put(out, t->text);
        }
    }
}

void emit_function_with_custom_decorators(const IR *ir, const IRNode *fn, Str *out) {
    int64_t custom_ids[64];
    size_t custom_count = 0, i;
    int64_t c;
    size_t body_open = function_body_open(ir, fn);
    size_t name_tok = function_name_token(ir, fn);
    if (body_open == SIZE_MAX || name_tok == SIZE_MAX) ir_die_at(ir, fn->first_tok, "custom decorators require a function definition with a named function");
    if (!(fn->kind == N_FUNCTION || fn->kind == N_METHOD)) ir_die_at(ir, fn->first_tok, "custom decorators can only be applied to functions or methods");
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *d = &ir->nodes[c];
        if (d->kind == N_DECORATOR && (d->flags & NF_DECORATOR_CUSTOM)) {
            if (custom_count < 64) custom_ids[custom_count] = c;
            custom_count++;
        }
    }
    if (custom_count > 64) ir_die_at(ir, fn->first_tok, "too many custom decorators on function '%s'", fn->name ? fn->name : "<unnamed>");

    /* Base implementation: same signature/body, with a private unique name. */
    {
        char base_name[256];
        snprintf(base_name, sizeof(base_name), "%s__cxxe_impl_%" PRIu64, fn->name ? fn->name : "function", (uint64_t)(fn - ir->nodes));
        emit_tokens_range(ir, out, (size_t)fn->first_tok, body_open - 1, base_name, name_tok);
        str_put(out, " {");
        if (body_open + 1 <= (size_t)fn->last_tok) emit_native_token_range(ir, out, body_open + 1, (size_t)fn->last_tok - 1);
        str_put(out, "}\n\n");

        /* Apply decorators from nearest-to-function outward. The first written
           decorator is therefore the outermost one, matching common decorator semantics. */
        {
            char current[256];
            snprintf(current, sizeof(current), "%s", base_name);
            for (i = custom_count; i-- > 0;) {
                const IRNode *dec = &ir->nodes[custom_ids[i]];
                const IRNode *cd = find_custom_decorator_def(ir, dec->name, dec->qualified);
                char wrapper_name[256];
                if (!cd) continue;
                if (i == 0) snprintf(wrapper_name, sizeof(wrapper_name), "%s", fn->name);
                else snprintf(wrapper_name, sizeof(wrapper_name), "%s__cxxe_decorator_%" PRIu64, fn->name ? fn->name : "function", (uint64_t)custom_ids[i]);
                emit_tokens_range(ir, out, (size_t)fn->first_tok, body_open - 1, wrapper_name, name_tok);
                str_put(out, " {");
                emit_custom_decorator_body(ir, cd, fn, dec, current, out);
                str_put(out, "}\n\n");
                snprintf(current, sizeof(current), "%s", wrapper_name);
            }
        }
    }
}
