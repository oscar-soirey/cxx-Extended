/*
 * Analysis.c
 *
 * Document analysis with the real compiler pipeline, diagnostics, and the
 * optional C++ check of the generated code (g++/clang++ -fsyntax-only, run
 * on save; #line directives map its errors back into the .cppe file).
 */

#include "Server.h"
#include "Guard.h"
#include "Protocol.h"
#include "CodeGen/CodeGen.h"
#include "Core/FileIo.h"
#include "Core/Memory.h"
#include "Driver/Pipeline.h"
#include "Frontend/IncludeResolver.h"
#include "Lexer/Token.h"
#include "Runtime/Runtime.h"

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define LS_MKDIR(p) _mkdir(p)
#define LS_POPEN _popen
#define LS_PCLOSE _pclose
#else
#include <sys/stat.h>
#define LS_MKDIR(p) mkdir((p), 0777)
#define LS_POPEN popen
#define LS_PCLOSE pclose
#endif

/* ------------------------------------------------------------------------- */
/* Compiler diagnostics -> document ranges                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    LsUnit *unit;
    LsDiagList *list;
} HookCtx;

static size_t word_end(const LsUnit *u, size_t off) {
    size_t tok = unit_token_at(u, off);
    if (tok != SIZE_MAX && u->ir.tokens.v[tok].byte_start == off) return (size_t)u->ir.tokens.v[tok].byte_end;
    while (off < u->ir.source_size && (isalnum(u->ir.source[off]) || u->ir.source[off] == '_')) off++;
    return off;
}

/* Range of the #include line naming `path` (for errors inside headers). */
static int include_line_for(const LsUnit *u, const char *path, size_t *start, size_t *end) {
    const char *base = path_basename(path);
    size_t i;
    for (i = 0; i < u->ir.tokens.n; ++i) {
        const Token *t = &u->ir.tokens.v[i];
        if (t->kind == TK_PP && strstr(t->text, "include") && strstr(t->text, base)) {
            *start = (size_t)t->byte_start;
            *end = (size_t)t->byte_end;
            return 1;
        }
    }
    return 0;
}

static void collect_diagnostic(void *vctx, int severity, const char *path, unsigned line, unsigned column, const char *message) {
    HookCtx *ctx = (HookCtx *)vctx;
    LsUnit *u = ctx->unit;
    int sev = severity == CXXE_DIAG_ERROR ? 1 : 2;
    size_t start = 0, end = 0;
    if (!u->lines.line_start) linemap_build(&u->lines, (const char *)u->ir.source, u->ir.source_size);
    if (!path || path_equal(path, u->path)) {
        if (line) {
            start = linemap_offset_from_compiler(&u->lines, line, column);
            end = word_end(u, start);
            if (end == start) end = start + 1 <= u->ir.source_size ? start + 1 : start;
        }
        diag_add(ctx->list, sev, start, end, "cxxe", "%s", message);
    } else {
        include_line_for(u, path, &start, &end);
        diag_add(ctx->list, sev, start, end, "cxxe", "%s:%u:%u: %s", path_basename(path), line, column, message);
    }
}

/* Unbalanced brackets make every later error confusing: report them first. */
static void check_brackets(LsUnit *u, LsDiagList *list) {
    size_t *stack = NULL, sp = 0, cap = 0, i;
    for (i = 0; i < u->ir.tokens.n; ++i) {
        const char *t = u->ir.tokens.v[i].text;
        char c = (u->ir.tokens.v[i].kind == TK_PUNCT && t[1] == 0) ? t[0] : 0;
        if (c == '(' || c == '[' || c == '{') {
            if (sp == cap) { cap = cap ? cap * 2 : 64; stack = (size_t *)xrealloc(stack, cap * sizeof(size_t)); }
            stack[sp++] = i;
        } else if (c == ')' || c == ']' || c == '}') {
            char want = c == ')' ? '(' : c == ']' ? '[' : '{';
            if (!sp || u->ir.tokens.v[stack[sp - 1]].text[0] != want) {
                size_t s0, e0;
                unit_token_range(u, i, &s0, &e0);
                diag_add(list, 1, s0, e0, "cxxe", "unmatched '%c'", c);
                free(stack);
                return;
            }
            sp--;
        }
    }
    if (sp) {
        size_t s0, e0;
        unit_token_range(u, stack[sp - 1], &s0, &e0);
        diag_add(list, 1, s0, e0, "cxxe", "'%s' is never closed", u->ir.tokens.v[stack[sp - 1]].text);
    }
    free(stack);
}

