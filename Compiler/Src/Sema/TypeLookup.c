/*
 * Sema/TypeLookup.c
 *
 * IR queries: enclosing callables, local declaration types, property resolution.
 */

#include "Core/Common.h"
#include "Sema/TypeLookup.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"

static int is_callable_kind(NodeKind k) {
    return k == N_FUNCTION || k == N_METHOD || k == N_CONSTRUCTOR ||
           k == N_DESTRUCTOR || k == N_OPERATOR_FUNCTION;
}

static int64_t find_enclosing_callable_ir(const IR *ir, size_t tok) {
    size_t i;
    int64_t best = -1;
    uint64_t best_span = UINT64_MAX;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        uint64_t span;
        if (!is_callable_kind(n->kind) || n->first_tok > tok || n->last_tok < tok) continue;
        span = n->last_tok - n->first_tok;
        if (span < best_span) {
            best_span = span;
            best = (int64_t)i;
        }
    }
    return best;
}

static int64_t find_enclosing_type_owner(const IR *ir, int64_t node) {
    int guard = 0;
    while (node >= 0 && (size_t)node < ir->node_count && guard++ < 1024) {
        const IRNode *n = &ir->nodes[node];
        if (n->kind == N_CLASS || n->kind == N_STRUCT || n->kind == N_UNION) return node;
        node = n->parent;
    }
    return -1;
}

static size_t ir_callable_body_open(const IR *ir, const IRNode *fn) {
    size_t i;
    int par = 0, br = 0, cur = 0;
    if (!fn || !is_callable_kind(fn->kind) || fn->first_tok >= ir->tokens.n) return SIZE_MAX;
    for (i = (size_t)fn->first_tok; i <= (size_t)fn->last_tok && i < ir->tokens.n; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (!strcmp(t, "(")) par++;
        else if (!strcmp(t, ")") && par) par--;
        else if (!strcmp(t, "[")) br++;
        else if (!strcmp(t, "]") && br) br--;
        else if (!strcmp(t, "{") && par == 0 && br == 0) { cur++; return i; }
    }
    return SIZE_MAX;
}

static char *normalize_type_tokens_ir(const IR *ir, size_t a, size_t b) {
    Str s;
    size_t i;
    int need_ident = 0;
    if (a > b || a >= ir->tokens.n) return xstrdup0("");
    if (b >= ir->tokens.n) b = ir->tokens.n - 1;
    str_init(&s);
    for (i = a; i <= b; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (!strcmp(t, "const") || !strcmp(t, "volatile") || !strcmp(t, "static") ||
            !strcmp(t, "mutable") || !strcmp(t, "typename") || !strcmp(t, "struct") ||
            !strcmp(t, "class") || !strcmp(t, "union") || !strcmp(t, "enum") ||
            !strcmp(t, "*") || !strcmp(t, "&") || !strcmp(t, "&&")) continue;
        if (ir->tokens.v[i].kind == TK_IDENTIFIER || !strcmp(t, "::") ||
            (t[0] && (isalpha((unsigned char)t[0]) || t[0] == '_'))) {
            if (need_ident && strcmp(t, "::") != 0) str_ch(&s, ' ');
            str_put(&s, t);
            need_ident = strcmp(t, "::") != 0;
        }
    }
    return str_take(&s);
}

static int64_t callable_parameter_for_name(const IR *ir, const IRNode *fn, const char *name) {
    int64_t c;
    if (!fn || !name) return -1;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *p = &ir->nodes[c];
        if (p->kind == N_PARAM_DECL && p->name && strcmp(p->name, name) == 0) return c;
    }
    return -1;
}

static int extract_local_decl_type_ir(const IR *ir, size_t a, size_t b, const char *name, Str *out) {
    size_t i, name_i = SIZE_MAX, eq = SIZE_MAX, init_open = SIZE_MAX;
    int par = 0, br = 0, cur = 0;
    if (a > b || !name || !*name || !out) return 0;
    if (b >= ir->tokens.n) b = ir->tokens.n - 1;
    for (i = a; i <= b; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (!strcmp(t, "(")) par++;
        else if (!strcmp(t, ")") && par) par--;
        else if (!strcmp(t, "[")) br++;
        else if (!strcmp(t, "]") && br) br--;
        else if (!strcmp(t, "{") && !par && !br && cur == 0) { init_open = i; break; }
        else if (!strcmp(t, "{")) cur++;
        else if (!strcmp(t, "}") && cur) cur--;
        else if (!par && !br && !cur && !strcmp(t, "=") && eq == SIZE_MAX) eq = i;
    }
    {
        size_t end = b;
        if (eq != SIZE_MAX) end = eq - 1;
        else if (init_open != SIZE_MAX) end = init_open - 1;
        while (end >= a) {
            if (ir->tokens.v[end].kind == TK_IDENTIFIER && !strcmp(ir->tokens.v[end].text, name)) {
                name_i = end;
                break;
            }
            if (end == a) break;
            --end;
        }
    }
    if (name_i == SIZE_MAX || name_i == a) return 0;
    {
        char *norm = normalize_type_tokens_ir(ir, a, name_i - 1);
        if (norm[0]) {
            str_put(out, norm);
            free(norm);
            return 1;
        }
        free(norm);
    }
    return 0;
}

