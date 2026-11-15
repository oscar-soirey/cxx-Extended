/*
 * Frontend/IncludeResolver.c
 *
 * Include resolution and discovery of decorators declared in included headers.
 */

#include "Core/Common.h"
#include "Frontend/IncludeResolver.h"
#include "Core/FileIo.h"
#include "Core/Memory.h"
#include "Core/Path.h"
#include "Ir/Ir.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"
#include "Parser/ParserInternal.h"

static void discover_includes_recursive(IR *dst, const char *current_file, IncludeContext *ctx);

/* The CXXE standard headers are installed next to cxxe.exe: <exe>/stde.
   Keep the resolved executable directory globally so every parse/roundtrip
   automatically gets the same standard include search path without requiring
   an explicit -I argument. */

static void include_ctx_free(IncludeContext *ctx) {
    size_t i;
    for (i = 0; i < ctx->visited_n; ++i) free(ctx->visited[i]);
    for (i = 0; i < ctx->include_dir_n; ++i) free(ctx->include_dirs[i]);
    free(ctx->visited);
    free(ctx->include_dirs);
    memset(ctx, 0, sizeof(*ctx));
}

static void include_add_dir(IncludeContext *ctx, const char *dir) {
    if (!dir || !*dir) return;
    if (ctx->include_dir_n == ctx->include_dir_cap) {
        ctx->include_dir_cap = ctx->include_dir_cap ? ctx->include_dir_cap * 2 : 8;
        ctx->include_dirs = (char **)xrealloc(ctx->include_dirs, ctx->include_dir_cap * sizeof(*ctx->include_dirs));
    }
    ctx->include_dirs[ctx->include_dir_n++] = xstrdup0(dir);
}

static int include_visited(IncludeContext *ctx, const char *path) {
    size_t i;
    for (i = 0; i < ctx->visited_n; ++i) {
#if defined(_WIN32)
        if (_stricmp(ctx->visited[i], path) == 0) return 1;
#else
        if (strcmp(ctx->visited[i], path) == 0) return 1;
#endif
    }
    return 0;
}

static void include_mark_visited(IncludeContext *ctx, const char *path) {
    if (include_visited(ctx, path)) return;
    if (ctx->visited_n == ctx->visited_cap) {
        ctx->visited_cap = ctx->visited_cap ? ctx->visited_cap * 2 : 16;
        ctx->visited = (char **)xrealloc(ctx->visited, ctx->visited_cap * sizeof(*ctx->visited));
    }
    ctx->visited[ctx->visited_n++] = xstrdup0(path);
}

static int parse_include_spec0(const char *raw, char **name_out, int *angled_out, int *next_out) {
    const char *kw = pp_keyword(raw), *q = raw, *e;
    if (name_out) *name_out = NULL;
    if (angled_out) *angled_out = 0;
    if (next_out) *next_out = 0;
    if (strcmp(kw, "include") != 0 && strcmp(kw, "include_next") != 0 && strcmp(kw, "import") != 0) return 0;
    if (next_out && strcmp(kw, "include_next") == 0) *next_out = 1;
    while (*q && *q != '#') q++;
    if (*q == '#') q++;
    while (*q && isspace((unsigned char)*q)) q++;
    while (*q && !isspace((unsigned char)*q)) q++;
    while (*q && isspace((unsigned char)*q)) q++;
    if (*q == '"') {
        q++; e = strchr(q, '"');
        if (!e) return 0;
        if (name_out) *name_out = xstrndup0(q, (size_t)(e - q));
        return 1;
    }
    if (*q == '<') {
        q++; e = strchr(q, '>');
        if (!e) return 0;
        if (name_out) *name_out = xstrndup0(q, (size_t)(e - q));
        if (angled_out) *angled_out = 1;
        return 1;
    }
    return 0; /* macro-expanded include: cannot resolve without a preprocessor */
}

static int header_extension_candidate0(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return 1;
    return strcmp(dot, ".h") == 0 || strcmp(dot, ".hpp") == 0 || strcmp(dot, ".he") == 0 || strcmp(dot, ".hppe") == 0 ||
           strcmp(dot, ".hh") == 0 || strcmp(dot, ".hxx") == 0 || strcmp(dot, ".inl") == 0 || strcmp(dot, ".inc") == 0;
}

static char *resolve_include_path0(const char *current_file, const char *name, int angled, IncludeContext *ctx) {
    size_t i;
    char *dir, *candidate;
    if (!name || !*name) return NULL;
    dir = path_dirname0(current_file);
    if (!angled) {
        candidate = path_join0(dir, name);
        if (file_exists0(candidate)) { char *r = normalize_path0(candidate); free(candidate); free(dir); return r; }
        free(candidate);
    }
    for (i = 0; i < ctx->include_dir_n; ++i) {
        candidate = path_join0(ctx->include_dirs[i], name);
        if (file_exists0(candidate)) { char *r = normalize_path0(candidate); free(candidate); free(dir); return r; }
        free(candidate);
    }
    if (angled) {
        /* Project-local angle includes are useful in CXXE projects; allow the
           current file directory as a final fallback without touching system
           compiler search paths. */
        candidate = path_join0(dir, name);
        if (file_exists0(candidate)) { char *r = normalize_path0(candidate); free(candidate); free(dir); return r; }
        free(candidate);
    }
    free(dir);
    return NULL;
}

