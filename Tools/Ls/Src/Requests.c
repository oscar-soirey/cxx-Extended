/*
 * Requests.c
 *
 * Hover, navigation, rename, completion, signature help, symbols, semantic
 * tokens, include links and the generated C++ view.
 */

#include "Requests.h"
#include "Symbols.h"
#include "Core/Memory.h"
#include "Frontend/IncludeResolver.h"
#include "Lexer/Token.h"
#include "Parser/ParserInternal.h"
#include "Sema/Resolve.h"

#define TK(ir, i) ((i) < (ir)->tokens.n ? (ir)->tokens.v[(i)].text : "")
#define TKIS(ir, i, s) (strcmp(TK(ir, i), (s)) == 0)

static size_t cursor_offset(const LsDocument *d, const Json *params) {
    return linemap_offset(&d->unit.lines, json_int(params, "position.line", 0), json_int(params, "position.character", 0));
}

static size_t cursor_token(const LsDocument *d, const Json *params) {
    return unit_token_at(&d->unit, cursor_offset(d, params));
}

/* Token whose bytes contain `offset` (no preference for the previous word). */
static size_t token_containing(const LsUnit *u, size_t offset) {
    const TokenVec *tv = &u->ir.tokens;
    size_t lo = 0, hi = tv->n;
    if (!tv->n) return SIZE_MAX;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (tv->v[mid].byte_start <= offset) lo = mid; else hi = mid;
    }
    if (offset >= tv->v[lo].byte_start && offset < tv->v[lo].byte_end && tv->v[lo].kind != TK_EOF) return lo;
    return SIZE_MAX;
}

static int is_ident_tok(const IR *ir, size_t i) {
    return i < ir->tokens.n && ir->tokens.v[i].kind == TK_IDENTIFIER;
}

static const char *kind_label(NodeKind k) {
    switch (k) {
    case N_CLASS: return "class";
    case N_STRUCT: return "struct";
    case N_UNION: return "union";
    case N_ENUM: return "enum";
    case N_ENUMERATOR: return "enumerator";
    case N_NAMESPACE: return "namespace";
    case N_FUNCTION: return "function";
    case N_METHOD: return "method";
    case N_CONSTRUCTOR: return "constructor";
    case N_OPERATOR_FUNCTION: return "operator";
    case N_FIELD_DECL: return "field";
    case N_PROPERTY: return "property";
    case N_VAR_DECL: return "variable";
    case N_PARAM_DECL: return "parameter";
    case N_TYPEDEF: case N_USING_DECL: return "type alias";
    case N_CUSTOM_DECORATOR: return "decorator";
    case N_CONCEPT: return "concept";
    default: return "symbol";
    }
}

/* ------------------------------------------------------------------------- */
/* Documentation of CXXE built-ins                                            */
/* ------------------------------------------------------------------------- */

static const char *builtin_doc(const char *word) {
    if (!strcmp(word, "register"))
        return "**@register** (native decorator)\n\nRegisters a class, struct, union or enum in the CXXE runtime: "
               "`stde::factory_new(\"Name\")`, `stde::factory_find(\"Name\")`, `stde::enum_to_string(...)`.";
    if (!strcmp(word, "exposed"))
        return "**@exposed** (native decorator)\n\nExposes a public field or method of a `@register` class to "
               "runtime reflection: `obj->get_member(\"name\")`.";
    if (!strcmp(word, "property"))
        return "**property** name { get { ... } set { ... } }\n\nNative accessors for the field with the same name. "
               "`obj.name` calls the getter, `obj.name = v` the setter (generated as `get_name()` / `set_name()`).";
    if (!strcmp(word, "get")) return "**get** block of a property: returns the value read by `obj.name`.";
    if (!strcmp(word, "set")) return "**set** block of a property: `value` holds the assigned value (same type as the field).";
    if (!strcmp(word, "value")) return "**value**: the value being assigned, inside a property `set` block.";
    if (!strcmp(word, "match"))
        return "**match** value { Type: statement; ... default: statement; }\n\nType-based dispatch on a `dynamic` value; "
               "inside a case the value is already cast to that type.";
    if (!strcmp(word, "dynamic")) return "**dynamic**: value whose type is only known at runtime (`is<T>()`, `as<T>()`, `match`).";
    if (!strcmp(word, "func")) return "**func**: the decorated function, inside a custom decorator body.";
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Hover / definition                                                         */
/* ------------------------------------------------------------------------- */

static void put_markdown(Str *out, const char *md, const LsUnit *u, size_t tok) {
    size_t a, b;
    str_put(out, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    json_put_string(out, md);
    str_put(out, "}");
    if (tok != SIZE_MAX) {
        unit_token_range(u, tok, &a, &b);
        str_put(out, ",\"range\":");
        unit_put_range(out, u, a, b);
    }
    str_ch(out, '}');
}

static int in_setter(const IR *ir, size_t tok) {
    size_t i;
    for (i = 0; i < ir->node_count; ++i)
        if (ir->nodes[i].kind == N_PROPERTY_SET && ir->nodes[i].first_tok <= tok && tok <= ir->nodes[i].last_tok) return 1;
    return 0;
}

void feature_hover(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsUnit *u = &d->unit;
    const IR *ir = &u->ir;
    size_t tok = cursor_token(d, params);
    LsSymbol sym;
    Str md;
    if (tok == SIZE_MAX || !is_ident_tok(ir, tok)) { str_put(out, "null"); return; }
    if (!strcmp(TK(ir, tok), "value") && in_setter(ir, tok)) { put_markdown(out, builtin_doc("value"), u, tok); return; }
    if (!sym_resolve(s, u, tok, &sym)) {
        const char *doc = builtin_doc(TK(ir, tok));
        if (doc && strcmp(TK(ir, tok), "register") && strcmp(TK(ir, tok), "exposed")) put_markdown(out, doc, u, tok);
        else str_put(out, "null");
        return;
    }
    str_init(&md);
    if (sym.kind == SYM_NATIVE_DECORATOR) {
        str_put(&md, builtin_doc(sym.name));
    } else if (sym.kind == SYM_LOCAL) {
        str_put(&md, "```cpp\n");
        str_put(&md, sym.type);
        str_ch(&md, ' ');
        str_put(&md, sym.name);
        str_put(&md, "\n```\nlocal variable");
    } else {
        const IRNode *n = &sym.unit->ir.nodes[sym.node];
        char *text = decl_text(sym.unit, sym.node);
        int named_label = tok + 1 < ir->tokens.n && TKIS(ir, tok + 1, "=") && n->kind == N_PARAM_DECL && sym.decl_tok != tok;
        str_put(&md, "```cpp\n");
        if (n->kind == N_CUSTOM_DECORATOR && text[0] != '@' && !strstr(text, "@")) str_ch(&md, '@');
        str_put(&md, text);
        str_put(&md, "\n```\n");
        free(text);
        if (named_label && n->parent >= 0) {
            char *fn = decl_text(sym.unit, n->parent);
            str_put(&md, "named argument for parameter of `");
            str_put(&md, fn);
            str_put(&md, "`");
            free(fn);
        } else {
            str_put(&md, kind_label(n->kind));
            if (n->qualified && strstr(n->qualified, "::")) { str_put(&md, " in `"); str_putn(&md, n->qualified, (size_t)(strrchr(n->qualified, ':') - 1 - n->qualified)); str_put(&md, "`"); }
        }
        if (n->kind == N_PROPERTY) {
            str_put(&md, "\n\n`obj.");
            str_put(&md, n->name);
            str_put(&md, "` reads through `get`, assignments go through `set`.");
        }
        if (n->kind == N_CUSTOM_DECORATOR) str_put(&md, "\n\nUse as `@name` before a function or method.");
        sym_locate(s, &sym);
        if (sym.decl_tok != SIZE_MAX && sym.unit != u) {
            char line[64];
            snprintf(line, sizeof(line), ":%u", sym.unit->ir.tokens.v[sym.decl_tok].line);
            str_put(&md, "\n\n*");
            str_put(&md, path_basename(sym.unit->path));
            str_put(&md, line);
            str_put(&md, "*");
        }
    }
    put_markdown(out, md.data ? md.data : "", u, tok);
    str_free(&md);
}

static char *include_spelled(const char *directive, size_t *a_out, size_t *b_out) {
    const char *a = strchr(directive, '"'), *b;
    char close = '"';
    if (!a) { a = strchr(directive, '<'); close = '>'; }
    if (!a) return NULL;
    b = strchr(a + 1, close);
    if (!b) return NULL;
    *a_out = (size_t)(a + 1 - directive);
    *b_out = (size_t)(b - directive);
    return xstrndup0(a + 1, (size_t)(b - a - 1));
}

static int include_target(LsServer *s, LsUnit *u, size_t tok, char **path) {
    const Token *t = &u->ir.tokens.v[tok];
    size_t n;
    const char *const *dirs;
    if (t->kind != TK_PP || !strstr(t->text, "include")) return 0;
    dirs = server_include_dirs(s, &n);
    *path = cxxe_resolve_include(u->path, t->text, dirs, n);
    return *path != NULL;
}

void feature_definition(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsUnit *u = &d->unit;
    size_t tok = cursor_token(d, params), a, b;
    LsSymbol sym;
    char *inc = NULL;
    if (tok == SIZE_MAX) { str_put(out, "null"); return; }
    if (include_target(s, u, tok, &inc)) {
        char *uri = path_to_uri(inc);
        str_put(out, "{\"uri\":");
        json_put_string(out, uri);
        str_put(out, ",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":0}}}");
        free(uri);
        free(inc);
        return;
    }
    if (!sym_resolve(s, u, tok, &sym) || sym.kind == SYM_NATIVE_DECORATOR || !sym_locate(s, &sym)) { str_put(out, "null"); return; }
    unit_token_range(sym.unit, sym.decl_tok, &a, &b);
    unit_put_location(out, sym.unit, a, b);
}

