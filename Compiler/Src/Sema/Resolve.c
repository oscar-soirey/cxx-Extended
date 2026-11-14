/*
 * Sema/Resolve.c
 *
 * Semantic passes: native feature resolution and named argument binding.
 */

#include "Core/Common.h"
#include "Sema/Resolve.h"
#include "CodeGen/CodeGenInternal.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"
#include "Ir/Ir.h"
#include "Lexer/Token.h"
#include "Parser/ParserInternal.h"

int get_member_pattern(const IR *ir, const IRNode *n, const char **member, size_t *member_tok, size_t *close_tok) {
    size_t a = (size_t)n->first_tok, b = (size_t)n->last_tok, i;
    if (a + 4 > b || b >= ir->tokens.n) return 0;
    for (i = a + 1; i + 3 <= b; ++i) {
        if ((strcmp(ir->tokens.v[i].text, ".") == 0 || strcmp(ir->tokens.v[i].text, "->") == 0) &&
            strcmp(ir->tokens.v[i + 1].text, "get_member") == 0 &&
            strcmp(ir->tokens.v[i + 2].text, "(") == 0) {
            int d = 0; size_t j;
            for (j = i + 2; j <= b; ++j) {
                if (strcmp(ir->tokens.v[j].text, "(") == 0) d++;
                else if (strcmp(ir->tokens.v[j].text, ")") == 0 && --d == 0) {
                    if (j == i + 3) return 0;
                    if (member && ir->tokens.v[i + 3].kind == TK_STRING) *member = ir->tokens.v[i + 3].text;
                    if (member_tok) *member_tok = i + 3;
                    if (close_tok) *close_tok = j;
                    return 1;
                }
            }
            return 0;
        }
    }
    return 0;
}

int find_custom_decorator_use(const IR *ir, const IRNode *use, int64_t *out_id) {
    size_t i, len, name_len, name_start, scope_len;
    const char *qualified = use->qualified && *use->qualified ? use->qualified : use->name;
    const char *name = use->name && *use->name ? use->name : qualified;
    char candidate[4096];
    if (!qualified || !*qualified || !name || !*name) return 0;
    len = strlen(qualified);
    name_len = strlen(name);
    if (len < name_len || strcmp(qualified + len - name_len, name) != 0) return 0;

    /* Resolve from the innermost lexical scope outward:
         A::B::decorator -> A::B::decorator -> A::decorator -> decorator. */
    name_start = len - name_len;
    scope_len = name_start;
    if (scope_len >= 2 && qualified[scope_len - 2] == ':' && qualified[scope_len - 1] == ':')
        scope_len -= 2;

    for (;;) {
        size_t pos = 0;
        if (scope_len) {
            if (scope_len + 2 + name_len >= sizeof(candidate)) return 0;
            memcpy(candidate, qualified, scope_len);
            candidate[scope_len] = ':';
            candidate[scope_len + 1] = ':';
            memcpy(candidate + scope_len + 2, name, name_len);
            candidate[scope_len + 2 + name_len] = 0;
        } else {
            snprintf(candidate, sizeof(candidate), "%s", name);
        }
        for (i = 0; i < ir->node_count; ++i) {
            const IRNode *cd = &ir->nodes[i];
            const char *cq = cd->qualified && *cd->qualified ? cd->qualified : cd->name;
            if (cd->kind == N_CUSTOM_DECORATOR && cq && strcmp(cq, candidate) == 0) {
                if (out_id) *out_id = (int64_t)i;
                return 1;
            }
        }
        if (!scope_len) break;
        pos = scope_len;
        while (pos >= 2 && !(qualified[pos - 2] == ':' && qualified[pos - 1] == ':')) --pos;
        if (pos < 2) scope_len = 0;
        else scope_len = pos - 2 + 0;
    }
    return 0;
}

