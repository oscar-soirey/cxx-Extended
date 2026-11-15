/*
 * Symbols.c
 *
 * Name resolution for the language server. The compiler IR has declaration
 * nodes for types, functions, members and parameters; local variables are
 * recovered from the tokens of function bodies.
 */

#include "Symbols.h"
#include "Core/Memory.h"
#include "Lexer/Token.h"
#include "Parser/ParserInternal.h"
#include "Sema/Resolve.h"

#define TK(ir, i) ((i) < (ir)->tokens.n ? (ir)->tokens.v[(i)].text : "")
#define TKIS(ir, i, s) (strcmp(TK(ir, i), (s)) == 0)

static int is_word(const IR *ir, size_t i) {
    return i < ir->tokens.n && ir->tokens.v[i].kind == TK_IDENTIFIER;
}

static int is_name(const IR *ir, size_t i) {
    return is_word(ir, i) && !is_keyword(TK(ir, i));
}

static int is_external(const IRNode *n) { return (n->flags & NF_EXTERNAL_DECL) != 0; }

int is_callable_kind_ls(NodeKind k) {
    return k == N_FUNCTION || k == N_METHOD || k == N_CONSTRUCTOR || k == N_OPERATOR_FUNCTION;
}

int is_declaration_kind(NodeKind k) {
    switch (k) {
    case N_CLASS: case N_STRUCT: case N_UNION: case N_ENUM: case N_ENUMERATOR: case N_NAMESPACE:
    case N_FUNCTION: case N_METHOD: case N_CONSTRUCTOR: case N_OPERATOR_FUNCTION:
    case N_FIELD_DECL: case N_PROPERTY: case N_VAR_DECL: case N_PARAM_DECL: case N_TYPEDEF:
    case N_USING_DECL: case N_CUSTOM_DECORATOR: case N_CONCEPT:
        return 1;
    default:
        return 0;
    }
}

static int is_type_kind(NodeKind k) { return k == N_CLASS || k == N_STRUCT || k == N_UNION; }

static int is_member_kind(NodeKind k) {
    return k == N_FIELD_DECL || k == N_PROPERTY || k == N_METHOD || k == N_CONSTRUCTOR || k == N_OPERATOR_FUNCTION;
}

static const char *leaf(const char *q) {
    const char *l = q, *p;
    if (!q) return "";
    for (p = q; *p; ++p) if (p[0] == ':' && p[1] == ':') l = p + 2;
    return l;
}

static void copy_trim(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!src) { dst[0] = 0; return; }
    while (*src && isspace((unsigned char)*src)) src++;
    n = strlen(src);
    while (n && isspace((unsigned char)src[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

size_t decl_name_token(const IR *ir, int64_t node) {
    const IRNode *n = &ir->nodes[node];
    size_t i, a = (size_t)n->first_tok, b = (size_t)n->last_tok;
    if (!n->name || !*n->name || is_external(n) || a >= ir->tokens.n) return SIZE_MAX;
    if (b >= ir->tokens.n) b = ir->tokens.n - 1;
    if (n->kind == N_CUSTOM_DECORATOR) {
        for (i = a; i < b; ++i) if (TKIS(ir, i, "@") && TKIS(ir, i + 1, n->name)) return i + 1;
        return SIZE_MAX;
    }
    if (is_callable_kind_ls(n->kind)) {
        int dtor = n->return_type && strchr(n->return_type, '~');
        for (i = a; i <= b; ++i) {
            if (!TKIS(ir, i, n->name) || !TKIS(ir, i + 1, "(")) continue;
            if (n->kind == N_CONSTRUCTOR && (i > 0 && TKIS(ir, i - 1, "~")) != dtor) continue;
            return i;
        }
        return SIZE_MAX;
    }
    for (i = a; i <= b; ++i) {
        if (!TKIS(ir, i, n->name) || !is_word(ir, i)) continue;
        if (i > 0 && (TKIS(ir, i - 1, ".") || TKIS(ir, i - 1, "->"))) continue;
        return i;
    }
    return SIZE_MAX;
}

int64_t enclosing_callable(const IR *ir, size_t tok) {
    size_t i;
    int64_t best = -1;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!is_callable_kind_ls(n->kind) || is_external(n) || n->first_tok > tok || n->last_tok < tok) continue;
        if (best < 0 || n->last_tok - n->first_tok < ir->nodes[best].last_tok - ir->nodes[best].first_tok) best = (int64_t)i;
    }
    return best;
}

static int64_t enclosing_class_body(const IR *ir, size_t tok) {
    size_t i;
    int64_t best = -1;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!is_type_kind(n->kind) || is_external(n) || n->first_tok > tok || n->last_tok < tok) continue;
        if (best < 0 || n->last_tok - n->first_tok < ir->nodes[best].last_tok - ir->nodes[best].first_tok) best = (int64_t)i;
    }
    return best;
}

int64_t find_class_named(const IR *ir, const char *name) {
    size_t i;
    int64_t ext = -1;
    const char *l = leaf(name);
    if (!*l) return -1;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!is_type_kind(n->kind) || !n->name || strcmp(n->name, l)) continue;
        if (!is_external(n)) return (int64_t)i;
        if (ext < 0) ext = (int64_t)i;
    }
    return ext;
}