/* ------------------------------------------------------------------------- */
/* References / highlight / rename                                            */
/* ------------------------------------------------------------------------- */

static size_t occurrences_at(LsServer *s, LsDocument *d, const Json *params, LsOcc **occ, LsSymbol *sym) {
    size_t tok = cursor_token(d, params);
    *occ = NULL;
    if (tok == SIZE_MAX || !sym_resolve(s, &d->unit, tok, sym)) return 0;
    index_refresh(s);
    return sym_occurrences(s, sym, occ);
}

void feature_references(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsOcc *occ;
    LsSymbol sym;
    int with_decl = json_bool(params, "context.includeDeclaration", 1);
    size_t n = occurrences_at(s, d, params, &occ, &sym), i, a, b;
    int first = 1;
    str_ch(out, '[');
    for (i = 0; i < n; ++i) {
        if (occ[i].is_decl && !with_decl) continue;
        if (!first) str_ch(out, ',');
        first = 0;
        unit_token_range(occ[i].unit, occ[i].tok, &a, &b);
        unit_put_location(out, occ[i].unit, a, b);
    }
    str_ch(out, ']');
    free(occ);
}

void feature_highlight(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsOcc *occ = NULL;
    LsSymbol sym;
    size_t tok = cursor_token(d, params), n = 0, i, a, b;
    int first = 1;
    if (tok != SIZE_MAX && sym_resolve(s, &d->unit, tok, &sym)) n = sym_occurrences(s, &sym, &occ);
    str_ch(out, '[');
    for (i = 0; i < n; ++i) {
        if (occ[i].unit != &d->unit) continue;
        if (!first) str_ch(out, ',');
        first = 0;
        unit_token_range(occ[i].unit, occ[i].tok, &a, &b);
        str_put(out, "{\"range\":");
        unit_put_range(out, occ[i].unit, a, b);
        str_put(out, occ[i].is_decl ? ",\"kind\":3}" : ",\"kind\":2}");
    }
    str_ch(out, ']');
    free(occ);
}

void feature_prepare_rename(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    size_t tok = cursor_token(d, params), a, b;
    LsSymbol sym;
    if (tok == SIZE_MAX || !sym_resolve(s, &d->unit, tok, &sym) || sym.kind == SYM_NATIVE_DECORATOR) { str_put(out, "null"); return; }
    unit_token_range(&d->unit, tok, &a, &b);
    str_put(out, "{\"range\":");
    unit_put_range(out, &d->unit, a, b);
    str_put(out, ",\"placeholder\":");
    json_put_string(out, sym.name);
    str_ch(out, '}');
}