/* ------------------------------------------------------------------------- */
/* Analysis                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    IR *ir;
    const char *path;
    uint8_t *text;
    size_t len;
    const char *const *dirs;
    size_t dir_n;
} BuildArgs;

static void do_build(void *arg) {
    BuildArgs *a = (BuildArgs *)arg;
    build_ir_from_memory(a->ir, a->path, a->text, a->len, a->dirs, a->dir_n);
}

typedef struct { const IR *ir; Str out; } EmitArgs;

static void do_emit(void *arg) {
    EmitArgs *a = (EmitArgs *)arg;
    emit_cpp_to_str(a->ir, &a->out);
}

static void index_includes(LsServer *s, LsUnit *u) {
    size_t i, n;
    const char *const *dirs = server_include_dirs(s, &n);
    for (i = 0; i < u->ir.node_count; ++i) {
        const IRNode *nd = &u->ir.nodes[i];
        char *resolved;
        if (nd->kind != N_INCLUDE || !nd->value || (nd->flags & NF_EXTERNAL_DECL)) continue;
        resolved = cxxe_resolve_include(u->path, nd->value, dirs, n);
        if (resolved) { index_add_external(s, resolved); free(resolved); }
    }
}

void analyze_document(LsServer *s, LsDocument *d) {
    LsUnit *u = &d->unit;
    HookCtx ctx;
    BuildArgs b;
    size_t n;
    char *text_copy = xstrndup0(d->text ? d->text : "", d->text_len);
    size_t len = d->text_len;
    diag_list_clear(&d->diags);
    free(d->generated);
    d->generated = NULL;
    if (u->has_ir) ir_free(&u->ir);
    linemap_free(&u->lines);
    ir_init(&u->ir);
    u->has_ir = 1;
    /* Lines must exist before the hook maps positions. */
    u->ir.source = (uint8_t *)text_copy;
    u->ir.source_size = len;
    linemap_build(&u->lines, text_copy, len);
    ctx.unit = u;
    ctx.list = &d->diags;
    b.ir = &u->ir;
    b.path = u->path;
    b.text = (uint8_t *)text_copy;
    b.len = len;
    b.dirs = server_include_dirs(s, &n);
    b.dir_n = n;
    d->analysis_failed = guard_run(do_build, &b, collect_diagnostic, &ctx);
    /* build_ir_from_memory re-initialised the IR around the same buffer. */
    if (u->ir.source != (uint8_t *)text_copy) { u->ir.source = (uint8_t *)text_copy; u->ir.source_size = len; }
    if (u->ir.tokens.n) check_brackets(u, &d->diags);
    if (!d->analysis_failed) {
        EmitArgs e;
        e.ir = &u->ir;
        str_init(&e.out);
        if (guard_run(do_emit, &e, collect_diagnostic, &ctx) == 0) d->generated = str_take(&e.out);
        else { str_free(&e.out); d->analysis_failed = 1; }
    }
    index_includes(s, u);
}

/* ------------------------------------------------------------------------- */
/* C++ check of the generated code                                            */
/* ------------------------------------------------------------------------- */

static void make_dirs(const char *path) {
    char *p = xstrdup0(path), *q;
    for (q = p + 1; *q; ++q) {
        if (*q == '/' || *q == '\\') { char c = *q; *q = 0; LS_MKDIR(p); *q = c; }
    }
    LS_MKDIR(p);
    free(p);
}

static int write_file(const char *path, const char *data, size_t n) {
    FILE *f;
    char *dir = path_parent(path);
    make_dirs(dir);
    free(dir);
    f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(data, 1, n, f);
    fclose(f);
    return 1;
}

static void put_quoted(Str *cmd, const char *arg) {
    str_ch(cmd, '"');
    str_put(cmd, arg);
    str_ch(cmd, '"');
}

/* Spelled name of an include directive ("dir/x.hppe" for #include "dir/x.hppe"). */
static char *include_spelling(const char *directive) {
    const char *a = strchr(directive, '"'), *b;
    char close = '"';
    if (!a) { a = strchr(directive, '<'); close = '>'; }
    if (!a) return NULL;
    b = strchr(a + 1, close);
    if (!b) return NULL;
    return xstrndup0(a + 1, (size_t)(b - a - 1));
}

/* Translates every .hppe reachable from `u` into out_dir (same spelling,
   .hpp extension) so the generated C++ compiles like a CMake build. */