int64_t callable_owner_class(const IR *ir, int64_t fn) {
    int64_t p;
    const char *q;
    if (fn < 0) return -1;
    for (p = ir->nodes[fn].parent; p >= 0; p = ir->nodes[p].parent)
        if (is_type_kind(ir->nodes[p].kind)) return p;
    {
        /* Out-of-line definition: `Type::name(` -> the class is the token
           before `::` (the IR's qualified name omits it). */
        size_t nt = decl_name_token(ir, fn);
        if (nt != SIZE_MAX && nt >= 2 && TKIS(ir, nt - 1, "::") && is_word(ir, nt - 2)) {
            int64_t c = find_class_named(ir, TK(ir, nt - 2));
            if (c >= 0) return c;
        }
    }
    q = ir->nodes[fn].qualified;
    if (q && strstr(q, "::")) {
        char owner[256];
        const char *last = q, *s;
        for (s = q; *s; ++s) if (s[0] == ':' && s[1] == ':') last = s;
        if ((size_t)(last - q) < sizeof(owner)) {
            memcpy(owner, q, (size_t)(last - q));
            owner[last - q] = 0;
            return find_class_named(ir, owner);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Locals                                                                     */
/* ------------------------------------------------------------------------- */

static int type_keyword(const char *s) {
    static const char *const kw[] = { "int", "char", "bool", "float", "double", "long", "short", "unsigned",
        "signed", "auto", "void", "const", "volatile", "static", "constexpr", "inline", "mutable", "thread_local",
        "struct", "class", "enum", "typename", "wchar_t", "char8_t", "char16_t", "char32_t", "dynamic", NULL };
    size_t i;
    for (i = 0; kw[i]; ++i) if (!strcmp(s, kw[i])) return 1;
    return 0;
}

static int boundary_token(const char *s) {
    return !strcmp(s, "{") || !strcmp(s, "}") || !strcmp(s, ";") || !strcmp(s, "(") || !strcmp(s, ",") || !strcmp(s, ":");
}

/* Walks back over the type in front of name_tok; returns its first token or
   SIZE_MAX when the tokens before the name are not a declaration type. */
static size_t type_start(const IR *ir, size_t name_tok, size_t floor) {
    size_t i = name_tok, types = 0;
    while (i > floor) {
        const char *t = TK(ir, i - 1);
        if (boundary_token(t)) break;
        if (!strcmp(t, ">") || !strcmp(t, ">>")) {
            int depth = !strcmp(t, ">") ? 1 : 2;
            i--;
            while (i > floor && depth) {
                const char *u = TK(ir, i - 1);
                if (!strcmp(u, ">")) depth++;
                else if (!strcmp(u, ">>")) depth += 2;
                else if (!strcmp(u, "<")) depth--;
                else if (!strcmp(u, ";") || !strcmp(u, "{") || !strcmp(u, "}")) return SIZE_MAX;
                i--;
            }
            if (depth) return SIZE_MAX;
            types++;
            continue;
        }
        if (!strcmp(t, "*") || !strcmp(t, "&") || !strcmp(t, "&&") || !strcmp(t, "::") || !strcmp(t, "...")) { i--; continue; }
        if (is_word(ir, i - 1) && (!is_keyword(t) || type_keyword(t))) { i--; types++; continue; }
        return SIZE_MAX;
    }
    return types ? i : SIZE_MAX;
}

static int is_local_decl(const IR *ir, size_t i, size_t floor) {
    const char *next = TK(ir, i + 1);
    if (!is_name(ir, i) || i == 0) return 0;
    if (!(!strcmp(next, "=") || !strcmp(next, ";") || !strcmp(next, "{") || !strcmp(next, "(") ||
          !strcmp(next, ",") || !strcmp(next, ":") || !strcmp(next, ")") || !strcmp(next, "["))) return 0;
    if (!strcmp(TK(ir, i - 1), ">") && !strcmp(next, ")")) return 0;
    return type_start(ir, i, floor) != SIZE_MAX;
}

void list_locals(const LsUnit *u, int64_t fn, size_t before, void (*cb)(void *ctx, size_t name_tok), void *ctx) {
    const IR *ir = &u->ir;
    size_t i, a, b;
    if (fn < 0) return;
    a = (size_t)ir->nodes[fn].first_tok;
    b = (size_t)ir->nodes[fn].last_tok;
    if (before < b) b = before;
    for (i = a; i <= b && i < ir->tokens.n; ++i) if (is_local_decl(ir, i, a)) cb(ctx, i);
}

void local_type(const LsUnit *u, size_t name_tok, char *out, size_t cap) {
    const IR *ir = &u->ir;
    size_t s = type_start(ir, name_tok, 0), i;
    Str t;
    out[0] = 0;
    if (s == SIZE_MAX) return;
    str_init(&t);
    for (i = s; i < name_tok; ++i) {
        const char *x = TK(ir, i);
        if (t.len && is_word(ir, i) && is_word(ir, i - 1)) str_ch(&t, ' ');
        str_put(&t, x);
    }
    copy_trim(out, cap, t.data ? t.data : "");
    str_free(&t);
}

/* ------------------------------------------------------------------------- */
/* Types of expressions                                                       */
/* ------------------------------------------------------------------------- */

/* "const std::unique_ptr<Node>&" + arrow -> "Node"; "Box<int>" -> "Box". */
static void bare_type(const char *type, int arrow, char *out, size_t cap) {
    char buf[512];
    char *p, *lt;
    copy_trim(buf, sizeof(buf), type);
    /* drop cv/ref/ptr decorations */
    for (;;) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '&' || buf[n - 1] == '*' || isspace((unsigned char)buf[n - 1]))) buf[--n] = 0;
        if (n > 6 && !strcmp(buf + n - 6, " const")) { buf[n - 6] = 0; continue; }
        break;
    }
    p = buf;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "const ", 6)) { p += 6; continue; }
        if (!strncmp(p, "volatile ", 9)) { p += 9; continue; }
        if (!strncmp(p, "typename ", 9)) { p += 9; continue; }
        if (!strncmp(p, "struct ", 7)) { p += 7; continue; }
        if (!strncmp(p, "class ", 6)) { p += 6; continue; }
        break;
    }
    lt = strchr(p, '<');
    if (lt) {
        char head[256];
        size_t hl = (size_t)(lt - p);
        const char *h;
        if (hl >= sizeof(head)) hl = sizeof(head) - 1;
        memcpy(head, p, hl);
        head[hl] = 0;
        h = leaf(head);
        /* Smart pointers and wrappers dereferenced with -> (or containers
           indexed by the caller) expose their first template argument. */
        if (arrow == 1 && (!strcmp(h, "unique_ptr") || !strcmp(h, "shared_ptr") || !strcmp(h, "weak_ptr") || !strcmp(h, "optional"))) {
            char inner[512];
            const char *a = lt + 1, *e = a;
            int depth = 0;
            while (*e && !(depth == 0 && (*e == ',' || *e == '>'))) { if (*e == '<') depth++; else if (*e == '>') depth--; e++; }
            if ((size_t)(e - a) < sizeof(inner)) { memcpy(inner, a, (size_t)(e - a)); inner[e - a] = 0; bare_type(inner, 0, out, cap); return; }
        }
        if (arrow == 2) { /* element type */
            char inner[512];
            const char *a = lt + 1, *e = a;
            int depth = 0;
            while (*e && !(depth == 0 && (*e == ',' || *e == '>'))) { if (*e == '<') depth++; else if (*e == '>') depth--; e++; }
            if ((size_t)(e - a) < sizeof(inner)) { memcpy(inner, a, (size_t)(e - a)); inner[e - a] = 0; bare_type(inner, 0, out, cap); return; }
        }
        *lt = 0;
    }
    copy_trim(out, cap, leaf(p));
}