static int valid_identifier(const char *s) {
    if (!s || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (; *s; ++s) if (!(isalnum((unsigned char)*s) || *s == '_')) return 0;
    return 1;
}

int feature_rename(LsServer *s, LsDocument *d, const Json *params, Str *out, const char **error) {
    const char *new_name = json_str(params, "newName");
    LsOcc *occ;
    LsSymbol sym;
    size_t n, i, j;
    if (!valid_identifier(new_name) || is_keyword(new_name)) { *error = "the new name is not a valid identifier"; return 0; }
    n = occurrences_at(s, d, params, &occ, &sym);
    if (!n) { *error = "nothing to rename here"; return 0; }
    str_put(out, "{\"changes\":{");
    for (i = 0; i < n; ++i) {
        int seen = 0, first = 1;
        size_t a, b;
        for (j = 0; j < i; ++j) if (occ[j].unit == occ[i].unit) seen = 1;
        if (seen) continue;
        if (out->data[out->len - 1] != '{') str_ch(out, ',');
        json_put_string(out, unit_uri(occ[i].unit));
        str_put(out, ":[");
        for (j = i; j < n; ++j) {
            if (occ[j].unit != occ[i].unit) continue;
            if (!first) str_ch(out, ',');
            first = 0;
            unit_token_range(occ[j].unit, occ[j].tok, &a, &b);
            str_put(out, "{\"range\":");
            unit_put_range(out, occ[j].unit, a, b);
            str_put(out, ",\"newText\":");
            json_put_string(out, new_name);
            str_ch(out, '}');
        }
        str_ch(out, ']');
    }
    str_put(out, "}}");
    free(occ);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Completion                                                                 */
/* ------------------------------------------------------------------------- */

enum { CK_METHOD = 2, CK_FUNCTION = 3, CK_CONSTRUCTOR = 4, CK_FIELD = 5, CK_VARIABLE = 6, CK_CLASS = 7,
       CK_MODULE = 9, CK_PROPERTY = 10, CK_ENUM = 13, CK_KEYWORD = 14, CK_ENUM_MEMBER = 20, CK_STRUCT = 22,
       CK_TYPE_PARAMETER = 25, CK_EVENT = 23 };

typedef struct {
    Str *out;
    char **labels;
    size_t n;
    LsServer *s;
} Items;

static int item_seen(Items *it, const char *label) {
    size_t i;
    for (i = 0; i < it->n; ++i) if (!strcmp(it->labels[i], label)) return 1;
    it->labels = (char **)xrealloc(it->labels, (it->n + 1) * sizeof(char *));
    it->labels[it->n++] = xstrdup0(label);
    return 0;
}

static void item(Items *it, const char *label, int kind, const char *detail, const char *doc, const char *insert, const char *sort) {
    char num[16];
    if (!label || !*label || item_seen(it, label)) return;
    if (it->out->data[it->out->len - 1] != '[') str_ch(it->out, ',');
    str_put(it->out, "{\"label\":");
    json_put_string(it->out, label);
    snprintf(num, sizeof(num), "%d", kind);
    str_put(it->out, ",\"kind\":");
    str_put(it->out, num);
    if (detail && *detail) { str_put(it->out, ",\"detail\":"); json_put_string(it->out, detail); }
    if (doc && *doc) { str_put(it->out, ",\"documentation\":{\"kind\":\"markdown\",\"value\":"); json_put_string(it->out, doc); str_ch(it->out, '}'); }
    if (insert) { str_put(it->out, ",\"insertText\":"); json_put_string(it->out, insert); }
    if (sort) { str_put(it->out, ",\"sortText\":"); json_put_string(it->out, sort); }
    str_ch(it->out, '}');
}

static int completion_kind(NodeKind k) {
    switch (k) {
    case N_CLASS: return CK_CLASS;
    case N_STRUCT: case N_UNION: return CK_STRUCT;
    case N_ENUM: return CK_ENUM;
    case N_ENUMERATOR: return CK_ENUM_MEMBER;
    case N_NAMESPACE: return CK_MODULE;
    case N_FUNCTION: return CK_FUNCTION;
    case N_METHOD: case N_OPERATOR_FUNCTION: return CK_METHOD;
    case N_CONSTRUCTOR: return CK_CONSTRUCTOR;
    case N_FIELD_DECL: return CK_FIELD;
    case N_PROPERTY: return CK_PROPERTY;
    case N_PARAM_DECL: case N_VAR_DECL: return CK_VARIABLE;
    case N_TYPEDEF: case N_USING_DECL: case N_CONCEPT: return CK_TYPE_PARAMETER;
    case N_CUSTOM_DECORATOR: return CK_EVENT;
    default: return CK_VARIABLE;
    }
}

static void node_item(Items *it, LsUnit *u, int64_t node, const char *sort) {
    const IRNode *n = &u->ir.nodes[node];
    char *detail;
    if (!n->name || !*n->name) return;
    if (n->kind == N_CONSTRUCTOR) return;
    /* A property and its backing field share the name: show the property. */
    if (n->kind == N_FIELD_DECL && n->parent >= 0) {
        int64_t c;
        for (c = u->ir.nodes[n->parent].first_child; c >= 0; c = u->ir.nodes[c].next_sibling)
            if (u->ir.nodes[c].kind == N_PROPERTY && u->ir.nodes[c].name && !strcmp(u->ir.nodes[c].name, n->name)) return;
    }
    detail = decl_text(u, node);
    item(it, n->name, completion_kind(n->kind), detail, NULL, NULL, sort);
    free(detail);
}

static void member_item_cb(void *ctx, LsUnit *u, int64_t m) { node_item((Items *)ctx, u, m, "1"); }

typedef struct { Items *it; const LsUnit *u; } LocalItems;

static void local_item_cb(void *ctx, size_t tok) {
    LocalItems *li = (LocalItems *)ctx;
    char type[256];
    local_type(li->u, tok, type, sizeof(type));
    item(li->it, TK(&li->u->ir, tok), CK_VARIABLE, type, NULL, NULL, "0");
}

static int enum_is_scoped(const LsUnit *u, int64_t e) {
    const IRNode *n = &u->ir.nodes[e];
    if (n->value && (strstr(n->value, "enum class") || strstr(n->value, "enum struct"))) return 1;
    if (!(n->flags & NF_EXTERNAL_DECL) && n->first_tok + 1 < u->ir.tokens.n)
        return TKIS(&u->ir, n->first_tok + 1, "class") || TKIS(&u->ir, n->first_tok + 1, "struct");
    return 0;
}

static void global_items(Items *it, LsUnit *u) {
    size_t i;
    for (i = 0; i < u->ir.node_count; ++i) {
        const IRNode *n = &u->ir.nodes[i];
        int64_t p = n->parent;
        int top = 1;
        for (; p >= 0; p = u->ir.nodes[p].parent) {
            NodeKind k = u->ir.nodes[p].kind;
            if (k == N_CLASS || k == N_STRUCT || k == N_UNION || is_callable_kind_ls(k) || k == N_CUSTOM_DECORATOR) { top = 0; break; }
            if (k == N_ENUM && (n->kind != N_ENUMERATOR || enum_is_scoped(u, p))) { top = 0; break; }
        }
        if (!top || n->kind == N_PARAM_DECL || n->kind == N_CUSTOM_DECORATOR) continue;
        if (!is_declaration_kind(n->kind)) continue;
        node_item(it, u, (int64_t)i, "2");
    }
}

static const char *const CPP_KEYWORDS[] = {
    "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr", "continue", "default", "delete",
    "do", "double", "else", "enum", "explicit", "false", "float", "for", "friend", "if", "inline", "int", "long",
    "namespace", "new", "noexcept", "nullptr", "operator", "override", "private", "protected", "public", "return",
    "short", "signed", "sizeof", "static", "static_cast", "struct", "switch", "template", "this", "throw", "true",
    "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "while", "const_cast",
    "dynamic_cast", "reinterpret_cast", "decltype", "concept", "requires", "co_await", "co_return", "co_yield", NULL };

static void keyword_items(Items *it, const IR *ir, size_t tok) {
    size_t i;
    for (i = 0; CPP_KEYWORDS[i]; ++i) item(it, CPP_KEYWORDS[i], CK_KEYWORD, NULL, NULL, NULL, "4");
    item(it, "property", CK_KEYWORD, "CXXE", builtin_doc("property"), NULL, "3");
    item(it, "match", CK_KEYWORD, "CXXE", builtin_doc("match"), NULL, "3");
    item(it, "dynamic", CK_KEYWORD, "CXXE", builtin_doc("dynamic"), NULL, "3");
    if (tok != SIZE_MAX && in_setter(ir, tok)) item(it, "value", CK_VARIABLE, "assigned value", builtin_doc("value"), NULL, "0");
}

/* Callee of the argument list the cursor is in, and the argument index. */
static int call_context(const IR *ir, size_t tok, size_t *callee_tok, size_t *arg_index, const char **named) {
    size_t i = tok, commas = 0;
    int depth = 0;
    *named = NULL;
    while (i > 0) {
        const char *t = TK(ir, i - 1);
        if (!strcmp(t, ")") || !strcmp(t, "]") || !strcmp(t, "}")) depth++;
        else if (!strcmp(t, "(") || !strcmp(t, "[") || !strcmp(t, "{")) {
            if (depth == 0) {
                if (strcmp(t, "(") || i < 2 || !is_ident_tok(ir, i - 2)) return 0;
                *callee_tok = i - 2;
                *arg_index = commas;
                return 1;
            }
            depth--;
        } else if (!strcmp(t, ";")) return 0;
        else if (depth == 0 && !strcmp(t, ",")) commas++;
        else if (depth == 0 && !strcmp(t, "=") && !*named && is_ident_tok(ir, i - 2) &&
                 (TKIS(ir, i - 3, "(") || TKIS(ir, i - 3, ","))) *named = TK(ir, i - 2);
        i--;
    }
    return 0;
}

typedef struct { LsUnit *unit; int64_t node; } Callable;

/* Functions/constructors a call at callee_tok may target. */
static size_t call_candidates(LsServer *s, LsUnit *u, size_t callee_tok, Callable **out) {
    const IR *ir = &u->ir;
    const char *name = TK(ir, callee_tok);
    const char *type = callee_tok > 0 && is_ident_tok(ir, callee_tok - 1) ? TK(ir, callee_tok - 1) : NULL;
    LsUnit **units;
    size_t nu, ui, i, n = 0;
    Callable *v = NULL;
    int ctor_pass;
    nu = server_units(s, &units);
    for (ctor_pass = 0; ctor_pass < 2 && !n; ++ctor_pass) {
        for (ui = 0; ui < nu; ++ui) {
            const IR *uir = &units[ui]->ir;
            if (ui > 0 && units[ui] == u) continue;
            for (i = 0; i < uir->node_count; ++i) {
                const IRNode *f = &uir->nodes[i];
                if (!is_callable_kind_ls(f->kind) || !f->name) continue;
                if (f->return_type && strchr(f->return_type, '~')) continue;
                if (ctor_pass == 0 && strcmp(f->name, name)) continue;
                if (ctor_pass == 1 && (f->kind != N_CONSTRUCTOR || !type || strcmp(f->name, type))) continue;
                /* skip an imported copy when the unit itself is in the list */
                v = (Callable *)xrealloc(v, (n + 1) * sizeof(Callable));
                v[n].unit = units[ui];
                v[n].node = (int64_t)i;
                n++;
            }
        }
    }
    free(units);
    /* One entry per entity (header declaration, definition and imported copy
       describe the same function): same scope, name and parameter count. */
    {
        size_t a, b, m = 0;
        for (a = 0; a < n; ++a) {
            const IRNode *fa = &v[a].unit->ir.nodes[v[a].node];
            int dup = 0;
            for (b = 0; b < m && !dup; ++b) {
                const IRNode *fb = &v[b].unit->ir.nodes[v[b].node];
                dup = fa->kind == fb->kind && function_param_count(&v[a].unit->ir, fa) == function_param_count(&v[b].unit->ir, fb) &&
                      !strcmp(fa->qualified ? fa->qualified : fa->name, fb->qualified ? fb->qualified : fb->name);
            }
            if (!dup) v[m++] = v[a];
        }
        n = m;
    }
    *out = v;
    return n;
}

static void named_argument_items(Items *it, LsServer *s, LsUnit *u, size_t callee_tok) {
    Callable *c;
    size_t n = call_candidates(s, u, callee_tok, &c), i;
    const IR *ir = &u->ir;
    for (i = 0; i < n; ++i) {
        const IR *fir = &c[i].unit->ir;
        int64_t p;
        for (p = fir->nodes[c[i].node].first_child; p >= 0; p = fir->nodes[p].next_sibling) {
            const IRNode *pn = &fir->nodes[p];
            char label[300], *detail;
            size_t t;
            int used = 0;
            if (pn->kind != N_PARAM_DECL || !pn->name || !*pn->name) continue;
            /* skip parameters already passed by name in this call */
            for (t = callee_tok + 2; t + 1 < ir->tokens.n && !TKIS(ir, t, ")") && !TKIS(ir, t, ";"); ++t)
                if (TKIS(ir, t, pn->name) && TKIS(ir, t + 1, "=") && (TKIS(ir, t - 1, "(") || TKIS(ir, t - 1, ","))) used = 1;
            if (used) continue;
            snprintf(label, sizeof(label), "%s=", pn->name);
            detail = param_text(c[i].unit, p);
            item(it, label, CK_FIELD, detail, "named argument", label, "00");
            free(detail);
        }
    }
    free(c);
}

static void decorator_items(Items *it, LsUnit *u) {
    size_t i;
    item(it, "register", CK_EVENT, "native decorator", builtin_doc("register"), NULL, "0");
    item(it, "exposed", CK_EVENT, "native decorator", builtin_doc("exposed"), NULL, "0");
    for (i = 0; i < u->ir.node_count; ++i) {
        const IRNode *n = &u->ir.nodes[i];
        char *detail;
        if (n->kind != N_CUSTOM_DECORATOR || !n->name) continue;
        detail = decl_text(u, (int64_t)i);
        item(it, n->qualified && *n->qualified ? n->qualified : n->name, CK_EVENT, detail, "custom decorator", NULL, "1");
        free(detail);
    }
}

static void stde_items(Items *it) {
    static const char *const fns[][2] = {
        { "factory_new", "Base* factory_new(const std::string& name) — new instance of a @register class" },
        { "factory_find", "factory_find(name) — the registered type, use as new stde::factory_find(\"A\")()" },
        { "enum_to_string", "std::string enum_to_string(E value)" },
        { "string_to_enum", "E string_to_enum<E>(const std::string& name)" },
        { "enum_names", "std::vector<std::string> enum_names<E>()" },
        { "enum_has_value", "bool enum_has_value(E value)" },
        { "enum_has_name", "bool enum_has_name<E>(const std::string& name)" },
        { "get_enum_info", "get_enum_info<E>()" },
        { NULL, NULL } };
    size_t i;
    for (i = 0; fns[i][0]; ++i) item(it, fns[i][0], CK_FUNCTION, fns[i][1], NULL, NULL, "0");
}

void feature_completion(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsUnit *u = &d->unit;
    const IR *ir = &u->ir;
    const char *text = (const char *)ir->source;
    size_t off = cursor_offset(d, params), start = off, tok;
    Items it;
    memset(&it, 0, sizeof(it));
    it.out = out;
    it.s = s;
    str_put(out, "{\"isIncomplete\":false,\"items\":[");
    while (start > 0 && (isalnum((unsigned char)text[start - 1]) || text[start - 1] == '_')) start--;
    tok = SIZE_MAX;
    {
        size_t k = start;
        while (k > 0 && isspace((unsigned char)text[k - 1])) k--;
        if (k > 0) tok = token_containing(u, k - 1);
    }

    if (start > 0 && text[start - 1] == '@') {
        decorator_items(&it, u);
    } else if (tok != SIZE_MAX && (TKIS(ir, tok, ".") || TKIS(ir, tok, "->"))) {
        LsUnit *cu = NULL;
        int64_t cls;
        if (receiver_class(s, u, tok, &cu, &cls)) class_members(s, cu, cls, member_item_cb, &it);
    } else if (tok != SIZE_MAX && TKIS(ir, tok, "::") && tok > 0 && is_ident_tok(ir, tok - 1)) {
        const char *qual = TK(ir, tok - 1);
        size_t i;
        if (!strcmp(qual, "stde")) stde_items(&it);
        for (i = 0; i < ir->node_count; ++i) {
            const IRNode *n = &ir->nodes[i];
            const IRNode *p = n->parent >= 0 ? &ir->nodes[n->parent] : NULL;
            const char *q = n->qualified;
            size_t ql = strlen(qual);
            int direct = 0;
            if (!is_declaration_kind(n->kind) || n->kind == N_PARAM_DECL || !n->name) continue;
            if (p && p->name && !strcmp(p->name, qual) &&
                (p->kind == N_ENUM || p->kind == N_NAMESPACE || p->kind == N_CLASS || p->kind == N_STRUCT || p->kind == N_UNION)) direct = 1;
            /* imported declarations keep their scope only in the qualified name */
            if (!direct && q) {
                const char *hit = q;
                while ((hit = strstr(hit, qual)) != NULL) {
                    if ((hit == q || (hit >= q + 2 && hit[-1] == ':' && hit[-2] == ':')) && !strncmp(hit + ql, "::", 2) &&
                        !strcmp(hit + ql + 2, n->name)) { direct = 1; break; }
                    hit++;
                }
            }
            if (direct) node_item(&it, u, (int64_t)i, "1");
        }
        {
            /* templates wrap declarations: look one level deeper for namespaces */
            size_t j;
            for (j = 0; j < ir->node_count; ++j) {
                const IRNode *n = &ir->nodes[j];
                if (n->kind != N_TEMPLATE || n->parent < 0 || !ir->nodes[n->parent].name || strcmp(ir->nodes[n->parent].name, qual)) continue;
                {
                    int64_t c;
                    for (c = n->first_child; c >= 0; c = ir->nodes[c].next_sibling) node_item(&it, u, c, "1");
                }
            }
        }
    } else {
        size_t here = unit_token_at(u, off ? off - 1 : 0), callee, argi;
        const char *named = NULL;
        size_t anchor = start > 0 ? start - 1 : 0;
        int64_t fn;
        size_t before = here != SIZE_MAX ? here : (tok != SIZE_MAX ? tok : 0);
        /* name= labels at the start of an argument */
        {
            size_t k = start;
            while (k > 0 && isspace((unsigned char)text[k - 1])) k--;
            if (k > 0 && (text[k - 1] == '(' || text[k - 1] == ',')) {
                size_t ctok = token_containing(u, k - 1);
                if (ctok != SIZE_MAX && call_context(ir, ctok + 1, &callee, &argi, &named)) named_argument_items(&it, s, u, callee);
            }
        }
        (void)anchor;
        fn = enclosing_callable(ir, before);
        if (fn >= 0) {
            LocalItems li;
            int64_t c, cls;
            li.it = &it;
            li.u = u;
            for (c = ir->nodes[fn].first_child; c >= 0; c = ir->nodes[c].next_sibling)
                if (ir->nodes[c].kind == N_PARAM_DECL) node_item(&it, u, c, "0");
            list_locals(u, fn, before, local_item_cb, &li);
            cls = callable_owner_class(ir, fn);
            if (cls >= 0) class_members(s, u, cls, member_item_cb, &it);
        }
        global_items(&it, u);
        keyword_items(&it, ir, before);
    }
    str_put(out, "]}");
    {
        size_t i;
        for (i = 0; i < it.n; ++i) free(it.labels[i]);
        free(it.labels);
    }
}

/* ------------------------------------------------------------------------- */
/* Signature help                                                             */
/* ------------------------------------------------------------------------- */

void feature_signature_help(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsUnit *u = &d->unit;
    const IR *ir = &u->ir;
    size_t off = cursor_offset(d, params), tok, callee, argi, n, i;
    const char *named = NULL;
    Callable *c;
    char num[32];
    /* first token at or after the cursor */
    tok = unit_token_at(u, off);
    if (tok == SIZE_MAX || ir->tokens.v[tok].byte_start < off) {
        size_t t = tok == SIZE_MAX ? 0 : tok;
        while (t < ir->tokens.n && ir->tokens.v[t].byte_start < off) t++;
        tok = t;
    }
    if (!call_context(ir, tok, &callee, &argi, &named)) { str_put(out, "null"); return; }
    n = call_candidates(s, u, callee, &c);
    if (!n) { free(c); str_put(out, "null"); return; }
    str_put(out, "{\"signatures\":[");
    for (i = 0; i < n; ++i) {
        const IR *fir = &c[i].unit->ir;
        char *label = decl_text(c[i].unit, c[i].node);
        int64_t p;
        size_t pi = 0;
        long active = (long)argi;
        int firstp = 1;
        if (i) str_ch(out, ',');
        str_put(out, "{\"label\":");
        json_put_string(out, label);
        str_put(out, ",\"parameters\":[");
        for (p = fir->nodes[c[i].node].first_child; p >= 0; p = fir->nodes[p].next_sibling) {
            char *pt;
            if (fir->nodes[p].kind != N_PARAM_DECL) continue;
            if (named && fir->nodes[p].name && !strcmp(fir->nodes[p].name, named)) active = (long)pi;
            pt = param_text(c[i].unit, p);
            if (!firstp) str_ch(out, ',');
            firstp = 0;
            str_put(out, "{\"label\":");
            json_put_string(out, pt);
            str_ch(out, '}');
            free(pt);
            pi++;
        }
        snprintf(num, sizeof(num), "%ld", active);
        str_put(out, "],\"activeParameter\":");
        str_put(out, num);
        str_ch(out, '}');
        free(label);
    }
    str_put(out, "],\"activeSignature\":0}");
    free(c);
}

/* ------------------------------------------------------------------------- */
/* Symbols                                                                    */
/* ------------------------------------------------------------------------- */

static int lsp_symbol_kind(NodeKind k) {
    switch (k) {
    case N_NAMESPACE: return 3;
    case N_CLASS: return 5;
    case N_METHOD: case N_OPERATOR_FUNCTION: return 6;
    case N_PROPERTY: return 7;
    case N_FIELD_DECL: return 8;
    case N_CONSTRUCTOR: return 9;
    case N_ENUM: return 10;
    case N_FUNCTION: case N_CUSTOM_DECORATOR: return 12;
    case N_VAR_DECL: return 13;
    case N_ENUMERATOR: return 22;
    case N_STRUCT: case N_UNION: return 23;
    case N_TYPEDEF: case N_USING_DECL: return 26;
    default: return 13;
    }
}

static int outline_kind(NodeKind k) {
    switch (k) {
    case N_NAMESPACE: case N_CLASS: case N_STRUCT: case N_UNION: case N_ENUM: case N_ENUMERATOR:
    case N_FUNCTION: case N_METHOD: case N_CONSTRUCTOR: case N_OPERATOR_FUNCTION: case N_FIELD_DECL:
    case N_PROPERTY: case N_CUSTOM_DECORATOR: case N_TYPEDEF: case N_USING_DECL: case N_VAR_DECL:
        return 1;
    default:
        return 0;
    }
}

static void outline_children(LsUnit *u, int64_t parent, Str *out, int *first);

static void outline_node(LsUnit *u, int64_t id, Str *out, int *first) {
    const IR *ir = &u->ir;
    const IRNode *n = &ir->nodes[id];
    size_t a, b, sa, sb, nt;
    char num[16];
    char *detail;
    int cfirst = 1;
    if (n->kind == N_TEMPLATE || n->kind == N_LINKAGE_SPEC || n->kind == N_EXPORT) { outline_children(u, id, out, first); return; }
    if (!outline_kind(n->kind) || !n->name || !*n->name || (n->flags & NF_EXTERNAL_DECL) || n->first_tok >= ir->tokens.n) return;
    if (n->kind == N_VAR_DECL && n->parent >= 0 && is_callable_kind_ls(ir->nodes[n->parent].kind)) return;
    if (n->kind == N_FIELD_DECL && n->parent >= 0) {
        int64_t c;
        for (c = ir->nodes[n->parent].first_child; c >= 0; c = ir->nodes[c].next_sibling)
            if (ir->nodes[c].kind == N_PROPERTY && ir->nodes[c].name && !strcmp(ir->nodes[c].name, n->name)) return;
    }
    a = (size_t)ir->tokens.v[n->first_tok].byte_start;
    b = (size_t)ir->tokens.v[n->last_tok < ir->tokens.n ? n->last_tok : ir->tokens.n - 1].byte_end;
    nt = decl_name_token(ir, id);
    if (nt != SIZE_MAX) unit_token_range(u, nt, &sa, &sb); else { sa = a; sb = a; }
    if (!*first) str_ch(out, ',');
    *first = 0;
    str_put(out, "{\"name\":");
    if (n->kind == N_CUSTOM_DECORATOR) { Str nm; str_init(&nm); str_ch(&nm, '@'); str_put(&nm, n->name); json_put_string(out, nm.data); str_free(&nm); }
    else if (n->kind == N_CONSTRUCTOR && n->return_type && strchr(n->return_type, '~')) { Str nm; str_init(&nm); str_ch(&nm, '~'); str_put(&nm, n->name); json_put_string(out, nm.data); str_free(&nm); }
    else json_put_string(out, n->name);
    detail = (n->kind == N_FUNCTION || n->kind == N_METHOD || n->kind == N_FIELD_DECL) ? decl_text(u, id) : NULL;
    if (detail) { str_put(out, ",\"detail\":"); json_put_string(out, detail); free(detail); }
    snprintf(num, sizeof(num), "%d", lsp_symbol_kind(n->kind));
    str_put(out, ",\"kind\":");
    str_put(out, num);
    str_put(out, ",\"range\":");
    unit_put_range(out, u, a, b);
    str_put(out, ",\"selectionRange\":");
    unit_put_range(out, u, sa, sb);
    str_put(out, ",\"children\":[");
    if (n->kind != N_FUNCTION && n->kind != N_METHOD && n->kind != N_CONSTRUCTOR && n->kind != N_PROPERTY)
        outline_children(u, id, out, &cfirst);
    str_put(out, "]}");
}

static void outline_children(LsUnit *u, int64_t parent, Str *out, int *first) {
    int64_t c;
    for (c = u->ir.nodes[parent].first_child; c >= 0; c = u->ir.nodes[c].next_sibling) outline_node(u, c, out, first);
}

void feature_document_symbols(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    int first = 1;
    (void)s; (void)params;
    str_ch(out, '[');
    if (d->unit.ir.node_count) outline_children(&d->unit, 0, out, &first);
    str_ch(out, ']');
}

static int contains_ci(const char *hay, const char *needle) {
    size_t i, j;
    if (!needle || !*needle) return 1;
    for (i = 0; hay[i]; ++i) {
        for (j = 0; needle[j] && hay[i + j] && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]); ++j) {}
        if (!needle[j]) return 1;
    }
    return 0;
}