void resolve_native_features(IR *ir) {
    size_t i;
    /* Decorators become semantic flags on their targets. */
    for (i = 0; i < ir->node_count; ++i) {
        IRNode *d = &ir->nodes[i];
        if (d->kind != N_DECORATOR || d->aux == UINT64_MAX || d->aux >= ir->node_count) continue;
        {
            IRNode *target = &ir->nodes[d->aux];
            if (d->flags & NF_DECORATOR_REGISTER) {
                if (!(target->kind == N_CLASS || target->kind == N_STRUCT || target->kind == N_UNION || target->kind == N_ENUM))
                    ir_die_at(ir, target->first_tok, "@register can only be applied to a class, struct, union, or enum");
                if (target->kind == N_ENUM && (!target->name || !*target->name))
                    ir_die_at(ir, target->first_tok, "@register requires a named enum");
                target->flags |= NF_REGISTERED;
            }
            if (d->flags & NF_DECORATOR_EXPOSED) {
                if (!(target->kind == N_FIELD_DECL || target->kind == N_METHOD))
                    ir_die_at(ir, target->first_tok, "@exposed can only be applied to a public member variable or method");
                if ((target->flags & NF_ACCESS_MASK) != NF_ACCESS_PUBLIC)
                    ir_die_at(ir, target->first_tok, "@exposed member '%s' must be public", target->name ? target->name : "<unnamed>");
                target->flags |= NF_EXPOSED;
            }
        }
    }

    /* Resolve custom decorators by their fully-qualified declaration name.
       A decorator used on a declaration is a real CXXE symbol reference: if
       it is neither a native decorator nor a declared custom decorator, that
       is a compile-time error. */
    for (i = 0; i < ir->node_count; ++i) {
        IRNode *d = &ir->nodes[i];
        int found = 0;
        const char *dkey;
        if (d->kind != N_DECORATOR || d->aux == UINT64_MAX) continue;
        if (d->flags & NF_DECORATOR_NATIVE) continue;
        dkey = d->qualified ? d->qualified : d->name;
        {
            int64_t cd_id = -1;
            if (find_custom_decorator_use(ir, d, &cd_id)) {
                IRNode *cd = &ir->nodes[cd_id];
                const char *ckey = cd->qualified ? cd->qualified : cd->name;
                d->flags |= NF_DECORATOR_CUSTOM;
                node_set_resolved(ir, (int64_t)i, ckey ? ckey : dkey);
                found = 1;
            }
        }
        if (!found) {
            die("unknown decorator '%s'", dkey ? dkey : (d->name ? d->name : "<unnamed>"));
        }
    }


    /* Fixed custom decorator signatures must match the decorated function.
       The generic func(...) form deliberately disables this check and accepts
       any parameter list. Parameter names are not part of the signature; the
       parameter types are. */
    for (i = 0; i < ir->node_count; ++i) {
        IRNode *d = &ir->nodes[i];
        if (d->kind != N_DECORATOR || !(d->flags & NF_DECORATOR_CUSTOM) ||
            d->aux == UINT64_MAX || d->aux >= ir->node_count) continue;
        {
            IRNode *cd = NULL, *target = NULL, *fn = &ir->nodes[d->aux];
            size_t j, target_count = 0, fn_count = 0;
            int64_t c;
            for (j = 0; j < ir->node_count; ++j) {
                IRNode *candidate = &ir->nodes[j];
                const char *ckey = candidate->qualified ? candidate->qualified : candidate->name;
                const char *dkey = d->qualified ? d->qualified : d->name;
                if (candidate->kind == N_CUSTOM_DECORATOR && ckey && dkey && strcmp(ckey, dkey) == 0) {
                    cd = candidate;
                    break;
                }
            }
            if (!cd) continue;
            {
                size_t expected_t = custom_decorator_template_param_count(ir, cd);
                size_t actual_t = (size_t)decorator_template_arg_ranges(ir, d, NULL, NULL, 0);
                if (expected_t == 0 && actual_t != 0)
                    die("custom decorator '%s' is not templated but received %zu template argument(s)",
                        d->qualified ? d->qualified : (d->name ? d->name : "<unnamed>"), actual_t);
                if (expected_t != 0 && actual_t != expected_t)
                    die("custom decorator '%s' expects %zu template argument(s), got %zu",
                        d->qualified ? d->qualified : (d->name ? d->name : "<unnamed>"), expected_t, actual_t);
            }
            for (c = cd->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
                if (ir->nodes[c].kind == N_DECORATOR_TARGET) {
                    target = &ir->nodes[c];
                    if (target->flags & NF_DECORATOR_VARIADIC) break; /* generic: no signature constraint */
                    break;
                }
            }
            if (!target) continue;
            if (target->flags & NF_DECORATOR_VARIADIC) continue;
            for (c = target->first_child; c >= 0; c = ir->nodes[c].next_sibling)
                if (ir->nodes[c].kind == N_PARAM_DECL) target_count++;
            for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling)
                if (ir->nodes[c].kind == N_PARAM_DECL) fn_count++;
            if (target_count != fn_count)
                die("custom decorator '%s' expects %zu function parameter(s), but decorated function '%s' has %zu",
                    d->qualified ? d->qualified : (d->name ? d->name : "<unnamed>"),
                    target_count, fn->name ? fn->name : "<unnamed>", fn_count);
            {
                size_t ti = 0, fi = 0;
                char a_type[2048], b_type[2048];
                int64_t tc = target->first_child, fc = fn->first_child;
                while (tc >= 0 && fc >= 0) {
                    IRNode *tp = &ir->nodes[tc], *fp = &ir->nodes[fc];
                    if (tp->kind == N_PARAM_DECL && fp->kind == N_PARAM_DECL) {
                        const char *ta = tp->type ? tp->type : "";
                        const char *fb = fp->type ? fp->type : "";
                        size_t x, y = 0;
                        /* Compare whitespace-insensitively because the parser's
                           token formatting is not semantically significant. */
                        for (x = 0; ta[x] && y + 1 < sizeof(a_type); ++x)
                            if (!isspace((unsigned char)ta[x])) a_type[y++] = ta[x];
                        a_type[y] = 0;
                        y = 0;
                        for (x = 0; fb[x] && y + 1 < sizeof(b_type); ++x)
                            if (!isspace((unsigned char)fb[x])) b_type[y++] = fb[x];
                        b_type[y] = 0;
                        if (strcmp(a_type, b_type) != 0)
                            die("custom decorator '%s' expects parameter %zu of type '%s', but decorated function '%s' has type '%s'",
                                d->qualified ? d->qualified : (d->name ? d->name : "<unnamed>"),
                                ti + 1, ta[0] ? ta : "<unknown>",
                                fn->name ? fn->name : "<unnamed>", fb[0] ? fb : "<unknown>");
                        ti++;
                    }
                    if (tp->kind == N_PARAM_DECL) tc = tp->next_sibling;
                    else tc = ir->nodes[tc].next_sibling;
                    if (fp->kind == N_PARAM_DECL) fc = fp->next_sibling;
                    else fc = ir->nodes[fc].next_sibling;
                    fi++;
                }
            }
        }
    }

    /* Factory intrinsics are ALWAYS runtime operations.
       Even a string literal such as "A" must be looked up in the runtime
       registry. This is intentional: CXXE supports dynamic class names and
       the runtime registry is the single source of truth. */

    /* get_member is ALWAYS a runtime intrinsic.
       A literal such as get_member("health") must NOT be resolved to a
       concrete member during compilation; the runtime registry performs the
       lookup by string every time. */
    for (i = 0; i < ir->node_count; ++i) {
        IRNode *n = &ir->nodes[i];
        size_t mtok = SIZE_MAX, close = SIZE_MAX;
        const char *member_literal = NULL;
        if (n->kind != N_FUNC_CALL) continue;
        if (!get_member_pattern(ir, n, &member_literal, &mtok, &close)) continue;
        n->kind = N_GET_MEMBER;
        n->flags |= NF_GET_MEMBER_RUNTIME;
        /* Deliberately do not inspect the member string, receiver type, or
           registered class here. The IR records the intrinsic, while the
           runtime performs the actual name lookup. */
        (void)member_literal;
        (void)mtok; (void)close;
    }
}