static int64_t class_in_units(LsServer *s, LsUnit *u, const char *name, LsUnit **out_unit) {
    int64_t c = find_class_named(&u->ir, name);
    LsUnit **units;
    size_t n, i;
    if (c >= 0) { *out_unit = u; return c; }
    n = server_units(s, &units);
    for (i = 0; i < n; ++i) {
        if (units[i] == u) continue;
        c = find_class_named(&units[i]->ir, name);
        if (c >= 0 && !is_external(&units[i]->ir.nodes[c])) { *out_unit = units[i]; free(units); return c; }
    }
    free(units);
    return -1;
}

static void symbol_type(LsServer *s, const LsSymbol *sym, char *out, size_t cap);

static int resolve_depth = 0;

int receiver_class(LsServer *s, LsUnit *u, size_t sep, LsUnit **out_unit, int64_t *out_cls) {
    const IR *ir = &u->ir;
    char type[512], bare[256];
    int arrow = TKIS(ir, sep, "->") ? 1 : 0;
    size_t r;
    LsSymbol sym;
    if (sep == 0 || resolve_depth > 6) return 0;
    r = sep - 1;
    type[0] = 0;
    resolve_depth++;
    if (TKIS(ir, r, "this")) {
        int64_t fn = enclosing_callable(ir, r), cls = callable_owner_class(ir, fn);
        resolve_depth--;
        if (cls < 0) return 0;
        *out_unit = u;
        *out_cls = cls;
        return 1;
    }
    if (TKIS(ir, r, "]") || TKIS(ir, r, ")")) {
        const char *close = TK(ir, r), *open = !strcmp(close, "]") ? "[" : "(";
        int depth = 0;
        size_t i = r;
        for (;;) {
            if (TKIS(ir, i, close)) depth++;
            else if (TKIS(ir, i, open) && --depth == 0) break;
            if (i == 0) { resolve_depth--; return 0; }
            i--;
        }
        if (i == 0 || !is_name(ir, i - 1)) { resolve_depth--; return 0; }
        if (sym_resolve(s, u, i - 1, &sym)) {
            symbol_type(s, &sym, type, sizeof(type));
            if (!strcmp(open, "[")) { bare_type(type, 2, bare, sizeof(bare)); goto lookup; }
        }
    } else if (is_name(ir, r)) {
        if (sym_resolve(s, u, r, &sym)) symbol_type(s, &sym, type, sizeof(type));
    }
    bare_type(type, arrow, bare, sizeof(bare));
lookup:
    resolve_depth--;
    if (!bare[0]) return 0;
    {
        int64_t c = class_in_units(s, u, bare, out_unit);
        if (c < 0) return 0;
        *out_cls = c;
        return 1;
    }
}

/* ------------------------------------------------------------------------- */
/* Members                                                                    */
/* ------------------------------------------------------------------------- */