void feature_workspace_symbols(LsServer *s, const Json *params, Str *out) {
    const char *query = json_str(params, "query");
    LsUnit **units;
    size_t n, ui, i, count = 0;
    int first = 1;
    index_refresh(s);
    n = server_units(s, &units);
    str_ch(out, '[');
    for (ui = 0; ui < n && count < 500; ++ui) {
        LsUnit *u = units[ui];
        const IR *ir = &u->ir;
        for (i = 0; i < ir->node_count && count < 500; ++i) {
            const IRNode *nd = &ir->nodes[i];
            size_t nt, a, b;
            char num[16];
            if (!outline_kind(nd->kind) || nd->kind == N_VAR_DECL || nd->kind == N_ENUMERATOR || !nd->name || (nd->flags & NF_EXTERNAL_DECL)) continue;
            if (!contains_ci(nd->name, query)) continue;
            nt = decl_name_token(ir, (int64_t)i);
            if (nt == SIZE_MAX) continue;
            unit_token_range(u, nt, &a, &b);
            if (!first) str_ch(out, ',');
            first = 0;
            count++;
            str_put(out, "{\"name\":");
            json_put_string(out, nd->name);
            snprintf(num, sizeof(num), "%d", lsp_symbol_kind(nd->kind));
            str_put(out, ",\"kind\":");
            str_put(out, num);
            if (nd->qualified && strstr(nd->qualified, "::")) {
                char cont[256];
                size_t cl = (size_t)(strrchr(nd->qualified, ':') - 1 - nd->qualified);
                if (cl >= sizeof(cont)) cl = sizeof(cont) - 1;
                memcpy(cont, nd->qualified, cl);
                cont[cl] = 0;
                str_put(out, ",\"containerName\":");
                json_put_string(out, cont);
            }
            str_put(out, ",\"location\":");
            unit_put_location(out, u, a, b);
            str_ch(out, '}');
        }
    }
    str_ch(out, ']');
    free(units);
}