static void generate_headers(LsServer *s, const IR *ir, const char *current, const char *out_dir, int depth) {
    size_t i, n;
    const char *const *dirs = server_include_dirs(s, &n);
    if (depth > 16) return;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *nd = &ir->nodes[i];
        char *resolved, *spell, *out_path;
        Str p;
        size_t sl;
        if (nd->kind != N_INCLUDE || !nd->value || (nd->flags & NF_EXTERNAL_DECL) || !strstr(nd->value, ".hppe")) continue;
        resolved = cxxe_resolve_include(current, nd->value, dirs, n);
        spell = include_spelling(nd->value);
        if (!resolved || !spell) { free(resolved); free(spell); continue; }
        str_init(&p);
        str_put(&p, out_dir);
        str_ch(&p, '/');
        sl = strlen(spell);
        str_putn(&p, spell, sl - 1); /* .hppe -> .hpp */
        out_path = str_take(&p);
        {
            LsDocument *open = server_doc_by_path(s, resolved);
            IR hir;
            BuildArgs b;
            EmitArgs e;
            size_t len = 0;
            uint8_t *text;
            if (open) { len = open->text_len; text = (uint8_t *)xstrndup0(open->text, len); }
            else {
                FILE *f = fopen(resolved, "rb");
                Str t;
                char buf[8192];
                size_t r;
                str_init(&t);
                if (f) { while ((r = fread(buf, 1, sizeof(buf), f)) > 0) str_putn(&t, buf, r); fclose(f); }
                len = t.len;
                text = (uint8_t *)(t.data ? str_take(&t) : xstrdup0(""));
            }
            b.ir = &hir; b.path = resolved; b.text = text; b.len = len; b.dirs = dirs; b.dir_n = n;
            ir_init(&hir);
            if (guard_run(do_build, &b, NULL, NULL) == 0) {
                char *sub_dir = path_parent(out_path);
                e.ir = &hir;
                str_init(&e.out);
                if (guard_run(do_emit, &e, NULL, NULL) == 0) write_file(out_path, e.out.data ? e.out.data : "", e.out.len);
                str_free(&e.out);
                generate_headers(s, &hir, resolved, sub_dir, depth + 1);
                free(sub_dir);
            }
            ir_free(&hir);
        }
        free(out_path);
        free(resolved);
        free(spell);
    }
}

/* Parses "path:line:col: error: message" (path may contain a drive colon). */
static int parse_compiler_line(const char *line, char **path, unsigned *ln, unsigned *col, int *sev, const char **msg) {
    static const char *const levels[] = { ": fatal error: ", ": error: ", ": warning: " };
    static const int sevs[] = { 1, 1, 2 };
    size_t k;
    for (k = 0; k < 3; ++k) {
        const char *hit = strstr(line, levels[k]);
        const char *p;
        unsigned a = 0, c = 0, mul = 1;
        if (!hit) continue;
        p = hit;
        while (p > line && isdigit((unsigned char)p[-1])) { c += (unsigned)(p[-1] - '0') * mul; mul *= 10; p--; }
        if (p == hit || p == line || p[-1] != ':') continue;
        p--;
        mul = 1;
        {
            const char *q = p;
            while (q > line && isdigit((unsigned char)q[-1])) { a += (unsigned)(q[-1] - '0') * mul; mul *= 10; q--; }
            if (q == p || q == line || q[-1] != ':') continue;
            *path = xstrndup0(line, (size_t)(q - 1 - line));
        }
        *ln = a; *col = c; *sev = sevs[k];
        *msg = hit + strlen(levels[k]);
        return 1;
    }
    return 0;
}

static unsigned long hash_path(const char *s) {
    unsigned long h = 5381;
    for (; *s; ++s) h = h * 33 + (unsigned char)tolower((unsigned char)*s);
    return h;
}