static int find_local_decl_type_ir(const IR *ir, const IRNode *fn, size_t before_tok,
                                   const char *name, Str *out) {
    size_t body, i, start, target_depth = 0;
    int par = 0, br = 0, cur = 0;
    Str best;
    if (!fn || !name || !*name || !out) return 0;
    body = ir_callable_body_open(ir, fn);
    if (body == SIZE_MAX || before_tok <= body) return 0;
    if (before_tok > fn->last_tok) before_tok = fn->last_tok;

    /* First determine the lexical block depth at the member access. A
       declaration from a block that has already closed is not visible. */
    for (i = body + 1; i < before_tok && i < ir->tokens.n; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (!strcmp(t, "(")) par++;
        else if (!strcmp(t, ")") && par) par--;
        else if (!strcmp(t, "[")) br++;
        else if (!strcmp(t, "]") && br) br--;
        else if (!strcmp(t, "{") && !par && !br) cur++;
        else if (!strcmp(t, "}") && cur && !par && !br) cur--;
    }
    target_depth = (size_t)cur;

    /* Second pass: keep the latest declaration of the same identifier that
       is still in an enclosing lexical block. This preserves normal shadowing
       without resolving a variable from a sibling block after that block ends. */
    par = br = cur = 0;
    start = body + 1;
    str_init(&best);
    for (i = body + 1; i < before_tok && i < ir->tokens.n; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (!strcmp(t, "(")) par++;
        else if (!strcmp(t, ")") && par) par--;
        else if (!strcmp(t, "[")) br++;
        else if (!strcmp(t, "]") && br) br--;
        else if (!strcmp(t, "{") && !par && !br) { cur++; start = i + 1; }
        else if (!strcmp(t, "}") && cur && !par && !br) { cur--; start = i + 1; }
        else if (!strcmp(t, ";") && !par && !br) {
            if ((size_t)cur <= target_depth) {
                Str candidate;
                str_init(&candidate);
                if (start <= i && extract_local_decl_type_ir(ir, start, i, name, &candidate)) {
                    str_free(&best);
                    best = candidate;
                } else {
                    str_free(&candidate);
                }
            }
            start = i + 1;
        }
    }
    if (best.len) {
        str_putn(out, best.data, best.len);
        str_free(&best);
        return 1;
    }
    str_free(&best);
    return 0;
}

static const char *property_owner_name(const IR *ir, int64_t prop_id) {
    int64_t p;
    if (prop_id < 0 || (size_t)prop_id >= ir->node_count) return NULL;
    p = ir->nodes[prop_id].parent;
    while (p >= 0 && (size_t)p < ir->node_count) {
        const IRNode *n = &ir->nodes[p];
        if (n->kind == N_CLASS || n->kind == N_STRUCT || n->kind == N_UNION) return n->qualified ? n->qualified : n->name;
        p = n->parent;
    }
    return NULL;
}