/* ------------------------------------------------------------------------- */
/* Semantic tokens                                                            */
/* ------------------------------------------------------------------------- */

const char *const SEMANTIC_TOKEN_TYPES[] = { "namespace", "class", "enum", "struct", "type", "parameter", "variable",
    "property", "enumMember", "function", "method", "keyword", "decorator", NULL };
const char *const SEMANTIC_TOKEN_MODIFIERS[] = { "declaration", NULL };

enum { ST_NAMESPACE, ST_CLASS, ST_ENUM, ST_STRUCT, ST_TYPE, ST_PARAMETER, ST_VARIABLE, ST_PROPERTY,
       ST_ENUM_MEMBER, ST_FUNCTION, ST_METHOD, ST_KEYWORD, ST_DECORATOR };

static int semantic_type_of(NodeKind k) {
    switch (k) {
    case N_NAMESPACE: return ST_NAMESPACE;
    case N_CLASS: return ST_CLASS;
    case N_STRUCT: case N_UNION: return ST_STRUCT;
    case N_ENUM: return ST_ENUM;
    case N_TYPEDEF: case N_USING_DECL: case N_CONCEPT: return ST_TYPE;
    case N_ENUMERATOR: return ST_ENUM_MEMBER;
    case N_FUNCTION: return ST_FUNCTION;
    case N_METHOD: case N_OPERATOR_FUNCTION: return ST_METHOD;
    case N_FIELD_DECL: case N_PROPERTY: return ST_PROPERTY;
    case N_CUSTOM_DECORATOR: return ST_DECORATOR;
    case N_PARAM_DECL: return ST_PARAMETER;
    case N_VAR_DECL: return ST_VARIABLE;
    default: return -1;
    }
}