static void base_names(const LsUnit *u, int64_t cls, char names[][128], size_t *count) {
    const IR *ir = &u->ir;
    const IRNode *c = &ir->nodes[cls];
    const char *text = NULL;
    char buf[1024];
    *count = 0;
    if (!is_external(c) && c->first_tok < ir->tokens.n) {
        size_t i, n = 0;
        for (i = (size_t)c->first_tok; i <= (size_t)c->last_tok && i < ir->tokens.n && !TKIS(ir, i, "{"); ++i) {
            size_t l = strlen(TK(ir, i));
            if (n + l + 2 >= sizeof(buf)) break;
            memcpy(buf + n, TK(ir, i), l);
            n += l;
            buf[n++] = ' ';
        }
        buf[n] = 0;
        text = buf;
    } else if (c->value) {
        copy_trim(buf, sizeof(buf), c->value);
        text = buf;
    }
    if (!text) return;
    {
        char *brace = strchr(buf, '{');
        char *colon = buf;
        if (brace) *brace = 0;
        /* first ':' that is not part of '::' */
        while ((colon = strchr(colon, ':')) != NULL) {
            if (colon[1] == ':') { colon += 2; continue; }
            if (colon > buf && colon[-1] == ':') { colon++; continue; }
            break;
        }
        if (!colon) return;
        colon++;
        while (*colon && *count < 8) {
            char part[256];
            char *comma = colon;
            int depth = 0;
            size_t pl;
            while (*comma && !(depth == 0 && *comma == ',')) { if (*comma == '<') depth++; else if (*comma == '>') depth--; comma++; }
            pl = (size_t)(comma - colon);
            if (pl >= sizeof(part)) pl = sizeof(part) - 1;
            memcpy(part, colon, pl);
            part[pl] = 0;
            {
                char *lt = strchr(part, '<');
                char *w, *last = NULL;
                if (lt) *lt = 0;
                for (w = strtok(part, " \t\r\n"); w; w = strtok(NULL, " \t\r\n"))
                    if (strcmp(w, "public") && strcmp(w, "protected") && strcmp(w, "private") && strcmp(w, "virtual")) last = w;
                if (last) copy_trim(names[(*count)++], 128, leaf(last));
            }
            colon = *comma ? comma + 1 : comma;
        }
    }
}

static void members_rec(LsServer *s, LsUnit *u, int64_t cls, void (*cb)(void *, LsUnit *, int64_t), void *ctx, int depth) {
    int64_t c;
    char bases[8][128];
    size_t nb, i;
    if (depth > 4) return;
    for (c = u->ir.nodes[cls].first_child; c >= 0; c = u->ir.nodes[c].next_sibling) {
        NodeKind k = u->ir.nodes[c].kind;
        if (is_member_kind(k) || is_type_kind(k) || k == N_ENUM) cb(ctx, u, c);
    }
    base_names(u, cls, bases, &nb);
    for (i = 0; i < nb; ++i) {
        LsUnit *bu = NULL;
        int64_t b = class_in_units(s, u, bases[i], &bu);
        if (b >= 0 && !(bu == u && b == cls)) members_rec(s, bu, b, cb, ctx, depth + 1);
    }
}

void class_members(LsServer *s, LsUnit *u, int64_t cls, void (*cb)(void *, LsUnit *, int64_t), void *ctx) {
    members_rec(s, u, cls, cb, ctx, 0);
}

typedef struct { const char *name; LsUnit *unit; int64_t node; } MemberFind;

static void member_find_cb(void *ctx, LsUnit *u, int64_t m) {
    MemberFind *f = (MemberFind *)ctx;
    const IRNode *n = &u->ir.nodes[m];
    if (!n->name || strcmp(n->name, f->name) || n->kind == N_CONSTRUCTOR) return;
    /* the property wins over its backing field */
    if (f->node >= 0 && !(n->kind == N_PROPERTY && f->unit->ir.nodes[f->node].kind == N_FIELD_DECL)) return;
    f->unit = u;
    f->node = m;
}

/* ------------------------------------------------------------------------- */
/* Resolution                                                                 */
/* ------------------------------------------------------------------------- */

static void set_node(LsSymbol *out, LsUnit *u, int64_t node) {
    const IRNode *n = &u->ir.nodes[node];
    out->kind = SYM_NODE;
    out->unit = u;
    out->node = node;
    out->decl_tok = decl_name_token(&u->ir, node);
    copy_trim(out->name, sizeof(out->name), n->name);
    if (n->kind == N_PARAM_DECL) out->scope = n->parent;
}

static int global_kind(const IR *ir, const IRNode *n) {
    if (!n->name || !*n->name) return 0;
    switch (n->kind) {
    case N_CLASS: case N_STRUCT: case N_UNION: case N_ENUM: case N_NAMESPACE: case N_TYPEDEF:
    case N_USING_DECL: case N_CONCEPT: case N_CUSTOM_DECORATOR: case N_ENUMERATOR:
        return 1;
    case N_FUNCTION: case N_VAR_DECL: {
        int64_t p;
        for (p = n->parent; p >= 0; p = ir->nodes[p].parent)
            if (is_type_kind(ir->nodes[p].kind) || is_callable_kind_ls(ir->nodes[p].kind)) return 0;
        return 1;
    }
    default:
        return 0;
    }
}

static int64_t find_global(const IR *ir, const char *name) {
    size_t i;
    int64_t ext = -1;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!n->name || strcmp(n->name, name) || !global_kind(ir, n)) continue;
        if (!is_external(n)) return (int64_t)i;
        if (ext < 0) ext = (int64_t)i;
    }
    return ext;
}

typedef struct { const IR *ir; const char *name; size_t best; } LocalSearch;

static void local_search_cb(void *ctx, size_t tok) {
    LocalSearch *ls = (LocalSearch *)ctx;
    if (!strcmp(TK(ls->ir, tok), ls->name)) ls->best = tok;
}