int resolve_named_args(IR *ir) {
    size_t i;
    int changed = 0;
    for (i = 0; i < ir->node_count; ++i) {
        IRNode *call = &ir->nodes[i];
        size_t f;
        if (call->kind != N_FUNC_CALL) continue;
        for (f = 0; f < ir->node_count; ++f) {
            IRNode *fn = &ir->nodes[f];
            int matches = 0;
            int call_changed = 0;
            int64_t arg;
            if (!(fn->kind == N_FUNCTION || fn->kind == N_METHOD || fn->kind == N_CONSTRUCTOR || fn->kind == N_OPERATOR_FUNCTION)) continue;
            if (call->resolved_name && *call->resolved_name && fn->qualified && *fn->qualified)
                matches = strcmp(call->resolved_name, fn->qualified) == 0;
            else if (call->name && fn->name)
                matches = strcmp(call->name, fn->name) == 0;
            if (!matches) continue;
            for (arg = call->first_child; arg >= 0; arg = ir->nodes[arg].next_sibling) {
                IRNode *an = &ir->nodes[arg];
                size_t pi = 0;
                int64_t param;
                if (an->kind != N_NAMED_ARG) continue;
                param = fn->first_child;
                while (param >= 0) {
                    IRNode *pn = &ir->nodes[param];
                    if (pn->kind == N_PARAM_DECL) {
                        if (pn->name && an->name && strcmp(pn->name, an->name) == 0) {
                            an->aux = (uint64_t)pi;
                            if (fn->qualified && *fn->qualified) {
                                Str q; str_init(&q);
                                str_put(&q, fn->qualified); str_put(&q, "::"); str_put(&q, pn->name);
                                node_set_resolved(ir, arg, q.data ? q.data : "");
                                str_free(&q);
                            }
                            changed = 1;
                            call_changed = 1;
                            break;
                        }
                        pi++;
                    }
                    param = pn->next_sibling;
                }
            }
            if (call_changed) break;
        }
    }
    return changed;
}