static int property_matches_type_context(const IR *ir, int64_t fn_id, const char *type_name, int64_t prop_id) {
    const IRNode *fn;
    const IRNode *prop;
    const char *owner;
    const char *leaf;
    size_t i;
    int unique = 1;
    Str scope;
    if (!type_name || !*type_name || fn_id < 0 || (size_t)fn_id >= ir->node_count ||
        prop_id < 0 || (size_t)prop_id >= ir->node_count) return 0;
    fn = &ir->nodes[fn_id];
    prop = &ir->nodes[prop_id];
    owner = property_owner_name(ir, prop_id);
    if (!owner || !*owner) return 0;
    if (!strcmp(type_name, owner)) return 1;
    leaf = owner;
    for (i = 0; owner[i]; ++i) if (owner[i] == ':' && owner[i + 1] == ':') leaf = owner + i + 2;
    if (!strcmp(type_name, leaf)) {
        for (i = 0; i < ir->node_count; ++i) {
            const IRNode *other = &ir->nodes[i];
            const char *oo;
            if (i == (size_t)prop_id || other->kind != N_PROPERTY || !other->name || strcmp(other->name, prop->name)) continue;
            oo = property_owner_name(ir, (int64_t)i);
            if (oo && !strcmp(oo, owner)) continue;
            if (oo) {
                const char *ol = oo;
                size_t q;
                for (q = 0; oo[q]; ++q) if (oo[q] == ':' && oo[q + 1] == ':') ol = oo + q + 2;
                if (!strcmp(ol, leaf)) unique = 0;
            }
        }
        if (unique) return 1;
        str_init(&scope);
        if (fn->qualified && *fn->qualified) {
            const char *q = strrchr(fn->qualified, ':');
            size_t n = q ? (size_t)(q - fn->qualified) : 0;
            if (q && q > fn->qualified && fn->qualified[q - fn->qualified - 1] == ':') --n;
            if (n) str_putn(&scope, fn->qualified, n);
            /* A member function's qualified prefix ends in its owner type. */
            {
                int64_t owner_cls = find_enclosing_type_owner(ir, fn_id);
                if (owner_cls >= 0 && ir->nodes[owner_cls].qualified && scope.len) {
                    size_t olen = strlen(ir->nodes[owner_cls].qualified);
                    if (scope.len >= olen && !strncmp(scope.data, ir->nodes[owner_cls].qualified, olen) &&
                        (scope.len == olen || scope.data[olen] == ':')) scope.len = olen;
                }
            }
        }
        if (scope.len) {
            int64_t owner_cls = find_enclosing_type_owner(ir, fn_id);
            if (owner_cls >= 0 && ir->nodes[owner_cls].qualified) {
                /* For a member function, an unqualified local type is looked
                   up in the namespace containing the owner type, not inside
                   the owner itself. */
                const char *oq = ir->nodes[owner_cls].qualified;
                const char *last_colon = strrchr(oq, ':');
                size_t ns_len = last_colon ? (size_t)(last_colon - oq) : 0;
                if (last_colon && last_colon > oq && last_colon[-1] == ':') --ns_len;
                scope.len = 0;
                if (ns_len) str_putn(&scope, oq, ns_len);
            }
            if (scope.len) {
                Str scoped;
                str_init(&scoped);
                str_put(&scoped, scope.data); str_put(&scoped, "::"); str_put(&scoped, leaf);
                if (!strcmp(scoped.data ? scoped.data : "", owner)) {
                    str_free(&scoped); str_free(&scope); return 1;
                }
                str_free(&scoped);
            }
        }
        str_free(&scope);
    }
    return 0;
}

/* "const ns::Box<int>&" -> "ns::Box": drops cv/ref/ptr, elaborated
   keywords and template arguments. */
static char *bare_type_name(const char *t) {
    Str o;
    int depth = 0;
    str_init(&o);
    while (*t) {
        if (*t == '<') { depth++; t++; continue; }
        if (*t == '>') { if (depth) depth--; t++; continue; }
        if (depth) { t++; continue; }
        if (isalpha((unsigned char)*t) || *t == '_') {
            const char *w = t;
            size_t n;
            while (isalnum((unsigned char)*t) || *t == '_') t++;
            n = (size_t)(t - w);
            if ((n == 5 && !strncmp(w, "const", 5)) || (n == 8 && !strncmp(w, "volatile", 8)) ||
                (n == 8 && !strncmp(w, "typename", 8)) || (n == 5 && !strncmp(w, "class", 5)) ||
                (n == 6 && !strncmp(w, "struct", 6)) || (n == 5 && !strncmp(w, "union", 5))) continue;
            str_putn(&o, w, n);
            continue;
        }
        if (*t == ':' && t[1] == ':') { if (o.len) str_put(&o, "::"); t += 2; continue; }
        t++;
    }
    return str_take(&o);
}

static int is_known_class_name(const IR *ir, const char *name) {
    size_t i;
    const char *leaf = name;
    if (!name || !*name) return 0;
    for (i = 0; name[i]; ++i) if (name[i] == ':' && name[i + 1] == ':') leaf = name + i + 2;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (n->kind != N_CLASS && n->kind != N_STRUCT && n->kind != N_UNION) continue;
        if ((n->qualified && !strcmp(n->qualified, name)) || (n->name && !strcmp(n->name, leaf))) return 1;
    }
    return 0;
}

static int unique_property_by_name(const IR *ir, const char *member, int64_t *out_id) {
    size_t i;
    int64_t found = -1;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!n->name || strcmp(n->name, member)) continue;
        if (n->kind == N_PROPERTY) {
            if (found >= 0) return 0;
            found = (int64_t)i;
        } else if (n->kind == N_FIELD_DECL && n->parent >= 0) {
            /* A same-named field is fine only if it backs a property. */
            int64_t c, has_prop = 0;
            for (c = ir->nodes[n->parent].first_child; c >= 0; c = ir->nodes[c].next_sibling)
                if (ir->nodes[c].kind == N_PROPERTY && ir->nodes[c].name && !strcmp(ir->nodes[c].name, member)) has_prop = 1;
            if (!has_prop) return 0;
        }
    }
    if (found < 0) return 0;
    if (out_id) *out_id = found;
    return 1;
}