static int resolve_scope_member(LsServer *s, LsUnit *u, const char *qual, const char *name, LsSymbol *out) {
    const IR *ir = &u->ir;
    size_t i;
    LsUnit *cu = NULL;
    int64_t cls = class_in_units(s, u, qual, &cu);
    if (cls >= 0) {
        MemberFind f;
        f.name = name; f.unit = NULL; f.node = -1;
        class_members(s, cu, cls, member_find_cb, &f);
        if (f.node >= 0) { set_node(out, f.unit, f.node); return 1; }
    }
    /* enum enumerators / namespace members */
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        int64_t p = n->parent;
        if (!n->name || strcmp(n->name, name) || p < 0) continue;
        if ((ir->nodes[p].kind == N_ENUM || ir->nodes[p].kind == N_NAMESPACE) && ir->nodes[p].name && !strcmp(ir->nodes[p].name, qual)) {
            set_node(out, u, (int64_t)i);
            return 1;
        }
    }
    return 0;
}

int sym_resolve(LsServer *s, LsUnit *u, size_t tok, LsSymbol *out) {
    const IR *ir = &u->ir;
    const char *name, *prev;
    size_t i;
    int64_t fn;
    memset(out, 0, sizeof(*out));
    out->node = -1;
    out->scope = -1;
    out->decl_tok = SIZE_MAX;
    if (!is_word(ir, tok)) return 0;
    name = TK(ir, tok);
    prev = tok > 0 ? TK(ir, tok - 1) : "";
    copy_trim(out->name, sizeof(out->name), name);

    /* @decorator (possibly qualified: @ns::deco) */
    {
        size_t j = tok;
        while (j >= 2 && TKIS(ir, j - 1, "::") && is_word(ir, j - 2)) j -= 2;
        if (j >= 1 && TKIS(ir, j - 1, "@")) {
            if (j == tok && (!strcmp(name, "register") || !strcmp(name, "exposed"))) { out->kind = SYM_NATIVE_DECORATOR; return 1; }
            for (i = 0; i < ir->node_count; ++i)
                if (ir->nodes[i].kind == N_CUSTOM_DECORATOR && ir->nodes[i].name && !strcmp(ir->nodes[i].name, name)) { set_node(out, u, (int64_t)i); return 1; }
            return 0;
        }
    }
    if (is_keyword(name)) return 0;

    /* Declaration name tokens (constructor/destructor names mean the class). */
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!is_declaration_kind(n->kind) || is_external(n) || n->first_tok > tok || n->last_tok < tok) continue;
        if (!n->name || strcmp(n->name, name) || decl_name_token(ir, (int64_t)i) != tok) continue;
        if (n->kind == N_CONSTRUCTOR) {
            int64_t cls = callable_owner_class(ir, (int64_t)i);
            if (cls >= 0) { set_node(out, u, cls); return 1; }
        }
        set_node(out, u, (int64_t)i);
        return 1;
    }

    /* name=value inside a call: the parameter of the resolved callee */
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *a = &ir->nodes[i];
        NamedCallPlan plan;
        if (a->kind != N_NAMED_ARG || a->first_tok != tok || a->parent < 0) continue;
        if (plan_named_call(ir, &ir->nodes[a->parent], &plan, NULL) == 1) {
            int64_t p = function_param_by_name(ir, &ir->nodes[plan.fn], name, NULL);
            named_call_plan_free(&plan);
            if (p >= 0) { set_node(out, u, p); out->scope = ir->nodes[p].parent; return 1; }
        } else named_call_plan_free(&plan);
        break;
    }

    if (!strcmp(prev, ".") || !strcmp(prev, "->")) {
        LsUnit *cu = NULL;
        int64_t cls;
        if (receiver_class(s, u, tok - 1, &cu, &cls)) {
            MemberFind f;
            f.name = name; f.unit = NULL; f.node = -1;
            class_members(s, cu, cls, member_find_cb, &f);
            if (f.node >= 0) { set_node(out, f.unit, f.node); return 1; }
        }
        return 0;
    }
    if (!strcmp(prev, "::") && tok >= 2 && is_word(ir, tok - 2))
        return resolve_scope_member(s, u, TK(ir, tok - 2), name, out);

    fn = enclosing_callable(ir, tok);
    if (fn >= 0) {
        int64_t c;
        LocalSearch ls;
        for (c = ir->nodes[fn].first_child; c >= 0; c = ir->nodes[c].next_sibling)
            if (ir->nodes[c].kind == N_PARAM_DECL && ir->nodes[c].name && !strcmp(ir->nodes[c].name, name)) {
                set_node(out, u, c);
                out->scope = fn;
                return 1;
            }
        ls.ir = ir; ls.name = name; ls.best = SIZE_MAX;
        list_locals(u, fn, tok, local_search_cb, &ls);
        if (ls.best != SIZE_MAX) {
            out->kind = SYM_LOCAL;
            out->unit = u;
            out->scope = fn;
            out->decl_tok = ls.best;
            local_type(u, ls.best, out->type, sizeof(out->type));
            return 1;
        }
    }
    {
        int64_t cls = fn >= 0 ? callable_owner_class(ir, fn) : enclosing_class_body(ir, tok);
        if (cls >= 0) {
            MemberFind f;
            f.name = name; f.unit = NULL; f.node = -1;
            class_members(s, u, cls, member_find_cb, &f);
            if (f.node >= 0) { set_node(out, f.unit, f.node); return 1; }
        }
    }
    {
        int64_t g = find_global(ir, name);
        LsUnit **units;
        size_t n;
        if (g >= 0 && !is_external(&ir->nodes[g])) { set_node(out, u, g); return 1; }
        n = server_units(s, &units);
        for (i = 0; i < n; ++i) {
            int64_t h;
            if (units[i] == u) continue;
            h = find_global(&units[i]->ir, name);
            if (h >= 0 && !is_external(&units[i]->ir.nodes[h])) { set_node(out, units[i], h); free(units); return 1; }
        }
        free(units);
        if (g >= 0) { set_node(out, u, g); return 1; }
    }
    return 0;
}