typedef struct { int *type; int *mod; const IR *ir; } TokClass;

typedef struct { const char *name; int64_t node; } NameEntry;

static int name_entry_cmp(const void *a, const void *b) {
    return strcmp(((const NameEntry *)a)->name, ((const NameEntry *)b)->name);
}

/* First entry named `name` in the sorted table (or n). */
static size_t name_lookup(const NameEntry *v, size_t n, const char *name) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (strcmp(v[mid].name, name) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void mark_local_cb(void *ctx, size_t tok) {
    TokClass *tc = (TokClass *)ctx;
    tc->type[tok] = ST_VARIABLE;
    tc->mod[tok] = 1;
}

void feature_semantic_tokens(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsUnit *u = &d->unit;
    const IR *ir = &u->ir;
    size_t nt = ir->tokens.n, i;
    int *type = (int *)xmalloc((nt + 1) * sizeof(int));
    int *mod = (int *)xcalloc(nt + 1, sizeof(int));
    int64_t *fn_of = (int64_t *)xmalloc((nt + 1) * sizeof(int64_t));
    long last_line = 0, last_char = 0;
    int first = 1;
    TokClass tc;
    NameEntry *names = (NameEntry *)xmalloc((ir->node_count + 1) * sizeof(NameEntry));
    size_t name_n = 0;
    (void)s; (void)params;
    for (i = 0; i < ir->node_count; ++i)
        if (ir->nodes[i].name && *ir->nodes[i].name) { names[name_n].name = ir->nodes[i].name; names[name_n].node = (int64_t)i; name_n++; }
    qsort(names, name_n, sizeof(NameEntry), name_entry_cmp);
    for (i = 0; i < nt; ++i) { type[i] = -1; fn_of[i] = -1; }
    tc.type = type; tc.mod = mod; tc.ir = ir;
    /* innermost callable of every token */
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        size_t t;
        if (!is_callable_kind_ls(n->kind) || (n->flags & NF_EXTERNAL_DECL) || n->first_tok >= nt) continue;
        for (t = (size_t)n->first_tok; t <= (size_t)n->last_tok && t < nt; ++t)
            if (fn_of[t] < 0 || ir->nodes[fn_of[t]].first_tok <= n->first_tok) fn_of[t] = (int64_t)i;
    }
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!is_callable_kind_ls(n->kind) || (n->flags & NF_EXTERNAL_DECL) || n->first_tok >= nt) continue;
        list_locals(u, (int64_t)i, (size_t)n->last_tok, mark_local_cb, &tc);
    }
    /* declarations, CXXE keywords, named-argument labels */
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        size_t t;
        int st = semantic_type_of(n->kind);
        if ((n->flags & NF_EXTERNAL_DECL) || n->first_tok >= nt) continue;
        if (st >= 0 && (t = decl_name_token(ir, (int64_t)i)) != SIZE_MAX) {
            if (n->kind == N_CONSTRUCTOR) st = ST_CLASS;
            type[t] = st;
            mod[t] = 1;
        }
        if (n->kind == N_PROPERTY || n->kind == N_PROPERTY_GET || n->kind == N_PROPERTY_SET || n->kind == N_MATCH_STMT)
            type[n->first_tok] = ST_KEYWORD;
        if (n->kind == N_NAMED_ARG) type[n->first_tok] = ST_PARAMETER;
        if (n->kind == N_PROPERTY_SET) {
            for (t = (size_t)n->first_tok + 1; t <= (size_t)n->last_tok && t < nt; ++t)
                if (TKIS(ir, t, "value") && !TKIS(ir, t - 1, ".") && !TKIS(ir, t - 1, "->")) type[t] = ST_PARAMETER;
        }
    }
    /* uses */
    for (i = 0; i < nt; ++i) {
        const char *t = TK(ir, i);
        if (!is_ident_tok(ir, i) || type[i] >= 0) continue;
        if (i > 0 && TKIS(ir, i - 1, "@")) { type[i] = ST_DECORATOR; type[i - 1] = ST_DECORATOR; continue; }
        if (!strcmp(t, "dynamic")) { type[i] = ST_KEYWORD; continue; }
        if (is_keyword(t)) continue;
        if (i > 0 && (TKIS(ir, i - 1, ".") || TKIS(ir, i - 1, "->"))) {
            size_t k;
            for (k = name_lookup(names, name_n, t); k < name_n && !strcmp(names[k].name, t); ++k) {
                const IRNode *n = &ir->nodes[names[k].node];
                if (n->kind == N_PROPERTY || n->kind == N_FIELD_DECL) { type[i] = ST_PROPERTY; break; }
                if (n->kind == N_METHOD) { type[i] = ST_METHOD; break; }
            }
            if (type[i] < 0 && TKIS(ir, i + 1, "(")) type[i] = ST_METHOD;
            continue;
        }
        if (fn_of[i] >= 0) {
            int64_t c;
            for (c = ir->nodes[fn_of[i]].first_child; c >= 0; c = ir->nodes[c].next_sibling)
                if (ir->nodes[c].kind == N_PARAM_DECL && ir->nodes[c].name && !strcmp(ir->nodes[c].name, t)) { type[i] = ST_PARAMETER; break; }
            if (type[i] >= 0) continue;
            {
                size_t k;
                int64_t f = fn_of[i];
                /* a local declared earlier in the same function */
                for (k = (size_t)ir->nodes[f].first_tok; k < i; ++k)
                    if (type[k] == ST_VARIABLE && mod[k] && !strcmp(TK(ir, k), t)) { type[i] = ST_VARIABLE; break; }
                if (type[i] >= 0) continue;
            }
        }
        {
            size_t k;
            for (k = name_lookup(names, name_n, t); k < name_n && !strcmp(names[k].name, t); ++k) {
                const IRNode *n = &ir->nodes[names[k].node];
                int st;
                if (n->kind == N_PARAM_DECL || n->kind == N_CONSTRUCTOR) continue;
                st = semantic_type_of(n->kind);
                if (st < 0 || st == ST_VARIABLE) continue;
                if ((st == ST_PROPERTY || st == ST_METHOD) && fn_of[i] < 0) continue;
                type[i] = st;
                break;
            }
        }
    }
    str_put(out, "{\"data\":[");
    for (i = 0; i < nt; ++i) {
        long line, ch, eline, ech;
        char b[96];
        if (type[i] < 0) continue;
        linemap_position(&u->lines, (size_t)ir->tokens.v[i].byte_start, &line, &ch);
        linemap_position(&u->lines, (size_t)ir->tokens.v[i].byte_end, &eline, &ech);
        if (eline != line) continue;
        snprintf(b, sizeof(b), "%s%ld,%ld,%ld,%d,%d", first ? "" : ",", line - last_line,
                 line == last_line ? ch - last_char : ch, ech - ch, type[i], mod[i] ? 1 : 0);
        str_put(out, b);
        first = 0;
        last_line = line;
        last_char = ch;
    }
    str_put(out, "]}");
    free(type);
    free(mod);
    free(fn_of);
    free(names);
}