int resolve_property_for_member_ir(const IR *ir, size_t sep_tok, const char *receiver,
                                          const char *member, int64_t *out_id) {
    int64_t fn_id, receiver_param;
    size_t i;
    Str type;
    if (!receiver || !*receiver || !member || !*member || sep_tok >= ir->tokens.n) return 0;
    fn_id = find_enclosing_callable_ir(ir, sep_tok);
    if (fn_id < 0) return 0;
    if (!strcmp(receiver, "this")) {
        int64_t cls = find_enclosing_type_owner(ir, fn_id);
        if (cls >= 0) {
            for (i = 0; i < ir->node_count; ++i) {
                const IRNode *p = &ir->nodes[i];
                if (p->kind != N_PROPERTY || !p->name || strcmp(p->name, member)) continue;
                if (property_owner_name(ir, (int64_t)i) &&
                    ir->nodes[cls].qualified && !strcmp(property_owner_name(ir, (int64_t)i), ir->nodes[cls].qualified)) {
                    if (out_id) *out_id = (int64_t)i;
                    return 1;
                }
            }
        }
        return 0;
    }
    receiver_param = callable_parameter_for_name(ir, &ir->nodes[fn_id], receiver);
    str_init(&type);
    if (receiver_param >= 0 && ir->nodes[receiver_param].type) {
        char *norm = normalize_type_tokens_ir(ir, ir->nodes[receiver_param].first_tok,
                                               ir->nodes[receiver_param].last_tok);
        /* Prefer the parameter's declared `type` field, because its token span
           also contains the parameter name. */
        free(norm);
        {
            size_t first = (size_t)ir->nodes[receiver_param].first_tok, last = (size_t)ir->nodes[receiver_param].last_tok;
            size_t ni = last;
            while (ni >= first && !(ir->tokens.v[ni].kind == TK_IDENTIFIER && ir->nodes[receiver_param].name &&
                                     !strcmp(ir->tokens.v[ni].text, ir->nodes[receiver_param].name))) {
                if (ni == first) break;
                --ni;
            }
            if (ni > first) {
                char *pnorm = normalize_type_tokens_ir(ir, first, ni - 1);
                if (pnorm && *pnorm) str_put(&type, pnorm);
                free(pnorm);
            }
        }
    } else {
        find_local_decl_type_ir(ir, &ir->nodes[fn_id], sep_tok, receiver, &type);
        /* A member access may use a field of the enclosing class as its
           receiver. Resolve the field's declared type before falling back to
           the global property-name search. */
        if (!type.len) {
            int64_t owner_cls = find_enclosing_type_owner(ir, fn_id);
            if (owner_cls >= 0) {
                int64_t fc;
                for (fc = ir->nodes[owner_cls].first_child; fc >= 0; fc = ir->nodes[fc].next_sibling) {
                    const IRNode *field = &ir->nodes[fc];
                    if (field->kind != N_FIELD_DECL || !field->name || strcmp(field->name, receiver) != 0) continue;
                    if (field->type && *field->type) {
                        char *fnorm = xstrdup0(field->type);
                        size_t k;
                        for (k = 0; fnorm[k]; ++k) {
                            if (fnorm[k] == '&' || fnorm[k] == '*') fnorm[k] = ' ';
                        }
                        {
                            size_t a = 0, z = strlen(fnorm);
                            while (a < z && isspace((unsigned char)fnorm[a])) a++;
                            while (z > a && isspace((unsigned char)fnorm[z - 1])) z--;
                            if (z > a) str_putn(&type, fnorm + a, z - a);
                        }
                        free(fnorm);
                    }
                    break;
                }
            }
        }
    }
    if (type.len) {
        char *bare = bare_type_name(type.data);
        for (i = 0; i < ir->node_count; ++i) {
            const IRNode *p = &ir->nodes[i];
            if (p->kind != N_PROPERTY || !p->name || strcmp(p->name, member)) continue;
            if (property_matches_type_context(ir, fn_id, bare, (int64_t)i)) {
                if (out_id) *out_id = (int64_t)i;
                free(bare);
                str_free(&type);
                return 1;
            }
        }
        /* The receiver has a known class type without such a property: it is
           a plain member access. */
        if (is_known_class_name(ir, bare)) { free(bare); str_free(&type); return 0; }
        free(bare);
    }
    str_free(&type);
    /* Receiver type unknown (v[i].x, f().x, auto&, smart pointers...): fall
       back to the member name when it designates exactly one property and
       no class declares a plain field with that name. */
    return unique_property_by_name(ir, member, out_id);
}