int sym_locate(LsServer *s, LsSymbol *sym) {
    LsUnit **units;
    size_t n, i, j;
    const IRNode *want;
    if (sym->kind != SYM_NODE || sym->decl_tok != SIZE_MAX) return sym->decl_tok != SIZE_MAX;
    want = &sym->unit->ir.nodes[sym->node];
    n = server_units(s, &units);
    for (i = 0; i < n; ++i) {
        const IR *ir = &units[i]->ir;
        for (j = 0; j < ir->node_count; ++j) {
            const IRNode *c = &ir->nodes[j];
            size_t t;
            if (is_external(c) || c->kind != want->kind || !c->name || !want->name || strcmp(c->name, want->name)) continue;
            if (want->qualified && c->qualified && strcmp(want->qualified, c->qualified)) continue;
            t = decl_name_token(ir, (int64_t)j);
            if (t == SIZE_MAX) continue;
            sym->unit = units[i];
            sym->node = (int64_t)j;
            sym->decl_tok = t;
            if (c->kind == N_PARAM_DECL) sym->scope = c->parent;
            free(units);
            return 1;
        }
    }
    free(units);
    return 0;
}

static void symbol_type(LsServer *s, const LsSymbol *sym, char *out, size_t cap) {
    out[0] = 0;
    (void)s;
    if (sym->kind == SYM_LOCAL) {
        copy_trim(out, cap, sym->type);
        if (!strcmp(out, "auto") || !strcmp(out, "const auto") || !strcmp(out, "auto&") || !strcmp(out, "const auto&")) {
            /* auto x = Type(...) / new Type / make_unique<Type>(...) */
            const IR *ir = &sym->unit->ir;
            size_t i = sym->decl_tok + 1;
            if (TKIS(ir, i, "=")) {
                for (i = i + 1; i < ir->tokens.n && !TKIS(ir, i, ";") && i < sym->decl_tok + 12; ++i) {
                    LsUnit *cu;
                    if (is_name(ir, i) && class_in_units(s, sym->unit, TK(ir, i), &cu) >= 0) { copy_trim(out, cap, TK(ir, i)); return; }
                }
            }
        }
        return;
    }
    if (sym->kind == SYM_NODE) {
        const IRNode *n = &sym->unit->ir.nodes[sym->node];
        if (n->kind == N_PROPERTY && n->parent >= 0) {
            int64_t c;
            for (c = sym->unit->ir.nodes[n->parent].first_child; c >= 0; c = sym->unit->ir.nodes[c].next_sibling) {
                const IRNode *f = &sym->unit->ir.nodes[c];
                if (f->kind == N_FIELD_DECL && f->name && n->name && !strcmp(f->name, n->name)) { copy_trim(out, cap, f->type); return; }
            }
        }
        if (is_callable_kind_ls(n->kind)) copy_trim(out, cap, n->return_type);
        else if (is_type_kind(n->kind)) copy_trim(out, cap, n->name);
        else copy_trim(out, cap, n->type);
    }
}

/* ------------------------------------------------------------------------- */
/* Occurrences                                                                */
/* ------------------------------------------------------------------------- */

typedef struct { LsOcc *v; size_t n; } OccList;

static void occ_add(OccList *l, LsUnit *u, size_t tok, int is_decl) {
    size_t i;
    for (i = 0; i < l->n; ++i) if (l->v[i].unit == u && l->v[i].tok == tok) return;
    l->v = (LsOcc *)xrealloc(l->v, (l->n + 1) * sizeof(LsOcc));
    l->v[l->n].unit = u;
    l->v[l->n].tok = tok;
    l->v[l->n].is_decl = is_decl;
    l->n++;
}

static int is_any_decl_token(const IR *ir, size_t tok) {
    size_t i;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!is_declaration_kind(n->kind) || is_external(n) || n->first_tok > tok || n->last_tok < tok) continue;
        if (decl_name_token(ir, (int64_t)i) == tok) return 1;
    }
    return 0;
}

/* Name of the function called by the argument list containing tok. */
static const char *call_callee_name(const IR *ir, size_t tok, const char **type_before) {
    size_t i = tok;
    int depth = 0;
    *type_before = NULL;
    while (i > 0) {
        const char *t = TK(ir, i - 1);
        if (!strcmp(t, ")") || !strcmp(t, "]") || !strcmp(t, "}")) depth++;
        else if (!strcmp(t, "(") || !strcmp(t, "[") || !strcmp(t, "{")) {
            if (depth == 0) {
                if (strcmp(t, "(") || i < 2 || !is_word(ir, i - 2)) return NULL;
                if (i >= 3 && is_word(ir, i - 3)) *type_before = TK(ir, i - 3);
                return TK(ir, i - 2);
            }
            depth--;
        } else if (!strcmp(t, ";")) return NULL;
        i--;
    }
    return NULL;
}

static int64_t owner_of(const IR *ir, int64_t node) {
    int64_t p;
    for (p = ir->nodes[node].parent; p >= 0; p = ir->nodes[p].parent)
        if (is_type_kind(ir->nodes[p].kind) || ir->nodes[p].kind == N_ENUM) return p;
    return -1;
}