/* Node kinds that make up a declaration's shape. Expression nodes from
   bodies are not imported: their `aux` fields reference nodes of the
   header's own IR and are meaningless in the including unit. */
static int declaration_shape_kind(NodeKind k) {
    switch (k) {
    case N_CLASS: case N_STRUCT: case N_UNION: case N_ENUM: case N_ENUMERATOR:
    case N_TEMPLATE: case N_FIELD_DECL: case N_PROPERTY: case N_PROPERTY_GET: case N_PROPERTY_SET:
    case N_FUNCTION: case N_METHOD: case N_CONSTRUCTOR: case N_DESTRUCTOR: case N_OPERATOR_FUNCTION:
    case N_PARAM_DECL:
        return 1;
    default:
        return 0;
    }
}

static int64_t clone_ir_subtree(IR *dst, const IR *src, int64_t src_id, int64_t parent, uint32_t extra_flags);

static int64_t clone_declaration_shape(IR *dst, const IR *src, int64_t src_id, int64_t parent) {
    const IRNode *orig = &src->nodes[src_id];
    int64_t n = ir_add_node(dst, orig->kind, UINT64_MAX, UINT64_MAX, parent);
    int64_t c;
    /* Keep access bits; drop semantic flags (registration...) that belong to
       the header's own translation. */
    dst->nodes[n].flags = (orig->flags & (NF_ACCESS_MASK | NF_PARAM_HAS_DEFAULT)) | NF_EXTERNAL_DECL;
    dst->nodes[n].aux = UINT64_MAX;
    node_set_name(dst, n, orig->name);
    node_set_qualified(dst, n, orig->qualified);
    node_set_type(dst, n, orig->type);
    node_set_return(dst, n, orig->return_type);
    node_set_value(dst, n, orig->value);
    node_set_resolved(dst, n, orig->resolved_name);
    for (c = orig->first_child; c >= 0; c = src->nodes[c].next_sibling) {
        if (!declaration_shape_kind(src->nodes[c].kind)) continue;
        ir_add_child(dst, n, clone_declaration_shape(dst, src, c, n));
    }
    return n;
}

static int64_t clone_ir_subtree(IR *dst, const IR *src, int64_t src_id, int64_t parent, uint32_t extra_flags) {
    const IRNode *orig = &src->nodes[src_id];
    int64_t n = ir_add_node(dst, orig->kind, UINT64_MAX, UINT64_MAX, parent);
    int64_t c;
    dst->nodes[n].flags = orig->flags | extra_flags;
    dst->nodes[n].aux = orig->aux;
    node_set_name(dst, n, orig->name);
    node_set_qualified(dst, n, orig->qualified);
    node_set_type(dst, n, orig->type);
    node_set_return(dst, n, orig->return_type);
    node_set_value(dst, n, orig->value);
    node_set_resolved(dst, n, orig->resolved_name);
    for (c = orig->first_child; c >= 0; c = src->nodes[c].next_sibling) {
        int64_t child = clone_ir_subtree(dst, src, c, n, extra_flags);
        ir_add_child(dst, n, child);
    }
    return n;
}

/* Declarations an including file may need: custom decorators, classes
   (fields, properties, constructors, methods) and free functions (named
   arguments, default values). Nested ones come with their outermost owner.
   Imported nodes are flagged NF_EXTERNAL_DECL and own no local tokens. */
static int importable_kind(NodeKind k) {
    return k == N_CUSTOM_DECORATOR || k == N_CLASS || k == N_STRUCT || k == N_UNION ||
           k == N_FUNCTION || k == N_METHOD || k == N_CONSTRUCTOR || k == N_OPERATOR_FUNCTION;
}

static void import_declarations(IR *dst, const IR *src) {
    size_t i;
    for (i = 0; i < src->node_count; ++i) {
        const IRNode *orig = &src->nodes[i];
        int64_t n, p;
        int nested = 0;
        if (!importable_kind(orig->kind)) continue;
        for (p = orig->parent; p >= 0; p = src->nodes[p].parent)
            if (importable_kind(src->nodes[p].kind)) { nested = 1; break; }
        if (nested) continue;
        if (orig->kind == N_CUSTOM_DECORATOR) n = clone_ir_subtree(dst, src, (int64_t)i, 0, NF_EXTERNAL_DECL);
        else n = clone_declaration_shape(dst, src, (int64_t)i, 0);
        ir_add_child(dst, 0, n);
    }
}