int node_is_named_call(const IR *ir, int64_t id) {
    const IRNode *n = &ir->nodes[id];
    int64_t c;
    for (c = n->first_child; c >= 0; c = ir->nodes[c].next_sibling)
        if (ir->nodes[c].kind == N_NAMED_ARG) return 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Named arguments: overload/constructor lookup and argument planning         */
/* ------------------------------------------------------------------------- */

static int param_is_variadic(const IR *ir, int64_t param);

static int is_callable_decl_kind(NodeKind k) {
    return k == N_FUNCTION || k == N_METHOD || k == N_CONSTRUCTOR || k == N_OPERATOR_FUNCTION;
}

static const char *leaf_name(const char *q) {
    const char *leaf = q;
    size_t i;
    if (!q) return NULL;
    for (i = 0; q[i]; ++i) if (q[i] == ':' && q[i + 1] == ':') leaf = q + i + 2;
    return leaf;
}

/* `a::b::c` ends with `b::c` on a `::` boundary. */
static int qualified_suffix(const char *full, const char *suffix) {
    size_t fl, sl;
    if (!full || !suffix) return 0;
    fl = strlen(full); sl = strlen(suffix);
    if (sl > fl || strcmp(full + fl - sl, suffix) != 0) return 0;
    return sl == fl || (fl - sl >= 2 && full[fl - sl - 1] == ':' && full[fl - sl - 2] == ':');
}

/* `Type name(args)`: a direct-initialisation whose callee token is really
   the variable name. Returns the class name when constructors of it exist. */
static const char *constructor_class_for_call(const IR *ir, const IRNode *call) {
    size_t i;
    const char *type;
    if (call->first_tok == 0 || call->first_tok >= ir->tokens.n) return NULL;
    if (ir->tokens.v[call->first_tok - 1].kind != TK_IDENTIFIER) return NULL;
    type = ir->tokens.v[call->first_tok - 1].text;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *fn = &ir->nodes[i];
        if (fn->kind == N_CONSTRUCTOR && fn->name && !strcmp(fn->name, type)) return type;
    }
    return NULL;
}

static int callee_matches(const IRNode *call, const IRNode *fn, const char *ctor_class) {
    if (!is_callable_decl_kind(fn->kind) || !fn->name) return 0;
    if (ctor_class) return fn->kind == N_CONSTRUCTOR && !strcmp(fn->name, ctor_class);
    if (!call->name || strcmp(call->name, fn->name) != 0) return 0;
    /* A qualified call (ns::f, Type::create) must agree with the declaration. */
    if (call->qualified && strstr(call->qualified, "::") && fn->qualified && strstr(fn->qualified, "::"))
        return qualified_suffix(fn->qualified, call->qualified) ||
               (call->resolved_name && qualified_suffix(fn->qualified, call->resolved_name));
    return 1;
}