size_t sym_occurrences(LsServer *s, const LsSymbol *sym_in, LsOcc **out) {
    LsSymbol sym = *sym_in;
    OccList l;
    LsUnit **units;
    size_t nu, ui, i;
    const char *name = sym.name;
    l.v = NULL;
    l.n = 0;
    *out = NULL;
    if (sym.kind == SYM_NATIVE_DECORATOR || sym.kind == SYM_NONE || !*name) return 0;
    sym_locate(s, &sym);

    /* Locals and parameters: the function body, plus name=... labels. */
    if (sym.kind == SYM_LOCAL || (sym.kind == SYM_NODE && sym.unit->ir.nodes[sym.node].kind == N_PARAM_DECL)) {
        const IR *ir = &sym.unit->ir;
        int64_t fn = sym.kind == SYM_LOCAL ? sym.scope : sym.unit->ir.nodes[sym.node].parent;
        if (fn >= 0 && !is_external(&ir->nodes[fn]) && ir->nodes[fn].first_tok < ir->tokens.n) {
            for (i = (size_t)ir->nodes[fn].first_tok; i <= (size_t)ir->nodes[fn].last_tok && i < ir->tokens.n; ++i) {
                if (!TKIS(ir, i, name) || !is_word(ir, i)) continue;
                if (i > 0 && (TKIS(ir, i - 1, ".") || TKIS(ir, i - 1, "->") || TKIS(ir, i - 1, "::"))) continue;
                occ_add(&l, sym.unit, i, i == sym.decl_tok);
            }
        }
        if (sym.kind == SYM_NODE && fn >= 0) {
            const IRNode *f = &ir->nodes[fn];
            const char *callee = f->name;
            int64_t owner = f->kind == N_CONSTRUCTOR ? callable_owner_class(ir, fn) : -1;
            nu = server_units(s, &units);
            for (ui = 0; ui < nu; ++ui) {
                const IR *uir = &units[ui]->ir;
                for (i = 0; i + 1 < uir->tokens.n; ++i) {
                    const char *cn, *tb;
                    if (!TKIS(uir, i, name) || !TKIS(uir, i + 1, "=")) continue;
                    if (!(TKIS(uir, i - 1, "(") || TKIS(uir, i - 1, ","))) continue;
                    cn = call_callee_name(uir, i, &tb);
                    if (!cn || !callee) continue;
                    if (!strcmp(cn, callee) || (owner >= 0 && tb && ir->nodes[owner].name && !strcmp(tb, ir->nodes[owner].name)))
                        occ_add(&l, units[ui], i, 0);
                }
            }
            free(units);
        }
        *out = l.v;
        return l.n;
    }

    nu = server_units(s, &units);
    {
        const IR *dir = &sym.unit->ir;
        const IRNode *d = &dir->nodes[sym.node];
        int member = is_member_kind(d->kind) || d->kind == N_ENUMERATOR;
        int64_t owner = owner_of(dir, sym.node);
        const char *owner_name = owner >= 0 ? dir->nodes[owner].name : NULL;
        int decorator = d->kind == N_CUSTOM_DECORATOR;
        for (ui = 0; ui < nu; ++ui) {
            LsUnit *u = units[ui];
            const IR *ir = &u->ir;
            for (i = 0; i < ir->tokens.n; ++i) {
                const char *prev;
                int ok = 0;
                if (!TKIS(ir, i, name) || !is_word(ir, i)) continue;
                prev = i > 0 ? TK(ir, i - 1) : "";
                if (decorator) ok = !strcmp(prev, "@") || is_any_decl_token(ir, i);
                else if (member && d->kind == N_ENUMERATOR) {
                    ok = strcmp(prev, ".") && strcmp(prev, "->") &&
                         (strcmp(prev, "::") || (owner_name && i >= 2 && TKIS(ir, i - 2, owner_name)));
                } else if (member) {
                    if (!strcmp(prev, ".") || !strcmp(prev, "->")) {
                        LsUnit *cu = NULL;
                        int64_t cls;
                        ok = 1;
                        if (receiver_class(s, u, i - 1, &cu, &cls) && owner_name && cu->ir.nodes[cls].name &&
                            strcmp(cu->ir.nodes[cls].name, owner_name)) ok = 0;
                    } else if (!strcmp(prev, "::")) {
                        ok = owner_name && i >= 2 && TKIS(ir, i - 2, owner_name);
                    } else {
                        int64_t fn = enclosing_callable(ir, i), cls = fn >= 0 ? callable_owner_class(ir, fn) : enclosing_class_body(ir, i);
                        ok = cls >= 0 && owner_name && ir->nodes[cls].name && !strcmp(ir->nodes[cls].name, owner_name);
                        if (ok && fn >= 0) {
                            /* a parameter or local with the same name shadows the member */
                            LsSymbol shadow;
                            if (sym_resolve(s, u, i, &shadow) && (shadow.kind == SYM_LOCAL ||
                                (shadow.kind == SYM_NODE && shadow.unit->ir.nodes[shadow.node].kind == N_PARAM_DECL))) ok = 0;
                        }
                    }
                } else {
                    ok = strcmp(prev, ".") && strcmp(prev, "->") && strcmp(prev, "@");
                    if (ok && strcmp(prev, "::")) {
                        int64_t fn = enclosing_callable(ir, i);
                        if (fn >= 0) {
                            LsSymbol shadow;
                            if (sym_resolve(s, u, i, &shadow) && shadow.kind == SYM_LOCAL) ok = 0;
                        }
                    }
                }
                if (ok) occ_add(&l, u, i, is_any_decl_token(ir, i));
            }
        }
    }
    free(units);
    *out = l.v;
    return l.n;
}