static void discover_one_include(IR *dst, const char *current_file, const char *raw, IncludeContext *ctx, IRNode *include_node,
                                 const IR *loc_ir, uint64_t loc_tok) {
    char *name = NULL, *resolved = NULL;
    int angled = 0, next = 0;
    IR hdr;
    if (!parse_include_spec0(raw, &name, &angled, &next)) return;
    (void)next;
    resolved = resolve_include_path0(current_file, name, angled, ctx);
    if (!resolved) {
        if (!angled) {
            if (loc_ir) ir_die_at(loc_ir, loc_tok, "cannot find include file \"%s\"", name ? name : "");
            die("cannot resolve include \"%s\" included from '%s'", name ? name : "", current_file);
        }
        free(name);
        return; /* system/SDK angle include: intentionally opaque to CXXE */
    }
    free(name);
    if (include_node) node_set_resolved(dst, (int64_t)(include_node - dst->nodes), resolved);
    if (include_visited(ctx, resolved)) { free(resolved); return; }
    include_mark_visited(ctx, resolved);
    if (!header_extension_candidate0(resolved)) { free(resolved); return; }
    ir_init(&hdr);
    hdr.path = xstrdup0(resolved);
    hdr.source = read_bytes(resolved, &hdr.source_size);
    lex_cpp(hdr.source, hdr.source_size, &hdr.tokens);
    parse_source(&hdr);
    discover_includes_recursive(&hdr, resolved, ctx);
    import_declarations(dst, &hdr);
    ir_free(&hdr);
    free(resolved);
}

static void discover_includes_recursive(IR *dst, const char *current_file, IncludeContext *ctx) {
    size_t i;
    for (i = 0; i < dst->node_count; ++i) {
        const IRNode *n = &dst->nodes[i];
        if ((n->kind != N_INCLUDE && n->kind != N_IMPORT) || !n->value) continue;
        discover_one_include(dst, current_file, n->value, ctx, (IRNode *)n, dst, n->first_tok);
    }
}

/* Imports into `dst` the declarations of every header included by `scan`
   (transitively). Returns the number of imported top-level declarations. */
size_t import_included_declarations(IR *dst, const IR *scan, const char *main_path,
                                    const char *const *include_dirs, size_t include_dir_n) {
    IncludeContext ctx;
    size_t i, before = dst->node_count;
    memset(&ctx, 0, sizeof(ctx));
    for (i = 0; i < include_dir_n; ++i) include_add_dir(&ctx, include_dirs[i]);
    if (g_cxxe_executable_dir) {
        char *stde_dir = path_join0(g_cxxe_executable_dir, "stde");
        include_add_dir(&ctx, stde_dir);
        free(stde_dir);
    }
    {
        char *root = normalize_path0(main_path);
        include_mark_visited(&ctx, root);
        free(root);
    }
    for (i = 0; i < scan->node_count; ++i) {
        const IRNode *n = &scan->nodes[i];
        if ((n->kind != N_INCLUDE && n->kind != N_IMPORT) || !n->value) continue;
        discover_one_include(dst, main_path, n->value, &ctx, NULL, scan, n->first_tok);
    }
    include_ctx_free(&ctx);
    return dst->node_count - before;
}

void discover_included_decorators(IR *ir, const char *main_path, const char *const *include_dirs, size_t include_dir_n) {
    IncludeContext ctx;
    size_t i;
    memset(&ctx, 0, sizeof(ctx));
    for (i = 0; i < include_dir_n; ++i) include_add_dir(&ctx, include_dirs[i]);

    /* Standard headers are always searched from the directory containing the
       CXXE executable. This is deliberately independent from the process
       working directory and from the location of the current .cppe file. */
    if (g_cxxe_executable_dir) {
        char *stde_dir = path_join0(g_cxxe_executable_dir, "stde");
        include_add_dir(&ctx, stde_dir);
        free(stde_dir);
    }

    {
        char *root = normalize_path0(main_path);
        include_mark_visited(&ctx, root);
        free(root);
    }
    discover_includes_recursive(ir, main_path, &ctx);
    include_ctx_free(&ctx);
}

/* Resolves an include directive's text (`#include "x"` or `<x>`) as the
   compiler does. Returns a malloc'd path or NULL. */
char *cxxe_resolve_include(const char *current_file, const char *directive,
                           const char *const *include_dirs, size_t include_dir_n) {
    IncludeContext ctx;
    char *name = NULL, *resolved = NULL;
    int angled = 0, next = 0;
    size_t i;
    if (!parse_include_spec0(directive, &name, &angled, &next)) return NULL;
    memset(&ctx, 0, sizeof(ctx));
    for (i = 0; i < include_dir_n; ++i) include_add_dir(&ctx, include_dirs[i]);
    if (g_cxxe_executable_dir) {
        char *stde_dir = path_join0(g_cxxe_executable_dir, "stde");
        include_add_dir(&ctx, stde_dir);
        free(stde_dir);
    }
    resolved = resolve_include_path0(current_file, name, angled, &ctx);
    free(name);
    include_ctx_free(&ctx);
    return resolved;
}