void cpp_check_document(LsServer *s, LsDocument *d) {
    char dir[1024], main_path[1200], rt_dir[1100];
    Str cmd, output;
    FILE *pipe;
    size_t i, n;
    const char *const *dirs;
    diag_list_clear(&d->cpp_diags);
    if (!s->cpp_check_on_save || !s->cpp_compiler || !*s->cpp_compiler || !d->generated || !s->temp_dir) return;
    snprintf(dir, sizeof(dir), "%s/%08lx", s->temp_dir, hash_path(d->unit.path) & 0xffffffffUL);
    snprintf(rt_dir, sizeof(rt_dir), "%s/runtime", s->temp_dir);
    snprintf(main_path, sizeof(main_path), "%s/%s.cpp", dir, path_basename(d->unit.path));
    {
        char rt_file[1200];
        snprintf(rt_file, sizeof(rt_file), "%s/cxxe_runtime.hpp", rt_dir);
        write_file(rt_file, CXXE_RUNTIME_HPP, CXXE_RUNTIME_HPP_SIZE);
    }
    if (!write_file(main_path, d->generated, strlen(d->generated))) return;
    generate_headers(s, &d->unit.ir, d->unit.path, dir, 0);

    str_init(&cmd);
#ifdef _WIN32
    str_ch(&cmd, '"'); /* cmd.exe /c strips one outer pair of quotes */
#endif
    put_quoted(&cmd, s->cpp_compiler);
    str_put(&cmd, " -fsyntax-only -fdiagnostics-color=never -fno-diagnostics-show-caret -std=");
    str_put(&cmd, s->cpp_standard ? s->cpp_standard : "c++20");
    str_put(&cmd, " -I"); put_quoted(&cmd, dir);
    str_put(&cmd, " -I"); put_quoted(&cmd, rt_dir);
    {
        char *doc_dir = path_parent(d->unit.path);
        str_put(&cmd, " -I"); put_quoted(&cmd, doc_dir);
        free(doc_dir);
    }
    dirs = server_include_dirs(s, &n);
    for (i = 0; i < n; ++i) { str_put(&cmd, " -I"); put_quoted(&cmd, dirs[i]); }
    str_ch(&cmd, ' ');
    put_quoted(&cmd, main_path);
    str_put(&cmd, " 2>&1");
#ifdef _WIN32
    str_ch(&cmd, '"');
#endif
    fflush(stdout);
    pipe = LS_POPEN(cmd.data, "r");
    if (!pipe) { protocol_log(2, "C++ check: cannot run %s", cmd.data); str_free(&cmd); return; }
    str_init(&output);
    {
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof(buf), pipe)) > 0) str_putn(&output, buf, r);
    }
    {
        int status = LS_PCLOSE(pipe);
        if (status != 0 && !output.len) protocol_log(2, "C++ check failed to start: %s", cmd.data);
    }
    str_free(&cmd);
    if (output.data) {
        char *line = output.data;
        size_t count = 0;
        while (line && *line && count < 100) {
            char *nl = strchr(line, '\n');
            char *path = NULL;
            unsigned ln = 0, col = 0;
            int sev = 0;
            const char *msg = NULL;
            if (nl) *nl = 0;
            if (nl && nl > line && nl[-1] == '\r') nl[-1] = 0;
            if (parse_compiler_line(line, &path, &ln, &col, &sev, &msg) && path_equal(path, d->unit.path)) {
                size_t st = linemap_offset_from_compiler(&d->unit.lines, ln, col);
                size_t en = word_end(&d->unit, st);
                if (en == st) en = st + 1;
                diag_add(&d->cpp_diags, sev, st, en, "c++", "%s", msg);
                count++;
            }
            free(path);
            line = nl ? nl + 1 : NULL;
        }
    }
    str_free(&output);
}

static void put_diags(Str *p, const LsUnit *u, const LsDiagList *l, int *first) {
    size_t i;
    char num[16];
    for (i = 0; i < l->n; ++i) {
        if (!*first) str_ch(p, ',');
        *first = 0;
        str_put(p, "{\"range\":");
        unit_put_range(p, u, l->v[i].start, l->v[i].end);
        snprintf(num, sizeof(num), "%d", l->v[i].severity);
        str_put(p, ",\"severity\":");
        str_put(p, num);
        str_put(p, ",\"source\":");
        json_put_string(p, l->v[i].source);
        str_put(p, ",\"message\":");
        json_put_string(p, l->v[i].message);
        str_ch(p, '}');
    }
}

void publish_diagnostics(LsServer *s, LsDocument *d) {
    Str p;
    int first = 1;
    char num[32];
    (void)s;
    str_init(&p);
    str_put(&p, "{\"uri\":");
    json_put_string(&p, d->unit.uri);
    snprintf(num, sizeof(num), "%ld", d->version);
    str_put(&p, ",\"version\":");
    str_put(&p, num);
    str_put(&p, ",\"diagnostics\":[");
    put_diags(&p, &d->unit, &d->diags, &first);
    /* C++ diagnostics are only meaningful while the CXXE analysis passes. */
    if (!d->analysis_failed) put_diags(&p, &d->unit, &d->cpp_diags, &first);
    str_put(&p, "]}");
    protocol_notify("textDocument/publishDiagnostics", p.data);
    str_free(&p);
}