/* ------------------------------------------------------------------------- */
/* Display text                                                               */
/* ------------------------------------------------------------------------- */

char *param_text(const LsUnit *u, int64_t param);

static void collapse(Str *out, const char *p, size_t n) {
    size_t i;
    int space = 0;
    for (i = 0; i < n; ++i) {
        if (isspace((unsigned char)p[i])) { space = out->len > 0; continue; }
        if (space) { str_ch(out, ' '); space = 0; }
        str_ch(out, p[i]);
    }
}

char *decl_text(const LsUnit *u, int64_t node) {
    const IR *ir = &u->ir;
    const IRNode *n = &ir->nodes[node];
    Str o;
    str_init(&o);
    if (!is_external(n) && n->first_tok < ir->tokens.n) {
        size_t a = (size_t)n->first_tok, b = (size_t)n->last_tok, i, end = b;
        int depth = 0;
        if (b >= ir->tokens.n) b = ir->tokens.n - 1;
        end = b;
        for (i = a; i <= b; ++i) {
            const char *t = TK(ir, i);
            if (!strcmp(t, "(") || !strcmp(t, "[") || !strcmp(t, "<")) depth++;
            else if ((!strcmp(t, ")") || !strcmp(t, "]") || !strcmp(t, ">")) && depth) depth--;
            else if (depth == 0 && (!strcmp(t, "{") || !strcmp(t, ";"))) { end = i ? i - 1 : i; break; }
            else if (depth == 0 && n->kind == N_CONSTRUCTOR && !strcmp(t, ":") && i > a && TKIS(ir, i - 1, ")")) { end = i - 1; break; }
        }
        if (end >= a) collapse(&o, (const char *)ir->source + ir->tokens.v[a].byte_start,
                               (size_t)(ir->tokens.v[end].byte_end - ir->tokens.v[a].byte_start));
    } else {
        /* Imported declarations have no tokens: rebuild from their fields. */
        char t[512];
        copy_trim(t, sizeof(t), is_callable_kind_ls(n->kind) ? n->return_type : n->type);
        if (is_callable_kind_ls(n->kind)) {
            int64_t c;
            int first = 1;
            if (t[0] && strcmp(t, "~")) { str_put(&o, t); str_ch(&o, ' '); }
            if (!strcmp(t, "~")) str_ch(&o, '~');
            str_put(&o, n->qualified ? n->qualified : (n->name ? n->name : ""));
            str_ch(&o, '(');
            for (c = n->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
                char *p;
                if (ir->nodes[c].kind != N_PARAM_DECL) continue;
                if (!first) str_put(&o, ", ");
                first = 0;
                p = param_text(u, c);
                str_put(&o, p);
                free(p);
            }
            str_ch(&o, ')');
        } else if (n->kind == N_FIELD_DECL || n->kind == N_VAR_DECL || n->kind == N_PARAM_DECL) {
            str_put(&o, t);
            if (o.len) str_ch(&o, ' ');
            str_put(&o, n->name ? n->name : "");
        } else if (n->kind == N_CUSTOM_DECORATOR) {
            copy_trim(t, sizeof(t), n->return_type);
            str_put(&o, t);
            str_put(&o, " @");
            str_put(&o, n->name ? n->name : "");
            str_put(&o, "()");
        } else if (is_type_kind(n->kind) || n->kind == N_ENUM || n->kind == N_NAMESPACE) {
            str_put(&o, n->kind == N_NAMESPACE ? "namespace " : n->kind == N_ENUM ? "enum " : n->kind == N_STRUCT ? "struct " : n->kind == N_UNION ? "union " : "class ");
            str_put(&o, n->qualified ? n->qualified : (n->name ? n->name : ""));
        } else if (n->value) {
            const char *v = n->value, *brace = strchr(v, '{');
            collapse(&o, v, brace ? (size_t)(brace - v) : strlen(v));
            if (o.len && o.data[o.len - 1] == ';') o.len--;
        } else if (n->name) {
            str_put(&o, n->name);
        }
    }
    if (n->kind == N_PROPERTY) {
        o.len = 0;
        str_put(&o, "property ");
        str_put(&o, n->name ? n->name : "");
        str_put(&o, " {");
        if (property_child_kind(ir, n, N_PROPERTY_GET) >= 0) str_put(&o, " get;");
        if (property_child_kind(ir, n, N_PROPERTY_SET) >= 0) str_put(&o, " set;");
        str_put(&o, " }");
    }
    if (!o.data) str_put(&o, "");
    return str_take(&o);
}

char *param_text(const LsUnit *u, int64_t param) {
    const IR *ir = &u->ir;
    const IRNode *p = &ir->nodes[param];
    Str o;
    str_init(&o);
    if (!is_external(p) && p->first_tok < ir->tokens.n && p->last_tok < ir->tokens.n) {
        collapse(&o, (const char *)ir->source + ir->tokens.v[p->first_tok].byte_start,
                 (size_t)(ir->tokens.v[p->last_tok].byte_end - ir->tokens.v[p->first_tok].byte_start));
    } else {
        char t[256];
        copy_trim(t, sizeof(t), p->type);
        str_put(&o, t);
        if (p->name) { if (o.len) str_ch(&o, ' '); str_put(&o, p->name); }
        if ((p->flags & NF_PARAM_HAS_DEFAULT) && p->value) { char v[256]; copy_trim(v, sizeof(v), p->value); str_put(&o, " = "); str_put(&o, v); }
    }
    if (!o.data) str_put(&o, "");
    return str_take(&o);
}