static size_t collect_params(const IR *ir, const IRNode *fn, int64_t **out, int *variadic) {
    size_t n = 0;
    int64_t c, *v = NULL;
    *variadic = 0;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *pn = &ir->nodes[c];
        if (pn->kind != N_PARAM_DECL) continue;
        if (param_is_variadic(ir, c)) *variadic = 1;
        v = (int64_t *)xrealloc(v, (n + 1) * sizeof(*v));
        v[n++] = c;
    }
    *out = v;
    return n;
}

int64_t function_param_id(const IR *ir, const IRNode *fn, size_t index) {
    size_t idx = 0;
    int64_t c;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind != N_PARAM_DECL) continue;
        if (idx++ == index) return c;
    }
    return -1;
}

/* Default value text of a parameter (the parser stores it in `value`), so
   it also works for declarations imported from headers. */
static char *param_own_default(const IR *ir, int64_t param) {
    const char *v = ir->nodes[param].value, *e;
    Str o;
    if (!(ir->nodes[param].flags & NF_PARAM_HAS_DEFAULT) || !v) return NULL;
    while (*v && isspace((unsigned char)*v)) v++;
    e = v + strlen(v);
    while (e > v && isspace((unsigned char)e[-1])) e--;
    if (e == v) return NULL;
    str_init(&o);
    str_putn(&o, v, (size_t)(e - v));
    return str_take(&o);
}

static int param_is_variadic(const IR *ir, int64_t param) {
    const IRNode *pn = &ir->nodes[param];
    if (pn->flags & NF_PARAM_HAS_DEFAULT) return 0;
    return (pn->value && strstr(pn->value, "...")) || (pn->type && strstr(pn->type, "..."));
}

/* Default value of parameter `index`, looked up on `fn` and on every other
   declaration of the same entity (defaults usually live on the declaration
   in the header, not on the definition). Caller frees. */
char *param_default_text(const IR *ir, const IRNode *fn, size_t index) {
    size_t i, pc = function_param_count(ir, fn);
    int64_t p = function_param_id(ir, fn, index);
    char *d = p >= 0 ? param_own_default(ir, p) : NULL;
    if (d) return d;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *other = &ir->nodes[i];
        if (other == fn || other->kind != fn->kind || !other->qualified || !fn->qualified) continue;
        if (strcmp(other->qualified, fn->qualified) != 0 || function_param_count(ir, other) != pc) continue;
        p = function_param_id(ir, other, index);
        if (p >= 0 && (d = param_own_default(ir, p)) != NULL) return d;
    }
    return NULL;
}

static int try_candidate(const IR *ir, const IRNode *call, const IRNode *fn, NamedCallPlan *plan, Str *why) {
    int64_t *params = NULL, c;
    int variadic = 0;
    size_t pc = collect_params(ir, fn, &params, &variadic), pos = 0, i;
    int64_t *slot = (int64_t *)xcalloc(pc ? pc : 1, sizeof(*slot));
    const char *fname = fn->qualified ? fn->qualified : fn->name;
    for (i = 0; i < pc; ++i) slot[i] = -1;
    for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *arg = &ir->nodes[c];
        if (arg->kind == N_NAMED_ARG) {
            size_t pi = SIZE_MAX;
            for (i = 0; i < pc; ++i)
                if (ir->nodes[params[i]].name && arg->name && !strcmp(ir->nodes[params[i]].name, arg->name)) { pi = i; break; }
            if (pi == SIZE_MAX) {
                if (why) { why->len = 0; str_put(why, "'"); str_put(why, fname); str_put(why, "' has no parameter named '"); str_put(why, arg->name ? arg->name : "?"); str_put(why, "'"); }
                goto fail;
            }
            if (slot[pi] >= 0) {
                if (why) { why->len = 0; str_put(why, "parameter '"); str_put(why, arg->name); str_put(why, "' of '"); str_put(why, fname); str_put(why, "' is given more than once"); }
                goto fail;
            }
            slot[pi] = c;
        } else if (arg->kind == N_CALL_ARG) {
            while (pos < pc && slot[pos] >= 0) pos++;
            if (pos >= pc || variadic) {
                if (why) { why->len = 0; str_put(why, variadic ? "named arguments cannot be combined with a variadic parameter list in a call to '" : "too many arguments in call to '"); str_put(why, fname); str_put(why, "'"); }
                goto fail;
            }
            slot[pos++] = c;
        }
    }
    {
        size_t last_used = 0;
        int any = 0;
        for (i = 0; i < pc; ++i) if (slot[i] >= 0) { last_used = i; any = 1; }
        for (i = 0; i < pc; ++i) {
            if (slot[i] >= 0) continue;
            char *d = param_default_text(ir, fn, i);
            if (d) { free(d); continue; }
            {
                if (why) { why->len = 0; str_put(why, "missing argument for parameter '"); str_put(why, ir->nodes[params[i]].name ? ir->nodes[params[i]].name : "?"); str_put(why, "' of '"); str_put(why, fname); str_put(why, "'"); }
                goto fail;
            }
        }
        plan->fn = (int64_t)(fn - ir->nodes);
        plan->param_count = any ? last_used + 1 : 0;
        plan->slot = slot;
        free(params);
        return 1;
    }