/* ------------------------------------------------------------------------- */
/* Include links and generated C++                                            */
/* ------------------------------------------------------------------------- */

void feature_document_links(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    LsUnit *u = &d->unit;
    size_t i, n;
    int first = 1;
    const char *const *dirs = server_include_dirs(s, &n);
    (void)params;
    str_ch(out, '[');
    for (i = 0; i < u->ir.tokens.n; ++i) {
        const Token *t = &u->ir.tokens.v[i];
        size_t a, b;
        char *spell, *resolved, *uri;
        if (t->kind != TK_PP || !strstr(t->text, "include")) continue;
        spell = include_spelled(t->text, &a, &b);
        if (!spell) continue;
        resolved = cxxe_resolve_include(u->path, t->text, dirs, n);
        free(spell);
        if (!resolved) continue;
        uri = path_to_uri(resolved);
        if (!first) str_ch(out, ',');
        first = 0;
        str_put(out, "{\"range\":");
        unit_put_range(out, u, (size_t)t->byte_start + a, (size_t)t->byte_start + b);
        str_put(out, ",\"target\":");
        json_put_string(out, uri);
        str_ch(out, '}');
        free(uri);
        free(resolved);
    }
    str_ch(out, ']');
}

void feature_generated_cpp(LsServer *s, LsDocument *d, const Json *params, Str *out) {
    (void)s; (void)params;
    str_put(out, "{\"text\":");
    if (d->generated) json_put_string(out, d->generated);
    else {
        Str t;
        size_t i;
        str_init(&t);
        str_put(&t, "// The CXXE analysis of this file failed, no C++ was generated.\n");
        for (i = 0; i < d->diags.n; ++i) { str_put(&t, "// "); str_put(&t, d->diags.v[i].message); str_ch(&t, '\n'); }
        json_put_string(out, t.data);
        str_free(&t);
    }
    str_put(out, ",\"ok\":");
    str_put(out, d->generated ? "true" : "false");
    str_ch(out, '}');
}
