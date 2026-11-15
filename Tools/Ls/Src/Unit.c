/*
 * Unit.c
 *
 * Parsed source files, diagnostics lists, documents and settings.
 */

#include "Server.h"
#include "Guard.h"
#include "Core/Memory.h"
#include "Lexer/Lexer.h"
#include "Parser/Parser.h"

void diag_list_clear(LsDiagList *l) {
    size_t i;
    for (i = 0; i < l->n; ++i) free(l->v[i].message);
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

void diag_add(LsDiagList *l, int severity, size_t start, size_t end, const char *source, const char *fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    l->v = (LsDiag *)xrealloc(l->v, (l->n + 1) * sizeof(LsDiag));
    l->v[l->n].severity = severity;
    l->v[l->n].start = start;
    l->v[l->n].end = end < start ? start : end;
    l->v[l->n].message = xstrdup0(msg);
    l->v[l->n].source = source;
    l->n++;
}

void unit_free(LsUnit *u) {
    if (u->has_ir) ir_free(&u->ir);
    linemap_free(&u->lines);
    free(u->path);
    free(u->uri);
    memset(u, 0, sizeof(*u));
}

typedef struct { LsUnit *u; } ParseArgs;

static void parse_only(void *arg) {
    LsUnit *u = ((ParseArgs *)arg)->u;
    lex_cpp(u->ir.source, u->ir.source_size, &u->ir.tokens);
    parse_source(&u->ir);
}

void unit_parse_text(LsUnit *u, const char *path, char *text, size_t len) {
    ParseArgs a;
    if (u->has_ir) ir_free(&u->ir);
    linemap_free(&u->lines);
    if (!u->path) u->path = xstrdup0(path);
    ir_init(&u->ir);
    u->ir.path = xstrdup0(path);
    u->ir.source = (uint8_t *)text;
    u->ir.source_size = len;
    u->has_ir = 1;
    a.u = u;
    guard_run(parse_only, &a, NULL, NULL);
    linemap_build(&u->lines, (const char *)u->ir.source, u->ir.source_size);
}

static int tok_is_word(const Token *t) {
    return t->kind == TK_IDENTIFIER;
}

size_t unit_token_at(const LsUnit *u, size_t offset) {
    const TokenVec *tv = &u->ir.tokens;
    size_t lo = 0, hi, i;
    if (!u->has_ir || !tv->n) return SIZE_MAX;
    hi = tv->n;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (tv->v[mid].byte_start <= offset) lo = mid; else hi = mid;
    }
    i = lo;
    if (tv->v[i].kind == TK_EOF) { if (!i) return SIZE_MAX; i--; }
    /* Cursor right after a word ("foo|." or "foo|"): prefer the word. */
    if (offset == tv->v[i].byte_start && i > 0 && tv->v[i - 1].byte_end == offset &&
        tok_is_word(&tv->v[i - 1]) && !tok_is_word(&tv->v[i])) i--;
    if (offset < tv->v[i].byte_start || offset > tv->v[i].byte_end) return SIZE_MAX;
    return i;
}

void unit_token_range(const LsUnit *u, size_t tok, size_t *start, size_t *end) {
    *start = (size_t)u->ir.tokens.v[tok].byte_start;
    *end = (size_t)u->ir.tokens.v[tok].byte_end;
}

void unit_put_range(Str *out, const LsUnit *u, size_t start, size_t end) {
    long sl, sc, el, ec;
    char b[160];
    linemap_position(&u->lines, start, &sl, &sc);
    linemap_position(&u->lines, end, &el, &ec);
    snprintf(b, sizeof(b), "{\"start\":{\"line\":%ld,\"character\":%ld},\"end\":{\"line\":%ld,\"character\":%ld}}", sl, sc, el, ec);
    str_put(out, b);
}

const char *unit_uri(LsUnit *u) {
    if (!u->uri) u->uri = path_to_uri(u->path);
    return u->uri;
}

void unit_put_location(Str *out, const LsUnit *u, size_t start, size_t end) {
    str_put(out, "{\"uri\":");
    json_put_string(out, unit_uri((LsUnit *)u));
    str_put(out, ",\"range\":");
    unit_put_range(out, u, start, end);
    str_ch(out, '}');
}

LsDocument *server_doc(LsServer *s, const char *uri) {
    size_t i;
    if (!uri) return NULL;
    for (i = 0; i < s->doc_n; ++i) if (!strcmp(s->docs[i]->unit.uri, uri)) return s->docs[i];
    return NULL;
}

LsDocument *server_doc_by_path(LsServer *s, const char *path) {
    size_t i;
    for (i = 0; i < s->doc_n; ++i) if (path_equal(s->docs[i]->unit.path, path)) return s->docs[i];
    return NULL;
}

static void set_string(char **dst, const char *v) {
    free(*dst);
    *dst = xstrdup0(v ? v : "");
}

/* Settings come from initializationOptions or workspace/didChangeConfiguration
   ({ includePaths: [], cppCompiler, cppStandard, cppCheck: "onSave"|"off" }). */
void server_settings(LsServer *s, const Json *settings) {
    const Json *inc, *c;
    const char *v;
    if (!settings || settings->type != JSON_OBJECT) return;
    inc = json_get(settings, "includePaths");
    if (inc && inc->type == JSON_ARRAY) {
        size_t i;
        for (i = 0; i < s->include_dir_n; ++i) free(s->include_dirs[i]);
        free(s->include_dirs);
        s->include_dirs = NULL;
        s->include_dir_n = 0;
        for (c = inc->child; c; c = c->next) {
            Str p;
            if (c->type != JSON_STRING || !*c->string) continue;
            str_init(&p);
            /* Relative entries are relative to the workspace folder. */
            if (s->root && !(c->string[0] == '/' || c->string[0] == '\\' || (isalpha((unsigned char)c->string[0]) && c->string[1] == ':'))) {
                str_put(&p, s->root);
                str_ch(&p, '/');
            }
            str_put(&p, c->string);
            s->include_dirs = (char **)xrealloc(s->include_dirs, (s->include_dir_n + 1) * sizeof(char *));
            s->include_dirs[s->include_dir_n++] = str_take(&p);
        }
    }
    if ((v = json_str(settings, "cppCompiler")) != NULL) set_string(&s->cpp_compiler, v);
    if ((v = json_str(settings, "cppStandard")) != NULL && *v) set_string(&s->cpp_standard, v);
    if ((v = json_str(settings, "cppCheck")) != NULL) s->cpp_check_on_save = strcmp(v, "off") != 0;
}

const char *const *server_include_dirs(LsServer *s, size_t *n) {
    *n = s->include_dir_n;
    return (const char *const *)s->include_dirs;
}