fail:
    free(slot);
    free(params);
    return 0;
}

int plan_named_call(const IR *ir, const IRNode *call, NamedCallPlan *plan, Str *why) {
    size_t i, candidates = 0;
    const char *ctor_class = NULL;
    Str first_why;
    memset(plan, 0, sizeof(*plan));
    plan->fn = -1;
    /* `Type var(args)` first; otherwise the callee name. */
    ctor_class = constructor_class_for_call(ir, call);
    str_init(&first_why);
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *fn = &ir->nodes[i];
        if (!callee_matches(call, fn, ctor_class)) continue;
        candidates++;
        if (try_candidate(ir, call, fn, plan, candidates == 1 ? &first_why : NULL)) { str_free(&first_why); return 1; }
    }
    if (!candidates) { str_free(&first_why); return 0; }
    if (why) {
        why->len = 0;
        if (candidates > 1) { str_put(why, "no overload of '"); str_put(why, leaf_name(call->name)); str_put(why, "' accepts these arguments (first candidate: "); }
        str_put(why, first_why.data ? first_why.data : "invalid named arguments");
        if (candidates > 1) str_put(why, ")");
    }
    str_free(&first_why);
    return -1;
}

void named_call_plan_free(NamedCallPlan *plan) {
    free(plan->slot);
    memset(plan, 0, sizeof(*plan));
    plan->fn = -1;
}

int find_matching_function(const IR *ir, const IRNode *call, int64_t *out) {
    NamedCallPlan plan;
    int r = plan_named_call(ir, call, &plan, NULL);
    if (r == 1 && out) *out = plan.fn;
    named_call_plan_free(&plan);
    return r == 1;
}

void check_named_calls(IR *ir) {
    size_t i;
    Str why;
    str_init(&why);
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *call = &ir->nodes[i];
        NamedCallPlan plan;
        int r;
        if (call->kind != N_FUNC_CALL || (call->flags & NF_EXTERNAL_DECL) || !node_is_named_call(ir, (int64_t)i)) continue;
        r = plan_named_call(ir, call, &plan, &why);
        named_call_plan_free(&plan);
        if (r < 0) ir_die_at(ir, call->first_tok, "%s", why.data ? why.data : "invalid named arguments");
        if (r == 0) ir_warn_at(ir, call->first_tok, "no declaration of '%s' is visible: '%s(name=value)' is kept as a C++ assignment expression",
                               call->name ? call->name : "?", call->name ? call->name : "?");
    }
    str_free(&why);
}

int64_t function_param_by_name(const IR *ir, const IRNode *fn, const char *name, size_t *index_out) {
    size_t idx = 0;
    int64_t p;
    for (p = fn->first_child; p >= 0; p = ir->nodes[p].next_sibling) {
        const IRNode *param = &ir->nodes[p];
        if (param->kind != N_PARAM_DECL) continue;
        if (param->name && name && strcmp(param->name, name) == 0) {
            if (index_out) *index_out = idx;
            return p;
        }
        idx++;
    }
    return -1;
}

size_t function_param_count(const IR *ir, const IRNode *fn) {
    size_t n = 0; int64_t p;
    for (p = fn->first_child; p >= 0; p = ir->nodes[p].next_sibling)
        if (ir->nodes[p].kind == N_PARAM_DECL) n++;
    return n;
}
