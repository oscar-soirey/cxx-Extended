/*
 * cxxe.c
 *
 * CXXE stand-alone C++ extended source -> IR -> C++ converter.
 *
 * IMPORTANT:
 *   - This file DOES NOT compile C++.
 *   - It DOES NOT call clang, llvm, libclang, a compiler driver, or a linker.
 *   - It lexes/parses the source itself and stores a lossless token stream.
 *   - The IR contains both generic syntax and semantic-oriented records.
 *
 * The design goal is: every input token survives the IR round-trip, while
 * the parser recognizes as many C++ constructs as possible and keeps unknown
 * constructs as generic nodes instead of discarding them. This makes the IR
 * suitable as the stable input to a future C+++ frontend/backend.
 *
 * Build:
 *   cc -std=c11 -O2 cxxe.c -o cxxe
 *
 * Usage:
 *   cxxe parse input.cpp output.cpir
 *   cxxe emit output.cpir output.cpp
 *   cxxe roundtrip input.cpp output.cpp
 *   cxxe dump output.cpir
 *
 * CPIR format: a small binary container intended for this program. It stores
 * the original bytes, the lossless tokens (including leading trivia), and the
 * parsed nodes. Node ranges refer to token indices; generators can therefore
 * either reproduce exact source or regenerate selected nodes later.
 */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#define CPIR_MAGIC "CPIR"
#define CPIR_VERSION 11u
#define ARRAY_GROW_MIN 64u

/* ------------------------------------------------------------------------- */
/* Memory / diagnostics                                                      */
/* ------------------------------------------------------------------------- */

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "cxxe: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) die("out of memory (%zu x %zu)", n, s);
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory reallocating to %zu bytes", n);
    return q;
}

static char *xstrdup0(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *r = (char *)xmalloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static int str_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------------- */
/* Dynamic byte/string buffer                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Str;

static void str_init(Str *s) { memset(s, 0, sizeof(*s)); }

static void str_reserve(Str *s, size_t extra) {
    size_t need = s->len + extra + 1;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2)) die("string too large");
        cap *= 2;
    }
    s->data = (char *)xrealloc(s->data, cap);
    s->cap = cap;
}

static void str_putn(Str *s, const char *p, size_t n) {
    if (!n) return;
    str_reserve(s, n);
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = 0;
}

static void str_put(Str *s, const char *p) {
    if (p) str_putn(s, p, strlen(p));
}

static void str_ch(Str *s, char c) {
    str_reserve(s, 1);
    s->data[s->len++] = c;
    s->data[s->len] = 0;
}

static char *str_take(Str *s) {
    char *r = s->data ? s->data : xstrdup0("");
    s->data = NULL;
    s->len = s->cap = 0;
    return r;
}

static void str_free(Str *s) {
    free(s->data);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------------- */
/* File IO                                                                    */
/* ------------------------------------------------------------------------- */

static uint8_t *read_bytes(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    long end;
    size_t n, got;
    uint8_t *p;
    if (!f) die("cannot open '%s': %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek '%s'", path);
    end = ftell(f);
    if (end < 0) die("cannot size '%s'", path);
    if (fseek(f, 0, SEEK_SET) != 0) die("cannot rewind '%s'", path);
    n = (size_t)end;
    p = (uint8_t *)xmalloc(n + 1);
    got = fread(p, 1, n, f);
    fclose(f);
    if (got != n) die("cannot read '%s'", path);
    p[n] = 0;
    if (size_out) *size_out = n;
    return p;
}

static void write_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot create '%s': %s", path, strerror(errno));
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        die("cannot write '%s'", path);
    }
    if (fclose(f) != 0) die("cannot close '%s'", path);
}

/* ------------------------------------------------------------------------- */
/* Lossless lexer                                                             */
/* ------------------------------------------------------------------------- */

typedef enum {
    TK_EOF = 0,
    TK_IDENTIFIER,
    TK_NUMBER,
    TK_STRING,
    TK_CHAR,
    TK_PUNCT,
    TK_PP
} TokenKind;

typedef struct {
    uint32_t kind;
    uint32_t line;
    uint32_t column;
    uint64_t byte_start;
    uint64_t byte_end;
    char *trivia;   /* exact whitespace/comments before token */
    char *text;     /* exact token bytes */
} Token;

typedef struct {
    Token *v;
    size_t n;
    size_t cap;
} TokenVec;

static void token_push(TokenVec *tv, Token t) {
    if (tv->n == tv->cap) {
        tv->cap = tv->cap ? tv->cap * 2 : ARRAY_GROW_MIN;
        tv->v = (Token *)xrealloc(tv->v, tv->cap * sizeof(*tv->v));
    }
    tv->v[tv->n++] = t;
}

static int is_ident_start_c(unsigned char c) {
    return c == '_' || isalpha(c) || c >= 0x80;
}

static int is_ident_continue_c(unsigned char c) {
    return c == '_' || isalnum(c) || c >= 0x80;
}

static int punct3(const char *p, size_t rem) {
    static const char *const a[] = {
        "<=>", "...", ">>>", "<<=", ">>=", "->*", ":::", NULL
    };
    size_t i;
    for (i = 0; a[i]; ++i)
        if (strlen(a[i]) <= rem && strncmp(p, a[i], strlen(a[i])) == 0) return (int)strlen(a[i]);
    return 0;
}

static int punct2(const char *p, size_t rem) {
    static const char *const a[] = {
        "::", "->", ".*", "++", "--", "&&", "||", "<<", ">>", "<=", ">=",
        "==", "!=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##",
        "??", "?.", "?:", "co_await", NULL
    };
    size_t i;
    for (i = 0; a[i]; ++i)
        if (strlen(a[i]) <= rem && strncmp(p, a[i], strlen(a[i])) == 0) return (int)strlen(a[i]);
    return 0;
}

static void lex_cpp(const uint8_t *src, size_t n, TokenVec *out) {
    size_t i = 0, trivia_start = 0;
    uint32_t line = 1, col = 1;
    while (i < n) {
        size_t tok_start, tok_end;
        Token t;
        memset(&t, 0, sizeof(t));

        if (isspace(src[i])) {
            while (i < n && isspace(src[i])) {
                if (src[i] == '\n') { line++; col = 1; }
                else col++;
                i++;
            }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            i += 2; col += 2;
            while (i < n && src[i] != '\n') { i++; col++; }
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            i += 2; col += 2;
            while (i + 1 < n) {
                if (src[i] == '*' && src[i + 1] == '/') { i += 2; col += 2; break; }
                if (src[i] == '\n') { i++; line++; col = 1; }
                else { i++; col++; }
            }
            continue;
        }

        tok_start = i;
        t.line = line;
        t.column = col;
        t.byte_start = i;
        t.trivia = (char *)xmalloc(i - trivia_start + 1);
        if (i > trivia_start) memcpy(t.trivia, src + trivia_start, i - trivia_start);
        t.trivia[i - trivia_start] = 0;

        /* Preprocessor directive: only if # is the first non-trivia byte on a line. */
        {
            size_t ls = i, only_hspace = 1;
            while (ls > 0 && src[ls - 1] != '\n') --ls;
            {
                size_t q = ls;
                while (q < i) {
                    if (src[q] != ' ' && src[q] != '\t' && src[q] != '\r' && src[q] != '\v' && src[q] != '\f') { only_hspace = 0; break; }
                    q++;
                }
            }
            if (src[i] == '#' && only_hspace) {
                size_t j = i;
                while (j < n) {
                    if (src[j] == '\\' && j + 1 < n && src[j + 1] == '\n') { j += 2; continue; }
                    if (src[j] == '\n') break;
                    j++;
                }
                tok_end = j;
                t.kind = TK_PP;
                t.text = (char *)xmalloc(tok_end - tok_start + 1);
                memcpy(t.text, src + tok_start, tok_end - tok_start);
                t.text[tok_end - tok_start] = 0;
                token_push(out, t);
                while (i < tok_end) { i++; col++; }
                trivia_start = i;
                continue;
            }
        }

        if (is_ident_start_c(src[i])) {
            i++;
            col++;
            while (i < n && is_ident_continue_c(src[i])) { i++; col++; }
            tok_end = i;
            t.kind = TK_IDENTIFIER;
        } else if (isdigit(src[i]) || (src[i] == '.' && i + 1 < n && isdigit(src[i + 1]))) {
            int base_prefix = 0;
            if (src[i] == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X' || src[i + 1] == 'b' || src[i + 1] == 'B')) base_prefix = 2;
            (void)base_prefix;
            while (i < n) {
                unsigned char c = src[i];
                if (isalnum(c) || c == '_' || c == '.' || c == '+' || c == '-') {
                    /* Stop + / - unless immediately after e/E/p/P. */
                    if ((c == '+' || c == '-') && i > tok_start) {
                        unsigned char prev = src[i - 1];
                        if (prev != 'e' && prev != 'E' && prev != 'p' && prev != 'P') break;
                    }
                    i++; col++;
                } else break;
            }
            tok_end = i;
            t.kind = TK_NUMBER;
        } else if (src[i] == '"') {
            i++; col++;
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) { i += 2; col += 2; continue; }
                if (src[i] == '"') { i++; col++; break; }
                if (src[i] == '\n') { line++; col = 1; i++; }
                else { i++; col++; }
            }
            tok_end = i;
            t.kind = TK_STRING;
        } else if (src[i] == '\'') {
            i++; col++;
            while (i < n) {
                if (src[i] == '\\' && i + 1 < n) { i += 2; col += 2; continue; }
                if (src[i] == '\'') { i++; col++; break; }
                if (src[i] == '\n') { line++; col = 1; i++; }
                else { i++; col++; }
            }
            tok_end = i;
            t.kind = TK_CHAR;
        } else {
            int k = punct3((const char *)src + i, n - i);
            if (!k) k = punct2((const char *)src + i, n - i);
            if (!k) k = 1;
            i += (size_t)k;
            col += (uint32_t)k;
            tok_end = i;
            t.kind = TK_PUNCT;
        }

        t.byte_end = tok_end;
        t.text = (char *)xmalloc(tok_end - tok_start + 1);
        memcpy(t.text, src + tok_start, tok_end - tok_start);
        t.text[tok_end - tok_start] = 0;
        token_push(out, t);
        trivia_start = i;
    }

    {
        Token eof;
        memset(&eof, 0, sizeof(eof));
        eof.kind = TK_EOF;
        eof.line = line;
        eof.column = col;
        eof.byte_start = eof.byte_end = n;
        eof.trivia = (char *)xmalloc(n - trivia_start + 1);
        if (n > trivia_start) memcpy(eof.trivia, src + trivia_start, n - trivia_start);
        eof.trivia[n - trivia_start] = 0;
        eof.text = xstrdup0("");
        token_push(out, eof);
    }
}

static void tokens_free(TokenVec *tv) {
    size_t i;
    for (i = 0; i < tv->n; ++i) {
        free(tv->v[i].trivia);
        free(tv->v[i].text);
    }
    free(tv->v);
    memset(tv, 0, sizeof(*tv));
}

/* ------------------------------------------------------------------------- */
/* IR                                                                         */
/* ------------------------------------------------------------------------- */

typedef enum {
    N_TRANSLATION_UNIT = 1,
    N_RAW,
    N_PREPROCESSOR,
    N_INCLUDE,
    N_DEFINE,
    N_UNDEF,
    N_IFDEF,
    N_IFNDEF,
    N_IF,
    N_ELIF,
    N_ELSE,
    N_ENDIF,
    N_PRAGMA,
    N_ERROR_DIRECTIVE,
    N_WARNING_DIRECTIVE,
    N_ATTRIBUTE,
    N_DECORATOR,
    N_DECORATOR_TEMPLATE,
    N_DECORATOR_TEMPLATE_ARG,
    N_CUSTOM_DECORATOR,
    N_DECORATOR_TARGET,
    N_NAMESPACE,
    N_NAMESPACE_ALIAS,
    N_USING_DIRECTIVE,
    N_USING_DECL,
    N_LINKAGE_SPEC,
    N_MODULE,
    N_IMPORT,
    N_EXPORT,
    N_CLASS,
    N_STRUCT,
    N_UNION,
    N_ENUM,
    N_ENUMERATOR,
    N_TEMPLATE,
    N_TEMPLATE_TYPE_PARAM,
    N_TEMPLATE_NON_TYPE_PARAM,
    N_TEMPLATE_TEMPLATE_PARAM,
    N_CONCEPT,
    N_REQUIRES,
    N_TYPEDEF,
    N_ALIAS,
    N_FRIEND,
    N_FIELD_DECL,
    N_PROPERTY,
    N_PROPERTY_GET,
    N_PROPERTY_SET,
    N_PROPERTY_ACCESS,
    N_VAR_DECL,
    N_VAR_ASSIGN,
    N_FUNCTION,
    N_METHOD,
    N_CONSTRUCTOR,
    N_DESTRUCTOR,
    N_OPERATOR_FUNCTION,
    N_PARAM_DECL,
    N_RETURN,
    N_IF_STMT,
    N_FOR_STMT,
    N_RANGE_FOR_STMT,
    N_WHILE_STMT,
    N_DO_STMT,
    N_SWITCH_STMT,
    N_MATCH_STMT,
    N_MATCH_CASE,
    N_CASE_STMT,
    N_DEFAULT_STMT,
    N_BREAK_STMT,
    N_CONTINUE_STMT,
    N_GOTO_STMT,
    N_LABEL_STMT,
    N_TRY_STMT,
    N_CATCH,
    N_THROW,
    N_CO_RETURN,
    N_CO_AWAIT,
    N_CO_YIELD,
    N_EXPR,
    N_FUNC_CALL,
    N_CALL_ARG,
    N_NAMED_ARG,
    N_MEMBER_CALL,
    N_GET_MEMBER,
    N_VAR_REF,
    N_MEMBER_ACCESS,
    N_INDEX_EXPR,
    N_BINARY_EXPR,
    N_UNARY_EXPR,
    N_TERNARY_EXPR,
    N_ASSIGN_EXPR,
    N_CAST_EXPR,
    N_NEW_EXPR,
    N_DELETE_EXPR,
    N_LAMBDA,
    N_THIS_EXPR,
    N_INIT_LIST,
    N_LITERAL,
    N_SIZEOF_EXPR,
    N_ALIGNOF_EXPR,
    N_TYPEID_EXPR,
    N_NOEXCEPT_EXPR,
    N_TYPE_TRAIT,
    N_UNKNOWN
} NodeKind;

#define NF_ACCESS_MASK       0x00000003u
#define NF_ACCESS_PRIVATE    0x00000000u
#define NF_ACCESS_PROTECTED  0x00000001u
#define NF_ACCESS_PUBLIC     0x00000002u
#define NF_DECORATOR_NATIVE  0x00000010u
#define NF_DECORATOR_REGISTER 0x00000020u
#define NF_DECORATOR_EXPOSED  0x00000040u
#define NF_REGISTERED         0x00000800u
#define NF_EXPOSED            0x00001000u
#define NF_GET_MEMBER_RUNTIME 0x00002000u
#define NF_DECORATOR_CUSTOM   0x00000080u
#define NF_DECORATOR_VARIADIC 0x00000100u
#define NF_EXTERNAL_DECL     0x00004000u
#define NF_INCLUDE_NEXT      0x00008000u

typedef struct {
    NodeKind kind;
    uint32_t flags;
    uint64_t first_tok;
    uint64_t last_tok; /* inclusive */
    char *name;
    char *qualified;
    char *type;
    char *return_type;
    char *operator_spelling;
    char *value;
    char *resolved_name;
    uint64_t aux;
    int64_t parent;
    int64_t first_child;
    int64_t last_child;
    int64_t next_sibling;
    int64_t prev_sibling;
} IRNode;

typedef struct {
    uint8_t *source;
    size_t source_size;
    TokenVec tokens;
    IRNode *nodes;
    size_t node_count;
    size_t node_cap;
} IR;

static void ir_init(IR *ir) { memset(ir, 0, sizeof(*ir)); }

static void ir_free(IR *ir) {
    size_t i;
    free(ir->source);
    tokens_free(&ir->tokens);
    for (i = 0; i < ir->node_count; ++i) {
        free(ir->nodes[i].name);
        free(ir->nodes[i].qualified);
        free(ir->nodes[i].type);
        free(ir->nodes[i].return_type);
        free(ir->nodes[i].operator_spelling);
        free(ir->nodes[i].value);
        free(ir->nodes[i].resolved_name);
    }
    free(ir->nodes);
    memset(ir, 0, sizeof(*ir));
}

static int64_t ir_add_node(IR *ir, NodeKind kind, uint64_t first, uint64_t last, int64_t parent) {
    IRNode n;
    memset(&n, 0, sizeof(n));
    n.kind = kind;
    n.first_tok = first;
    n.last_tok = last;
    n.parent = parent;
    n.first_child = n.last_child = n.next_sibling = n.prev_sibling = -1;
    if (ir->node_count == ir->node_cap) {
        ir->node_cap = ir->node_cap ? ir->node_cap * 2 : 1024;
        ir->nodes = (IRNode *)xrealloc(ir->nodes, ir->node_cap * sizeof(*ir->nodes));
    }
    ir->nodes[ir->node_count] = n;
    return (int64_t)ir->node_count++;
}

static void ir_add_child(IR *ir, int64_t parent, int64_t child) {
    IRNode *p = &ir->nodes[parent];
    IRNode *c = &ir->nodes[child];
    c->parent = parent;
    if (p->last_child >= 0) {
        IRNode *prev = &ir->nodes[p->last_child];
        prev->next_sibling = child;
        c->prev_sibling = p->last_child;
    } else {
        p->first_child = child;
    }
    p->last_child = child;
}

static const char *node_kind_name(NodeKind k) {
#define K(x) case x: return #x
    switch (k) {
        K(N_TRANSLATION_UNIT); K(N_RAW); K(N_PREPROCESSOR); K(N_INCLUDE); K(N_DEFINE);
        K(N_UNDEF); K(N_IFDEF); K(N_IFNDEF); K(N_IF); K(N_ELIF); K(N_ELSE); K(N_ENDIF);
        K(N_PRAGMA); K(N_ERROR_DIRECTIVE); K(N_WARNING_DIRECTIVE); K(N_ATTRIBUTE); K(N_DECORATOR);
        K(N_DECORATOR_TEMPLATE); K(N_DECORATOR_TEMPLATE_ARG); K(N_CUSTOM_DECORATOR); K(N_DECORATOR_TARGET);
        K(N_NAMESPACE); K(N_NAMESPACE_ALIAS); K(N_USING_DIRECTIVE); K(N_USING_DECL);
        K(N_LINKAGE_SPEC); K(N_MODULE); K(N_IMPORT); K(N_EXPORT); K(N_CLASS); K(N_STRUCT);
        K(N_UNION); K(N_ENUM); K(N_ENUMERATOR); K(N_TEMPLATE); K(N_TEMPLATE_TYPE_PARAM);
        K(N_TEMPLATE_NON_TYPE_PARAM); K(N_TEMPLATE_TEMPLATE_PARAM); K(N_CONCEPT); K(N_REQUIRES);
        K(N_TYPEDEF); K(N_ALIAS); K(N_FRIEND); K(N_FIELD_DECL); K(N_PROPERTY); K(N_PROPERTY_GET); K(N_PROPERTY_SET); K(N_PROPERTY_ACCESS); K(N_VAR_DECL); K(N_VAR_ASSIGN);
        K(N_FUNCTION); K(N_METHOD); K(N_CONSTRUCTOR); K(N_DESTRUCTOR);
        K(N_OPERATOR_FUNCTION); K(N_PARAM_DECL); K(N_RETURN); K(N_IF_STMT); K(N_FOR_STMT);
        K(N_RANGE_FOR_STMT); K(N_WHILE_STMT); K(N_DO_STMT); K(N_SWITCH_STMT); K(N_MATCH_STMT); K(N_MATCH_CASE); K(N_CASE_STMT);
        K(N_DEFAULT_STMT); K(N_BREAK_STMT); K(N_CONTINUE_STMT); K(N_GOTO_STMT); K(N_LABEL_STMT);
        K(N_TRY_STMT); K(N_CATCH); K(N_THROW); K(N_CO_RETURN); K(N_CO_AWAIT); K(N_CO_YIELD);
        K(N_EXPR); K(N_FUNC_CALL); K(N_CALL_ARG); K(N_NAMED_ARG); K(N_MEMBER_CALL); K(N_GET_MEMBER); K(N_VAR_REF); K(N_MEMBER_ACCESS);
        K(N_INDEX_EXPR); K(N_BINARY_EXPR); K(N_UNARY_EXPR); K(N_TERNARY_EXPR); K(N_ASSIGN_EXPR);
        K(N_CAST_EXPR); K(N_NEW_EXPR); K(N_DELETE_EXPR); K(N_LAMBDA); K(N_THIS_EXPR);
        K(N_INIT_LIST); K(N_LITERAL); K(N_SIZEOF_EXPR); K(N_ALIGNOF_EXPR); K(N_TYPEID_EXPR);
        K(N_NOEXCEPT_EXPR); K(N_TYPE_TRAIT); K(N_UNKNOWN);
    }
#undef K
    return "N_UNKNOWN";
}

/* ------------------------------------------------------------------------- */
/* Parser helpers                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    IR *ir;
    size_t i;
    size_t end;
    int64_t root;
    char **scope;
    size_t scope_n;
    size_t scope_cap;
    int64_t *pending_decorators;
    size_t pending_decorator_n;
    size_t pending_decorator_cap;
} Parser;

static const char *pt(Parser *p, size_t i) {
    if (i >= p->ir->tokens.n) return "";
    return p->ir->tokens.v[i].text;
}

static uint32_t pk(Parser *p, size_t i) {
    if (i >= p->ir->tokens.n) return TK_EOF;
    return p->ir->tokens.v[i].kind;
}

static int peq(Parser *p, size_t i, const char *s) { return str_eq(pt(p, i), s); }

static int is_word_tok(Parser *p, size_t i) {
    uint32_t k = pk(p, i);
    return k == TK_IDENTIFIER || k == TK_NUMBER || k == TK_STRING || k == TK_CHAR;
}

static int is_keyword(const char *s) {
    static const char *const kws[] = {
        "alignas","alignof","and","and_eq","asm","atomic_cancel","atomic_commit",
        "atomic_noexcept","auto","bitand","bitor","bool","break","case","catch",
        "char","char8_t","char16_t","char32_t","class","compl","concept","const",
        "consteval","constexpr","constinit","const_cast","continue","co_await","co_return",
        "co_yield","decltype","default","delete","do","double","dynamic_cast","else",
        "enum","explicit","export","extern","false","float","for","friend","goto",
        "if","inline","int","long","mutable","namespace","new","noexcept","not","not_eq",
        "nullptr","operator","or","or_eq","private","protected","public","reflexpr","register",
        "reinterpret_cast","requires","return","short","signed","sizeof","static","static_assert",
        "static_cast","struct","switch","synchronized","template","this","thread_local","throw",
        "true","try","typedef","typeid","typename","union","unsigned","using","virtual","void",
        "volatile","wchar_t","while","dynamic","match","xor","xor_eq","module","import","override","final","transaction_safe",
        "transaction_safe_dynamic","__attribute__","__declspec","nullptr","constinit","requires","concept",
        NULL
    };
    size_t i;
    for (i = 0; kws[i]; ++i) if (strcmp(kws[i], s) == 0) return 1;
    return 0;
}

static int is_identifier(Parser *p, size_t i) {
    return pk(p, i) == TK_IDENTIFIER && !is_keyword(pt(p, i));
}

static int is_decorator_identifier(Parser *p, size_t i) {
    return pk(p, i) == TK_IDENTIFIER || pk(p, i) == TK_PUNCT;
}

static size_t skip_balanced(Parser *p, size_t i, size_t end, const char *open, const char *close) {
    int depth = 0;
    while (i < end) {
        if (peq(p, i, open)) depth++;
        else if (peq(p, i, close)) {
            depth--;
            if (depth == 0) return i;
        }
        i++;
    }
    return end ? end - 1 : 0;
}

static size_t find_matching(Parser *p, size_t open_i, size_t end) {
    const char *open = pt(p, open_i);
    const char *close = NULL;
    if (str_eq(open, "(")) close = ")";
    else if (str_eq(open, "[")) close = "]";
    else if (str_eq(open, "{")) close = "}";
    else if (str_eq(open, "<")) close = ">";
    else return open_i;
    return skip_balanced(p, open_i, end, open, close);
}

static size_t find_stmt_end(Parser *p, size_t i, size_t end) {
    int par = 0, br = 0, cur = 0;
    while (i < end) {
        if (peq(p, i, "(")) par++;
        else if (peq(p, i, ")")) { if (par) par--; }
        else if (peq(p, i, "[")) br++;
        else if (peq(p, i, "]")) { if (br) br--; }
        else if (peq(p, i, "{")) cur++;
        else if (peq(p, i, "}")) { if (cur) cur--; else return i; }
        else if (peq(p, i, ";") && par == 0 && br == 0 && cur == 0) return i;
        i++;
    }
    return end ? end - 1 : 0;
}

static char *token_range_text(Parser *p, size_t a, size_t b) {
    Str s;
    size_t i;
    str_init(&s);
    if (b < a) return xstrdup0("");
    if (b >= p->ir->tokens.n) b = p->ir->tokens.n - 1;
    for (i = a; i <= b; ++i) {
        str_put(&s, p->ir->tokens.v[i].trivia);
        str_put(&s, p->ir->tokens.v[i].text);
    }
    return str_take(&s);
}

static char *join_qualified(Parser *p, size_t a, size_t b) {
    Str s;
    size_t i;
    int need = 0;
    str_init(&s);
    for (i = a; i <= b && i < p->ir->tokens.n; ++i) {
        const char *t = pt(p, i);
        if (is_identifier(p, i)) {
            if (need) str_put(&s, "::");
            str_put(&s, t);
            need = 1;
        }
    }
    if (!s.len && a <= b) str_put(&s, pt(p, a));
    return str_take(&s);
}

static char *scope_qualified(Parser *p, const char *name) {
    Str s;
    size_t i;
    str_init(&s);
    for (i = 0; i < p->scope_n; ++i) {
        if (s.len) str_put(&s, "::");
        str_put(&s, p->scope[i]);
    }
    if (name && *name) {
        if (s.len) str_put(&s, "::");
        str_put(&s, name);
    }
    return str_take(&s);
}

static void scope_push(Parser *p, const char *name) {
    if (!name || !*name) return;
    if (p->scope_n == p->scope_cap) {
        p->scope_cap = p->scope_cap ? p->scope_cap * 2 : 16;
        p->scope = (char **)xrealloc(p->scope, p->scope_cap * sizeof(*p->scope));
    }
    p->scope[p->scope_n++] = xstrdup0(name);
}

static void scope_pop(Parser *p) {
    if (p->scope_n) free(p->scope[--p->scope_n]);
}

static void node_set_name(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].name);
    ir->nodes[id].name = xstrdup0(s ? s : "");
}

static void node_set_qualified(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].qualified);
    ir->nodes[id].qualified = xstrdup0(s ? s : "");
}

static void node_set_type(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].type);
    ir->nodes[id].type = xstrdup0(s ? s : "");
}

static void node_set_return(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].return_type);
    ir->nodes[id].return_type = xstrdup0(s ? s : "");
}

static void node_set_value(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].value);
    ir->nodes[id].value = xstrdup0(s ? s : "");
}

static void node_set_resolved(IR *ir, int64_t id, const char *s) {
    free(ir->nodes[id].resolved_name);
    ir->nodes[id].resolved_name = xstrdup0(s ? s : "");
}


/* ------------------------------------------------------------------------- */
/* CXXE native decorators                                                     */
/* ------------------------------------------------------------------------- */

static void parser_pending_push(Parser *p, int64_t id) {
    if (p->pending_decorator_n == p->pending_decorator_cap) {
        p->pending_decorator_cap = p->pending_decorator_cap ? p->pending_decorator_cap * 2 : 8;
        p->pending_decorators = (int64_t *)xrealloc(p->pending_decorators,
            p->pending_decorator_cap * sizeof(*p->pending_decorators));
    }
    p->pending_decorators[p->pending_decorator_n++] = id;
}

static void parser_apply_pending(Parser *p, int64_t target) {
    size_t i;
    for (i = 0; i < p->pending_decorator_n; ++i) {
        int64_t d = p->pending_decorators[i];
        p->ir->nodes[d].aux = (uint64_t)target;
        ir_add_child(p->ir, target, d);
    }
    p->pending_decorator_n = 0;
}

static size_t find_template_close_cpp(Parser *p, size_t open_i, size_t end_exclusive) {
    size_t i;
    int depth = 0;
    if (open_i >= p->ir->tokens.n || !str_eq(pt(p, open_i), "<")) return SIZE_MAX;
    for (i = open_i; i < end_exclusive && i < p->ir->tokens.n; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "<")) depth++;
        else if (str_eq(t, ">")) {
            if (depth > 0) depth--;
            if (depth == 0) return i;
        } else if (str_eq(t, ">>")) {
            if (depth <= 2) return i;
            depth -= 2;
        } else if (str_eq(t, ">>>")) {
            if (depth <= 3) return i;
            depth -= 3;
        }
    }
    return SIZE_MAX;
}

static size_t split_template_args_parser(Parser *p, size_t a, size_t b,
                                          size_t *starts, size_t *ends, size_t cap) {
    size_t count = 0, start = a, i;
    int par = 0, br = 0, cur = 0, angle = 0;
    if (a > b) return 0;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")") && par) par--;
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]") && br) br--;
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}") && cur) cur--;
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (str_eq(t, ">>")) angle -= angle >= 2 ? 2 : angle;
        else if (str_eq(t, ">>>")) angle -= angle >= 3 ? 3 : angle;
        else if (str_eq(t, ",") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            if (start <= i - 1 && count < cap) {
                starts[count] = start;
                ends[count] = i - 1;
                count++;
            }
            start = i + 1;
        }
    }
    if (start <= b && count < cap) {
        starts[count] = start;
        ends[count] = b;
        count++;
    }
    return count;
}

static void parse_decorator_template_args(Parser *p, int64_t parent, size_t open, size_t close) {
    int64_t tpl = ir_add_node(p->ir, N_DECORATOR_TEMPLATE, open, close, parent);
    size_t starts[128], ends[128], n, i;
    node_set_name(p->ir, tpl, "template_args");
    node_set_value(p->ir, tpl, token_range_text(p, open, close));
    ir_add_child(p->ir, parent, tpl);
    if (close <= open + 1) return;
    n = split_template_args_parser(p, open + 1, close - 1, starts, ends, 128);
    for (i = 0; i < n; ++i) {
        int64_t arg = ir_add_node(p->ir, N_DECORATOR_TEMPLATE_ARG, starts[i], ends[i], tpl);
        node_set_value(p->ir, arg, token_range_text(p, starts[i], ends[i]));
        ir_add_child(p->ir, tpl, arg);
    }
}

static void parse_custom_decorator_template_decl(Parser *p, int64_t parent,
                                                  size_t template_i, size_t template_close) {
    int64_t tpl = ir_add_node(p->ir, N_TEMPLATE, template_i, template_close, parent);
    size_t open = template_i + 1, starts[128], ends[128], n = 0, i;
    node_set_value(p->ir, tpl, token_range_text(p, template_i, template_close));
    ir_add_child(p->ir, parent, tpl);
    while (open < template_close && !str_eq(pt(p, open), "<")) open++;
    if (open >= template_close || template_close <= open + 1) return;
    n = split_template_args_parser(p, open + 1, template_close - 1, starts, ends, 128);
    for (i = 0; i < n; ++i) {
        size_t aa = starts[i], bb = ends[i], j;
        NodeKind k = N_TEMPLATE_NON_TYPE_PARAM;
        const char *name = NULL;
        if (str_eq(pt(p, aa), "typename") || str_eq(pt(p, aa), "class")) k = N_TEMPLATE_TYPE_PARAM;
        else if (str_eq(pt(p, aa), "template")) k = N_TEMPLATE_TEMPLATE_PARAM;
        for (j = bb + 1; j-- > aa;) {
            if (is_identifier(p, j)) { name = pt(p, j); break; }
            if (j == aa) break;
        }
        {
            int64_t pn = ir_add_node(p->ir, k, aa, bb, tpl);
            if (name) node_set_name(p->ir, pn, name);
            node_set_value(p->ir, pn, token_range_text(p, aa, bb));
            if (k == N_TEMPLATE_TYPE_PARAM) node_set_type(p->ir, pn, pt(p, aa));
            ir_add_child(p->ir, tpl, pn);
        }
    }
}

static size_t parse_one_decorator(Parser *p, size_t a, size_t b) {
    size_t i = a + 1, end, name_end;
    char *spelling = NULL, *qualified = NULL;
    if (i > b || !is_decorator_identifier(p, i)) return a;
    end = i;
    while (end + 2 <= b && str_eq(pt(p, end + 1), "::") && is_decorator_identifier(p, end + 2)) end += 2;
    name_end = end;
    if (end + 1 <= b && str_eq(pt(p, end + 1), "<")) {
        size_t tc = find_template_close_cpp(p, end + 1, b + 1);
        if (tc == SIZE_MAX) return a;
        end = tc;
    }
    if (end + 1 <= b && str_eq(pt(p, end + 1), "(")) {
        size_t pc = find_matching(p, end + 1, b + 1);
        if (pc > b) return a;
        end = pc;
    }
    spelling = join_qualified(p, i, name_end);
    qualified = scope_qualified(p, spelling);
    {
        int64_t n = ir_add_node(p->ir, N_DECORATOR, a, end, -1);
        p->ir->nodes[n].aux = UINT64_MAX;
        node_set_name(p->ir, n, spelling);
        node_set_qualified(p->ir, n, qualified);
        node_set_value(p->ir, n, token_range_text(p, a, end));
        if (name_end + 1 <= end && str_eq(pt(p, name_end + 1), "<")) {
            size_t tc = find_template_close_cpp(p, name_end + 1, end + 1);
            if (tc != SIZE_MAX) parse_decorator_template_args(p, n, name_end + 1, tc);
        }
        free(spelling); free(qualified);
        if (str_eq(p->ir->nodes[n].name, "register") || str_eq(p->ir->nodes[n].name, "exposed")) {
            p->ir->nodes[n].flags |= NF_DECORATOR_NATIVE;
            if (str_eq(p->ir->nodes[n].name, "register")) p->ir->nodes[n].flags |= NF_DECORATOR_REGISTER;
            else p->ir->nodes[n].flags |= NF_DECORATOR_EXPOSED;
        }
        p->ir->nodes[n].parent = -1;
        parser_pending_push(p, n);
    }
    return end;
}

static void parse_decorator_sequence(Parser *p, size_t *io_i, size_t b, int64_t parent) {
    size_t i = *io_i;
    (void)parent;
    while (i <= b && str_eq(pt(p, i), "@")) {
        size_t end = parse_one_decorator(p, i, b);
        if (end < i) break;
        i = end + 1;
    }
    *io_i = i;
}

/* ------------------------------------------------------------------------- */
/* Expression recognition                                                     */
/* ------------------------------------------------------------------------- */

static int looks_like_assignment(Parser *p, size_t a, size_t b, size_t *op_out) {
    int par = 0, br = 0, cur = 0;
    size_t i;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")")) { if (par) par--; }
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]")) { if (br) br--; }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}")) { if (cur) cur--; }
        else if ((par | br | cur) == 0 &&
                 (str_eq(t, "=") || str_eq(t, "+=") || str_eq(t, "-=") || str_eq(t, "*=") ||
                  str_eq(t, "/=") || str_eq(t, "%=") || str_eq(t, "&=") || str_eq(t, "|=") ||
                  str_eq(t, "^=") || str_eq(t, "<<=") || str_eq(t, ">>="))) {
            if (op_out) *op_out = i;
            return 1;
        }
    }
    return 0;
}

static size_t find_top_call_paren(Parser *p, size_t a, size_t b) {
    int angle = 0, bracket = 0;
    size_t i;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (str_eq(t, "[")) bracket++;
        else if (str_eq(t, "]") && bracket) bracket--;
        else if (str_eq(t, "(") && angle == 0 && bracket == 0) return i;
    }
    return SIZE_MAX;
}



static void str_trim_ascii(Str *s) {
    size_t a = 0, b = s->len;
    while (a < b && isspace((unsigned char)s->data[a])) a++;
    while (b > a && isspace((unsigned char)s->data[b - 1])) b--;
    if (a) memmove(s->data, s->data + a, b - a);
    s->len = b - a;
    if (s->data) s->data[s->len] = 0;
}

static int find_top_level_token(Parser *p, size_t a, size_t b, const char *want, size_t *out) {
    int par = 0, br = 0, cur = 0, angle = 0;
    size_t i;
    if (a > b) return 0;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(") ) par++;
        else if (str_eq(t, ")")) { if (par) par--; }
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]")) { if (br) br--; }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}")) { if (cur) cur--; }
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (par == 0 && br == 0 && cur == 0 && angle == 0 && str_eq(t, want)) {
            if (out) *out = i;
            return 1;
        }
    }
    return 0;
}

static int call_arg_is_named(Parser *p, size_t a, size_t b, size_t *eq_out) {
    size_t eq;
    if (a > b || !is_identifier(p, a)) return 0;
    if (find_top_level_token(p, a, b, "=", &eq)) {
        if (eq == a || eq != a + 1) return 0;
        if (eq_out) *eq_out = eq;
        return 1;
    }
    return 0;
}

static void parse_call_arguments(Parser *p, int64_t call, size_t a, size_t b) {
    size_t i = a, start = a;
    int par = 0, br = 0, cur = 0, angle = 0;
    while (i <= b) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")")) { if (par) par--; }
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]")) { if (br) br--; }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}")) { if (cur) cur--; }
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;

        if ((str_eq(t, ",") && par == 0 && br == 0 && cur == 0 && angle == 0) || i == b) {
            size_t e = (i == b ? i : i - 1);
            if (start <= e) {
                size_t eq = SIZE_MAX;
                int named = call_arg_is_named(p, start, e, &eq);
                int64_t an = ir_add_node(p->ir, named ? N_NAMED_ARG : N_CALL_ARG, start, e, call);
                p->ir->nodes[an].aux = UINT64_MAX;
                if (named) {
                    node_set_name(p->ir, an, pt(p, start));
                    node_set_qualified(p->ir, an, pt(p, start));
                    {
                        char *av = token_range_text(p, eq + 1, e);
                        Str clean;
                        str_init(&clean);
                        str_put(&clean, av);
                        str_trim_ascii(&clean);
                        node_set_value(p->ir, an, clean.data ? clean.data : "");
                        str_free(&clean);
                        free(av);
                    }
                } else {
                    node_set_value(p->ir, an, token_range_text(p, start, e));
                }
                ir_add_child(p->ir, call, an);
            }
            start = i + 1;
        }
        i++;
    }
}

static int find_property_by_name(const IR *ir, const char *name, int64_t *out_id);

static void add_expr_nodes(Parser *p, int64_t parent, size_t a, size_t b) {
    IR *ir = p->ir;
    size_t i;
    if (a > b || b >= ir->tokens.n) return;

    /* assignment */
    {
        size_t op;
        if (looks_like_assignment(p, a, b, &op)) {
            int64_t n = ir_add_node(ir, N_ASSIGN_EXPR, a, b, parent);
            ir->nodes[n].flags = 1;
            node_set_name(ir, n, pt(p, op));
            node_set_value(ir, n, token_range_text(p, a, b));
            ir_add_child(ir, parent, n);
        }
    }

    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "match")) {
            size_t brace = SIZE_MAX, close = SIZE_MAX, j;
            int mpar = 0, mbr = 0, mangle = 0;
            for (j = i + 1; j <= b; ++j) {
                const char *mt = pt(p, j);
                if (str_eq(mt, "(")) mpar++;
                else if (str_eq(mt, ")") && mpar) mpar--;
                else if (str_eq(mt, "[")) mbr++;
                else if (str_eq(mt, "]") && mbr) mbr--;
                else if (str_eq(mt, "<")) mangle++;
                else if (str_eq(mt, ">") && mangle) mangle--;
                else if (str_eq(mt, "{") && !mpar && !mbr && !mangle) { brace = j; break; }
            }
            if (brace != SIZE_MAX) {
                close = find_matching(p, brace, b + 1);
                if (close <= b) {
                    int64_t mn = ir_add_node(ir, N_MATCH_STMT, i, close, parent);
                    node_set_value(ir, mn, token_range_text(p, i, close));
                    ir->nodes[mn].aux = brace;
                    ir_add_child(ir, parent, mn);
                    i = close;
                    continue;
                }
            }
            die("CXXE: invalid match statement");
        } else if (str_eq(t, "this")) {
            int64_t n = ir_add_node(ir, N_THIS_EXPR, i, i, parent);
            node_set_name(ir, n, "this");
            ir_add_child(ir, parent, n);
        } else if (pk(p, i) == TK_NUMBER || pk(p, i) == TK_STRING || pk(p, i) == TK_CHAR || str_eq(t, "true") || str_eq(t, "false") || str_eq(t, "nullptr")) {
            int64_t n = ir_add_node(ir, N_LITERAL, i, i, parent);
            node_set_value(ir, n, t);
            ir_add_child(ir, parent, n);
        } else if ((str_eq(t, ".") || str_eq(t, "->")) && i + 1 <= b && pk(p, i + 1) == TK_IDENTIFIER) {
            int64_t prop = -1;
            if (i > a && find_property_by_name(ir, pt(p, i + 1), &prop)) {
                int64_t n = ir_add_node(ir, N_PROPERTY_ACCESS, i - 1, i + 1, parent);
                node_set_name(ir, n, pt(p, i + 1));
                node_set_qualified(ir, n, ir->nodes[prop].qualified);
                ir->nodes[n].aux = (uint64_t)prop;
                node_set_value(ir, n, token_range_text(p, i - 1, i + 1));
                ir_add_child(ir, parent, n);
            }
        } else if (is_identifier(p, i)) {
            /* CXXE native member intrinsic: object.get_member("name").
               Represent the whole intrinsic call so the backend can replace it
               with object->name while preserving an optional outer invocation. */
            if (i + 3 <= b && (str_eq(pt(p, i + 1), ".") || str_eq(pt(p, i + 1), "->")) &&
                str_eq(pt(p, i + 2), "get_member") && str_eq(pt(p, i + 3), "(")) {
                size_t close = find_matching(p, i + 3, b + 1);
                if (close <= b) {
                    int64_t n = ir_add_node(ir, N_FUNC_CALL, i, close, parent);
                    node_set_name(ir, n, "get_member");
                    node_set_qualified(ir, n, "get_member");
                    node_set_value(ir, n, token_range_text(p, i, close));
                    if (i + 4 <= close - 1) parse_call_arguments(p, n, i + 4, close - 1);
                    ir_add_child(ir, parent, n);
                    i = close;
                    continue;
                }
            }
            size_t j = i;
            /* qualified identifier a :: b :: c */
            while (j + 2 <= b && str_eq(pt(p, j + 1), "::") && is_word_tok(p, j + 2)) j += 2;
            if (j + 1 <= b && (str_eq(pt(p, j + 1), "(") || str_eq(pt(p, j + 1), "<"))) {
                size_t open = SIZE_MAX;
                if (str_eq(pt(p, j + 1), "(")) open = j + 1;
                else if (str_eq(pt(p, j + 1), "<")) {
                    size_t q = find_matching(p, j + 1, b + 1);
                    if (q + 1 <= b && str_eq(pt(p, q + 1), "(")) open = q + 1;
                }
                if (open != SIZE_MAX) {
                    size_t close = find_matching(p, open, b + 1);
                    if (close <= b) {
                        int64_t n = ir_add_node(ir, N_FUNC_CALL, i, close, parent);
                        char *qname = join_qualified(p, i, j);
                        char *resolved = scope_qualified(p, qname);
                        node_set_name(ir, n, pt(p, j));
                        node_set_qualified(ir, n, qname);
                        node_set_resolved(ir, n, resolved);
                        node_set_value(ir, n, token_range_text(p, i, close));
                        if (open + 1 <= close - 1) parse_call_arguments(p, n, open + 1, close - 1);
                        ir_add_child(ir, parent, n);
                        free(qname);
                        free(resolved);
                        i = close;
                        continue;
                    }
                }
            }
            {
                int64_t n = ir_add_node(ir, N_VAR_REF, i, j, parent);
                char *qname = join_qualified(p, i, j);
                node_set_name(ir, n, pt(p, j));
                node_set_qualified(ir, n, qname);
                node_set_resolved(ir, n, scope_qualified(p, qname));
                ir_add_child(ir, parent, n);
                free(qname);
            }
        }

        if (str_eq(t, ".") || str_eq(t, "->") || str_eq(t, "::")) {
            if (i + 1 <= b && (is_word_tok(p, i + 1) || str_eq(pt(p, i + 1), "operator"))) {
                int64_t n = ir_add_node(ir, N_MEMBER_ACCESS, i, i + 1, parent);
                node_set_name(ir, n, pt(p, i + 1));
                node_set_value(ir, n, t);
                ir_add_child(ir, parent, n);
            }
        }

        if (str_eq(t, "[") && i > a) {
            size_t close = find_matching(p, i, b + 1);
            if (close <= b) {
                int64_t n = ir_add_node(ir, N_INDEX_EXPR, i, close, parent);
                node_set_value(ir, n, token_range_text(p, i, close));
                ir_add_child(ir, parent, n);
                i = close;
            }
        }

        if (str_eq(t, "new")) {
            int64_t n = ir_add_node(ir, N_NEW_EXPR, i, b, parent);
            node_set_value(ir, n, token_range_text(p, i, b));
            ir_add_child(ir, parent, n);
        } else if (str_eq(t, "delete")) {
            int64_t n = ir_add_node(ir, N_DELETE_EXPR, i, b, parent);
            node_set_value(ir, n, token_range_text(p, i, b));
            ir_add_child(ir, parent, n);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Statement/declaration parser                                               */
/* ------------------------------------------------------------------------- */

static NodeKind decl_kind_for_context(int class_context) {
    return class_context ? N_FIELD_DECL : N_VAR_DECL;
}

static void infer_decl_name_type(Parser *p, size_t a, size_t b, int64_t n) {
    size_t eq = SIZE_MAX, semi = b;
    size_t i;
    for (i = a; i <= b; ++i) {
        if (str_eq(pt(p, i), "=") || str_eq(pt(p, i), ":")) { if (eq == SIZE_MAX) eq = i; }
        if (str_eq(pt(p, i), ";")) { semi = i; break; }
    }
    (void)semi;
    for (i = (eq == SIZE_MAX ? b : eq - 1); i >= a; --i) {
        if (is_identifier(p, i)) {
            node_set_name(p->ir, n, pt(p, i));
            node_set_qualified(p->ir, n, scope_qualified(p, pt(p, i)));
            break;
        }
        if (i == a) break;
    }
    if (eq != SIZE_MAX && eq > a) node_set_value(p->ir, n, token_range_text(p, eq + 1, b));
    else node_set_value(p->ir, n, token_range_text(p, a, b));
    {
        size_t name_i = SIZE_MAX;
        for (i = a; i <= b; ++i) if (is_identifier(p, i) && strcmp(pt(p, i), p->ir->nodes[n].name ? p->ir->nodes[n].name : "") == 0) { name_i = i; break; }
        if (name_i != SIZE_MAX && name_i > a) node_set_type(p->ir, n, token_range_text(p, a, name_i - 1));
    }
}

static void parse_parameters(Parser *p, int64_t fn, size_t a, size_t b) {
    size_t i = a, start = a;
    int depth = 0;
    while (i <= b) {
        const char *t = pt(p, i);
        if (str_eq(t, "(") || str_eq(t, "[") || str_eq(t, "{")) depth++;
        else if (str_eq(t, ")") || str_eq(t, "]") || str_eq(t, "}")) { if (depth) depth--; }
        if ((str_eq(t, ",") && depth == 0) || i == b) {
            size_t e = (i == b ? i : i - 1);
            if (start <= e) {
                int64_t pn = ir_add_node(p->ir, N_PARAM_DECL, start, e, fn);
                infer_decl_name_type(p, start, e, pn);
                ir_add_child(p->ir, fn, pn);
            }
            start = i + 1;
        }
        i++;
    }
}

static void parse_region_items(Parser *p, size_t a, size_t b, int64_t parent, int class_context, const char *class_name);

static void add_property_assignment_nodes_in_range(Parser *p, int64_t parent, size_t a, size_t b);

static int parse_function_like(Parser *p, size_t a, size_t b, int64_t parent, const char *class_name, size_t open_paren, int64_t *out_fn) {
    size_t close_paren, k, name_i = SIZE_MAX;
    NodeKind kind = N_FUNCTION;
    char *name = NULL;
    char *qual = NULL;
    int64_t fn;
    close_paren = find_matching(p, open_paren, b + 1);
    if (close_paren > b) return 0;

    /* Find function name immediately before ( or operator spelling. */
    if (open_paren > a) {
        k = open_paren - 1;
        if (is_identifier(p, k) || str_eq(pt(p, k), "operator") || str_eq(pt(p, k), "~")) {
            name_i = k;
        } else if (str_eq(pt(p, k), ">")) {
            int depth = 0;
            while (k > a) {
                if (str_eq(pt(p, k), ">")) depth++;
                else if (str_eq(pt(p, k), "<") && depth) { if (--depth == 0) break; }
                if (depth == 0 && is_identifier(p, k - 1)) { name_i = k - 1; break; }
                if (!k) break;
                k--;
            }
        }
    }
    if (name_i == SIZE_MAX) return 0;
    /* A free-function declaration needs a declaration prefix before its name.
       This prevents ordinary calls such as add(x) from being misclassified. */
    if (!class_name && name_i == a && !str_eq(pt(p, name_i), "operator")) return 0;
    if (!class_name && name_i > a) {
        size_t q;
        for (q = a; q < name_i; ++q) {
            const char *qt = pt(p, q);
            if (str_eq(qt, "=") || str_eq(qt, "+") || str_eq(qt, "-") ||
                str_eq(qt, "/") || str_eq(qt, "%") || str_eq(qt, "!") ||
                str_eq(qt, "?") || str_eq(qt, ".") || str_eq(qt, "->") ||
                str_eq(qt, ",")) return 0;
        }
    }
    if (str_eq(pt(p, name_i), "operator")) kind = N_OPERATOR_FUNCTION;
    else if (class_name && str_eq(pt(p, name_i), class_name)) kind = N_CONSTRUCTOR;
    else if (class_name && str_eq(pt(p, name_i), "~")) kind = N_DESTRUCTOR;
    else if (name_i > a && str_eq(pt(p, name_i - 1), "~")) kind = N_DESTRUCTOR;
    else if (class_name) kind = N_METHOD;

    name = xstrdup0(pt(p, name_i));
    qual = scope_qualified(p, name);
    {
        size_t function_last = b;
        size_t body_open = SIZE_MAX;
        fn = ir_add_node(p->ir, kind, a, b, parent);
        node_set_name(p->ir, fn, name);
        node_set_qualified(p->ir, fn, qual);
        if (kind == N_OPERATOR_FUNCTION) node_set_resolved(p->ir, fn, qual);
        if (name_i > a) node_set_return(p->ir, fn, token_range_text(p, a, name_i - 1));
        parse_parameters(p, fn, open_paren + 1, close_paren - 1);

        /* CXXE accepts both `f(...) {}` and C++ trailing-return syntax
           `auto f(...) -> T {}`.  Locate the body in the latter form too,
           otherwise CXXE-only constructs inside the function (e.g. match)
           would remain in the emitted C++ unchanged. */
        if (close_paren + 1 <= b) {
            if (str_eq(pt(p, close_paren + 1), "{")) {
                body_open = close_paren + 1;
            } else if (str_eq(pt(p, close_paren + 1), "->")) {
                size_t j = close_paren + 2;
                while (j <= b && !str_eq(pt(p, j), "{") && !str_eq(pt(p, j), ";")) j++;
                if (j <= b && str_eq(pt(p, j), "{")) body_open = j;
            }
        }
        if (body_open != SIZE_MAX) {
            size_t close = find_matching(p, body_open, b + 1);
            if (close <= b) {
                function_last = close;
                p->ir->nodes[fn].last_tok = close;
                if (body_open + 1 <= close - 1) {
                    /* Insert property assignment nodes before the generic expression
                       nodes so emission order follows source order when reads and
                       writes overlap the same statement range. */
                    add_property_assignment_nodes_in_range(p, fn, body_open + 1, close - 1);
                    add_expr_nodes(p, fn, body_open + 1, close - 1);
                }
            }
        }
        p->ir->nodes[fn].last_tok = function_last;
        ir_add_child(p->ir, parent, fn);
    }
    if (out_fn) *out_fn = fn;
    free(name); free(qual);
    return 1;
}

static void parse_attributes(Parser *p, size_t a, size_t b, int64_t parent) {
    size_t i = a;
    while (i + 1 <= b) {
        if (str_eq(pt(p, i), "[") && str_eq(pt(p, i + 1), "[")) {
            size_t j = i + 2;
            while (j + 1 <= b && !(str_eq(pt(p, j), "]") && str_eq(pt(p, j + 1), "]"))) j++;
            if (j + 1 <= b) {
                int64_t n = ir_add_node(p->ir, N_ATTRIBUTE, i, j + 1, parent);
                node_set_value(p->ir, n, token_range_text(p, i, j + 1));
                ir_add_child(p->ir, parent, n);
                i = j + 2;
                continue;
            }
        }
        i++;
    }
}

static char *xstrndup0(const char *s, size_t n);

static const char *pp_keyword(const char *s) {
    const char *p = s;
    static char kw[64];
    size_t n = 0;
    while (*p && *p != '#') p++;
    if (*p == '#') p++;
    while (*p && isspace((unsigned char)*p)) p++;
    while (*p && !isspace((unsigned char)*p) && n + 1 < sizeof(kw)) {
        kw[n++] = *p++;
    }
    kw[n] = 0;
    return kw;
}

static NodeKind pp_kind(const char *s) {
    const char *p = pp_keyword(s);
    if (strcmp(p, "include") == 0 || strcmp(p, "include_next") == 0) return N_INCLUDE;
    if (strcmp(p, "define") == 0) return N_DEFINE;
    if (strcmp(p, "undef") == 0) return N_UNDEF;
    if (strcmp(p, "ifdef") == 0) return N_IFDEF;
    if (strcmp(p, "ifndef") == 0) return N_IFNDEF;
    if (strcmp(p, "if") == 0) return N_IF;
    if (strcmp(p, "elif") == 0 || strcmp(p, "elifdef") == 0 || strcmp(p, "elifndef") == 0) return N_ELIF;
    if (strcmp(p, "else") == 0) return N_ELSE;
    if (strcmp(p, "endif") == 0) return N_ENDIF;
    if (strcmp(p, "pragma") == 0) return N_PRAGMA;
    if (strcmp(p, "error") == 0) return N_ERROR_DIRECTIVE;
    if (strcmp(p, "warning") == 0) return N_WARNING_DIRECTIVE;
    if (strcmp(p, "module") == 0) return N_MODULE;
    if (strcmp(p, "import") == 0) return N_IMPORT;
    if (strcmp(p, "export") == 0) return N_EXPORT;
    return N_PREPROCESSOR;
}

static void parse_preprocessor(Parser *p, size_t i, int64_t parent) {
    int64_t n = ir_add_node(p->ir, pp_kind(pt(p, i)), i, i, parent);
    const char *raw = pt(p, i);
    const char *kw = pp_keyword(raw);
    node_set_value(p->ir, n, raw);
    if (strcmp(kw, "include") == 0 || strcmp(kw, "include_next") == 0 || strcmp(kw, "import") == 0) {
        const char *q = raw;
        while (*q && *q != '#') q++;
        if (*q == '#') q++;
        while (*q && isspace((unsigned char)*q)) q++;
        while (*q && !isspace((unsigned char)*q)) q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == '"') {
            const char *e = strchr(q + 1, '"');
            if (e) node_set_name(p->ir, n, xstrndup0(q + 1, (size_t)(e - (q + 1))));
        } else if (*q == '<') {
            const char *e = strchr(q + 1, '>');
            if (e) node_set_name(p->ir, n, xstrndup0(q + 1, (size_t)(e - (q + 1))));
        } else if (*q) {
            const char *e = q;
            while (*e && !isspace((unsigned char)*e)) e++;
            node_set_name(p->ir, n, xstrndup0(q, (size_t)(e - q)));
        }
        if (strcmp(kw, "include_next") == 0) p->ir->nodes[n].flags |= NF_INCLUDE_NEXT;
    } else if (strcmp(kw, "define") == 0) {
        const char *q = raw;
        while (*q && *q != '#') q++;
        if (*q) q++;
        while (*q && isspace((unsigned char)*q)) q++;
        q += 6;
        while (*q && isspace((unsigned char)*q)) q++;
        {
            const char *name_start = q;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
            if (q > name_start) node_set_name(p->ir, n, xstrndup0(name_start, (size_t)(q - name_start)));
        }
    }
    ir_add_child(p->ir, parent, n);
}

static char *xstrndup0(const char *s, size_t n) {
    char *r = (char *)xmalloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static void parse_simple_statement(Parser *p, size_t a, size_t b, int64_t parent) {
    if (a > b) return;
    const char *t = pt(p, a);
    NodeKind k = N_EXPR;
    if (str_eq(t, "return")) k = N_RETURN;
    else if (str_eq(t, "if")) k = N_IF_STMT;
    else if (str_eq(t, "for")) k = N_FOR_STMT;
    else if (str_eq(t, "while")) k = N_WHILE_STMT;
    else if (str_eq(t, "do")) k = N_DO_STMT;
    else if (str_eq(t, "switch")) k = N_SWITCH_STMT;
    else if (str_eq(t, "break")) k = N_BREAK_STMT;
    else if (str_eq(t, "continue")) k = N_CONTINUE_STMT;
    else if (str_eq(t, "goto")) k = N_GOTO_STMT;
    else if (str_eq(t, "throw")) k = N_THROW;
    else if (str_eq(t, "co_return")) k = N_CO_RETURN;
    else if (str_eq(t, "co_await")) k = N_CO_AWAIT;
    else if (str_eq(t, "co_yield")) k = N_CO_YIELD;
    else if (str_eq(t, "try")) k = N_TRY_STMT;
    else if (str_eq(t, "catch")) k = N_CATCH;
    else if (str_eq(t, "new")) k = N_NEW_EXPR;
    else if (str_eq(t, "delete")) k = N_DELETE_EXPR;
    else if (str_eq(t, "static_cast") || str_eq(t, "dynamic_cast") || str_eq(t, "reinterpret_cast") || str_eq(t, "const_cast")) k = N_CAST_EXPR;
    else if (str_eq(t, "sizeof")) k = N_SIZEOF_EXPR;
    else if (str_eq(t, "alignof")) k = N_ALIGNOF_EXPR;
    else if (str_eq(t, "typeid")) k = N_TYPEID_EXPR;
    else if (str_eq(t, "noexcept")) k = N_NOEXCEPT_EXPR;

    {
        int64_t n = ir_add_node(p->ir, k, a, b, parent);
        node_set_value(p->ir, n, token_range_text(p, a, b));
        ir_add_child(p->ir, parent, n);
        add_expr_nodes(p, n, a, b);
    }
}

static void mark_access(IR *ir, int64_t node, int access) {
    ir->nodes[node].flags = (ir->nodes[node].flags & ~NF_ACCESS_MASK) | (uint32_t)access;
}

static int64_t property_child_kind(const IR *ir, const IRNode *prop, NodeKind kind) {
    int64_t c;
    for (c = prop->first_child; c >= 0; c = ir->nodes[c].next_sibling)
        if (ir->nodes[c].kind == kind) return c;
    return -1;
}

static int property_has_get(const IR *ir, const IRNode *prop) { return property_child_kind(ir, prop, N_PROPERTY_GET) >= 0; }
static int property_has_set(const IR *ir, const IRNode *prop) { return property_child_kind(ir, prop, N_PROPERTY_SET) >= 0; }

static int parse_property(Parser *p, size_t a, size_t b, int64_t cls, int access, size_t *end_out) {
    size_t name_i, brace, close, i;
    if (a > b || !str_eq(pt(p, a), "property") || a + 1 > b || !is_identifier(p, a + 1)) return 0;
    name_i = a + 1;
    brace = name_i + 1;
    if (brace > b || !str_eq(pt(p, brace), "{")) return 0;
    close = find_matching(p, brace, b + 1);
    if (close > b) return 0;
    {
        int64_t prop = ir_add_node(p->ir, N_PROPERTY, a, close, cls);
        node_set_name(p->ir, prop, pt(p, name_i));
        node_set_qualified(p->ir, prop, scope_qualified(p, pt(p, name_i)));
        node_set_value(p->ir, prop, token_range_text(p, a, close));
        mark_access(p->ir, prop, access);
        ir_add_child(p->ir, cls, prop);
        for (i = brace + 1; i < close; ) {
            if ((str_eq(pt(p, i), "get") || str_eq(pt(p, i), "set")) && i + 1 < close && str_eq(pt(p, i + 1), "{")) {
                size_t oc = find_matching(p, i + 1, close + 1);
                if (oc <= close) {
                    NodeKind nk = str_eq(pt(p, i), "get") ? N_PROPERTY_GET : N_PROPERTY_SET;
                    int64_t child = ir_add_node(p->ir, nk, i, oc, prop);
                    node_set_name(p->ir, child, pt(p, i));
                    node_set_value(p->ir, child, token_range_text(p, i, oc));
                    ir_add_child(p->ir, prop, child);
                    i = oc + 1;
                    continue;
                }
            }
            i++;
        }
        if (!property_has_get(p->ir, &p->ir->nodes[prop]) && !property_has_set(p->ir, &p->ir->nodes[prop]))
            die("property '%s' must contain at least a get or set block", pt(p, name_i));
        parser_apply_pending(p, prop);
        if (end_out) *end_out = close;
        return 1;
    }
}

static int find_property_by_name(const IR *ir, const char *name, int64_t *out_id) {
    size_t i;
    if (!name || !*name) return 0;

    /* Property names are case-sensitive and must be matched literally.
       There is intentionally no Health -> health (or reverse) fallback:
       a property and its backing field are a pair only when their declared
       names are exactly identical. */
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (n->kind == N_PROPERTY && n->name && strcmp(n->name, name) == 0) {
            if (out_id) *out_id = (int64_t)i;
            return 1;
        }
    }
    return 0;
}

static int find_field_in_class_by_name(const IR *ir, int64_t cls, const char *name, int64_t *out_id) {
    int64_t c;
    if (cls < 0 || (size_t)cls >= ir->node_count || !name || !*name) return 0;
    for (c = ir->nodes[cls].first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *n = &ir->nodes[c];
        if (n->kind == N_FIELD_DECL && n->name && strcmp(n->name, name) == 0) {
            if (out_id) *out_id = c;
            return 1;
        }
    }
    return 0;
}

static void validate_class_properties(const IR *ir, int64_t cls) {
    int64_t c;
    if (cls < 0 || (size_t)cls >= ir->node_count) return;
    for (c = ir->nodes[cls].first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *prop = &ir->nodes[c];
        int64_t field = -1;
        if (prop->kind != N_PROPERTY || !prop->name || !*prop->name) continue;
        if (!find_field_in_class_by_name(ir, cls, prop->name, &field)) {
            die("CXXE: property '%s' must have a variable with the exact same name in the same type", prop->name);
        }
    }
}

static void add_property_assignment_nodes_in_range(Parser *p, int64_t parent, size_t a, size_t b) {
    size_t start=a,i;
    int par=0,br=0,cur=0;
    for(i=a;i<=b;++i){
        const char*t=pt(p,i);
        if(!strcmp(t,"("))par++; else if(!strcmp(t,")")&&par)par--; else if(!strcmp(t,"["))br++; else if(!strcmp(t,"]")&&br)br--; else if(!strcmp(t,"{"))cur++; else if(!strcmp(t,"}")&&cur)cur--;
        if((!par&&!br&&!cur&&strcmp(t,";")==0) || (i==b)){
            size_t end = (i==b && strcmp(t,";")!=0) ? i : i;
            if(start<=end){
                size_t op=SIZE_MAX;
                if(looks_like_assignment(p,start,end,&op) && op>start && is_identifier(p,op-1)){
                    size_t pn=op-1;
                    int64_t pid=-1;
                    if(find_property_by_name(p->ir,pt(p,pn),&pid) && pn>start &&
                       (str_eq(pt(p,pn-1),".")||str_eq(pt(p,pn-1),"->"))){
                        int64_t n=ir_add_node(p->ir,N_ASSIGN_EXPR,start,end,parent);
                        node_set_name(p->ir,n,pt(p,op));
                        node_set_value(p->ir,n,token_range_text(p,start,end));
                        ir_add_child(p->ir,parent,n);
                    }
                }
            }
            start=i+1;
        }
    }
}


static void parse_class_body(Parser *p, size_t a, size_t b, int64_t cls, const char *class_name) {
    size_t i = a, start = a;
    int access = (p->ir->nodes[cls].kind == N_CLASS) ? NF_ACCESS_PRIVATE : NF_ACCESS_PUBLIC;

    while (i <= b) {
        if (str_eq(pt(p, i), "public") || str_eq(pt(p, i), "protected") || str_eq(pt(p, i), "private")) {
            if (i + 1 <= b && str_eq(pt(p, i + 1), ":") && start == i) {
                if (str_eq(pt(p, i), "public")) access = NF_ACCESS_PUBLIC;
                else if (str_eq(pt(p, i), "protected")) access = NF_ACCESS_PROTECTED;
                else access = NF_ACCESS_PRIVATE;
                i += 2;
                start = i;
                continue;
            }
        }

        if (str_eq(pt(p, i), "@")) {
            if (start < i) {
                /* An incomplete previous declaration: keep it losslessly. */
                int64_t raw = ir_add_node(p->ir, N_RAW, start, i - 1, cls);
                node_set_value(p->ir, raw, token_range_text(p, start, i - 1));
                mark_access(p->ir, raw, access);
                ir_add_child(p->ir, cls, raw);
                parser_apply_pending(p, raw);
            }
            parse_decorator_sequence(p, &i, b, cls);
            start = i;
            continue;
        }

        if (str_eq(pt(p, i), "property") && start == i) {
            size_t property_end = SIZE_MAX;
            if (parse_property(p, i, b, cls, access, &property_end)) {
                i = property_end + 1;
                start = i;
                continue;
            }
        }

        if (str_eq(pt(p, i), ";")) {
            if (start <= i) {
                size_t open = find_top_call_paren(p, start, i);
                int64_t n;
                n = -1;
                if (open != SIZE_MAX && parse_function_like(p, start, i, cls, class_name, open, &n)) {
                    if (n >= 0) mark_access(p->ir, n, access);
                    if (n >= 0) parser_apply_pending(p, n);
                } else {
                    n = ir_add_node(p->ir, N_FIELD_DECL, start, i, cls);
                    infer_decl_name_type(p, start, i, n);
                    mark_access(p->ir, n, access);
                    ir_add_child(p->ir, cls, n);
                    parser_apply_pending(p, n);
                }
            }
            start = i + 1;
        } else if (str_eq(pt(p, i), "{") && start < i) {
            /* Function definition: parse_function_like knows how to consume its body. */
            size_t open_paren = find_top_call_paren(p, start, i);
            if (open_paren != SIZE_MAX) {
                size_t e = find_stmt_end(p, start, b + 1);
                int64_t n = -1;
                if (parse_function_like(p, start, e, cls, class_name, open_paren, &n) && n >= 0) {
                    mark_access(p->ir, n, access);
                    parser_apply_pending(p, n);
                    i = e;
                    start = e + 1;
                }
            }
        }
        i++;
    }

    if (start <= b) {
        int64_t raw = ir_add_node(p->ir, N_RAW, start, b, cls);
        node_set_value(p->ir, raw, token_range_text(p, start, b));
        mark_access(p->ir, raw, access);
        ir_add_child(p->ir, cls, raw);
        parser_apply_pending(p, raw);
    }
}

static void parse_class(Parser *p, size_t a, size_t b, int64_t parent, NodeKind kind) {
    size_t i = a + 1, name_i = SIZE_MAX, brace = SIZE_MAX, close = SIZE_MAX;
    char *name = NULL, *qname = NULL;
    while (i <= b && i < p->end) {
        if (is_identifier(p, i) && name_i == SIZE_MAX) name_i = i;
        if (str_eq(pt(p, i), "{")) { brace = i; break; }
        i++;
    }
    if (brace == SIZE_MAX) {
        int64_t n = ir_add_node(p->ir, kind, a, b, parent);
        mark_access(p->ir, n, kind == N_CLASS ? NF_ACCESS_PRIVATE : NF_ACCESS_PUBLIC);
        node_set_value(p->ir, n, token_range_text(p, a, b));
        if (name_i != SIZE_MAX) {
            name = xstrdup0(pt(p, name_i));
            qname = scope_qualified(p, name);
            node_set_name(p->ir, n, name);
            node_set_qualified(p->ir, n, qname);
        }
        ir_add_child(p->ir, parent, n);
        parser_apply_pending(p, n);
        free(name); free(qname);
        return;
    }
    close = find_matching(p, brace, b + 1);
    if (close > b) close = b;
    {
        int64_t n = ir_add_node(p->ir, kind, a, close, parent);
        mark_access(p->ir, n, kind == N_CLASS ? NF_ACCESS_PRIVATE : NF_ACCESS_PUBLIC);
        if (name_i != SIZE_MAX) {
            name = xstrdup0(pt(p, name_i));
            qname = scope_qualified(p, name);
            node_set_name(p->ir, n, name);
            node_set_qualified(p->ir, n, qname);
            scope_push(p, name);
        }
        node_set_value(p->ir, n, token_range_text(p, a, close));
        ir_add_child(p->ir, parent, n);
        parser_apply_pending(p, n);
        if (name_i != SIZE_MAX) {
            parse_class_body(p, brace + 1, close ? close - 1 : close, n, name);
            validate_class_properties(p->ir, n);
        }
        if (name_i != SIZE_MAX) scope_pop(p);
    }
    free(name); free(qname);
}

static void parse_enum(Parser *p, size_t a, size_t b, int64_t parent) {
    size_t i = a + 1, brace = SIZE_MAX, name_i = SIZE_MAX;
    while (i <= b) {
        if (is_identifier(p, i) && name_i == SIZE_MAX) name_i = i;
        if (str_eq(pt(p, i), "{")) { brace = i; break; }
        i++;
    }
    if (brace == SIZE_MAX) return;
    size_t close = find_matching(p, brace, b + 1);
    if (close > b) close = b;
    int64_t en = ir_add_node(p->ir, N_ENUM, a, close, parent);
    if (name_i != SIZE_MAX) {
        node_set_name(p->ir, en, pt(p, name_i));
        node_set_qualified(p->ir, en, scope_qualified(p, pt(p, name_i)));
    }
    ir_add_child(p->ir, parent, en);
    size_t s = brace + 1;
    for (i = brace + 1; i <= close; ++i) {
        if (str_eq(pt(p, i), ",") || i == close) {
            if (s < i) {
                int64_t e = ir_add_node(p->ir, N_ENUMERATOR, s, i == close ? i - 1 : i - 1, en);
                if (is_identifier(p, s)) node_set_name(p->ir, e, pt(p, s));
                node_set_value(p->ir, e, token_range_text(p, s, i - 1));
                ir_add_child(p->ir, en, e);
            }
            s = i + 1;
        }
    }
}

static int looks_like_var_decl(Parser *p, size_t a, size_t b) {
    size_t i;
    if (a > b) return 0;
    if (str_eq(pt(p, a), "return") || str_eq(pt(p, a), "if") || str_eq(pt(p, a), "for") || str_eq(pt(p, a), "while") || str_eq(pt(p, a), "switch") || str_eq(pt(p, a), "throw")) return 0;
    for (i = a; i <= b; ++i) {
        if (str_eq(pt(p, i), "=") || str_eq(pt(p, i), ";")) continue;
        if (pk(p, i) == TK_IDENTIFIER || pk(p, i) == TK_PUNCT) {
            if (i > a && is_identifier(p, i) && (str_eq(pt(p, i - 1), "*") || str_eq(pt(p, i - 1), "&") || is_keyword(pt(p, i - 1)))) return 1;
        }
    }
    if (is_keyword(pt(p, a)) || str_eq(pt(p, a), "dynamic")) return 1;
    return 0;
}


/* ------------------------------------------------------------------------- */
/* Custom decorator declarations                                             */
/*                                                                           */
/* Syntax:                                                                   */
/*   void @my_decorator(T x) -> func(int a) { before(x); func(a); }         */
/*                                                                           */
/* The declaration itself is a CXXE-only construct and disappears from the  */
/* generated C++. At use sites (@my_decorator(...)), the decorator body is  */
/* instantiated around the decorated function.                              */
/* ------------------------------------------------------------------------- */

static int find_top_level_at(Parser *p, size_t a, size_t b, size_t *at_out) {
    int par = 0, br = 0, cur = 0, angle = 0;
    size_t i;
    for (i = a; i <= b; ++i) {
        const char *t = pt(p, i);
        if (str_eq(t, "(")) par++;
        else if (str_eq(t, ")") && par) par--;
        else if (str_eq(t, "[")) br++;
        else if (str_eq(t, "]") && br) br--;
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}") && cur) cur--;
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (str_eq(t, "@") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            if (at_out) *at_out = i;
            return 1;
        }
    }
    return 0;
}


static int looks_like_custom_decorator_decl(Parser *p, size_t a, size_t b, int64_t parent, size_t *end_out) {
    size_t at = SIZE_MAX, name_i, params_open, params_close, func_i, target_open, target_close, body_open, body_close;
    size_t i;
    int par = 0, br = 0, cur = 0, angle = 0;
    (void)parent;
    if (a > b) return 0;
    /* Locate a top-level @ before the statement ends. */
    if (!find_top_level_at(p, a, b, &at) || at <= a || at + 1 > b) return 0;
    name_i = at + 1;
    if (!is_decorator_identifier(p, name_i)) return 0;
    i = name_i + 1;
    params_open = SIZE_MAX;
    if (i <= b && str_eq(pt(p, i), "(")) {
        params_open = i;
        params_close = find_matching(p, params_open, b + 1);
        if (params_close > b) return 0;
        i = params_close + 1;
    } else {
        params_close = SIZE_MAX;
    }
    if (i > b || !str_eq(pt(p, i), "->")) return 0;
    i++;
    if (i > b || !str_eq(pt(p, i), "func")) return 0;
    func_i = i;
    i++;
    if (i > b || !str_eq(pt(p, i), "(")) return 0;
    target_open = i;
    target_close = find_matching(p, target_open, b + 1);
    if (target_close > b) return 0;
    i = target_close + 1;
    while (i <= b && (str_eq(pt(p, i), "const") || str_eq(pt(p, i), "noexcept") || str_eq(pt(p, i), "&") || str_eq(pt(p, i), "&&") || str_eq(pt(p, i), "requires"))) i++;
    if (i > b || !str_eq(pt(p, i), "{")) return 0;
    body_open = i;
    body_close = find_matching(p, body_open, b + 1);
    if (body_close > b) return 0;
    if (end_out) *end_out = body_close;
    (void)cur; (void)par; (void)br; (void)angle; (void)func_i;
    return 1;
}

static int parse_custom_decorator_decl(Parser *p, size_t a, size_t b, int64_t parent) {
    size_t at = SIZE_MAX, name_i, name_end, params_open, params_close, target_open, target_close, body_open, body_close, i;
    size_t template_i = SIZE_MAX, template_close = SIZE_MAX, decl_start = a;
    if (!find_top_level_at(p, a, b, &at) || at <= a || at + 1 > b) return 0;
    if (str_eq(pt(p, a), "template")) {
        template_i = a;
        {
            size_t open = a + 1;
            while (open < at && !str_eq(pt(p, open), "<")) open++;
            if (open >= at) return 0;
            template_close = find_template_close_cpp(p, open, at);
            if (template_close == SIZE_MAX || template_close + 1 >= at) return 0;
            decl_start = template_close + 1;
        }
    }
    name_i = at + 1;
    if (!is_decorator_identifier(p, name_i)) return 0;
    name_end = name_i;
    while (name_end + 2 <= b && str_eq(pt(p, name_end + 1), "::") && is_decorator_identifier(p, name_end + 2)) name_end += 2;
    i = name_end + 1;
    params_open = params_close = SIZE_MAX;
    if (i <= b && str_eq(pt(p, i), "(")) {
        params_open = i;
        params_close = find_matching(p, i, b + 1);
        if (params_close > b) return 0;
        i = params_close + 1;
    }
    if (i > b || !str_eq(pt(p, i), "->")) return 0;
    if (i + 2 > b || !str_eq(pt(p, i + 1), "func") || !str_eq(pt(p, i + 2), "(")) return 0;
    target_open = i + 2;
    target_close = find_matching(p, target_open, b + 1);
    if (target_close > b) return 0;
    i = target_close + 1;
    while (i <= b && (str_eq(pt(p, i), "const") || str_eq(pt(p, i), "noexcept") || str_eq(pt(p, i), "&") || str_eq(pt(p, i), "&&") || str_eq(pt(p, i), "requires"))) i++;
    if (i > b || !str_eq(pt(p, i), "{")) return 0;
    body_open = i;
    body_close = find_matching(p, body_open, b + 1);
    if (body_close > b) return 0;

    {
        int64_t n = ir_add_node(p->ir, N_CUSTOM_DECORATOR, a, body_close, parent);
        Str rt; str_init(&rt);
        str_put(&rt, token_range_text(p, decl_start, at - 1));
        str_trim_ascii(&rt);
        node_set_return(p->ir, n, rt.data ? rt.data : "");
        str_free(&rt);
        {
            char *decorator_name = join_qualified(p, name_i, name_end);
            char *decorator_qualified = scope_qualified(p, decorator_name);
            node_set_name(p->ir, n, decorator_name);
            node_set_qualified(p->ir, n, decorator_qualified);
            free(decorator_name); free(decorator_qualified);
        }
        node_set_value(p->ir, n, token_range_text(p, a, body_close));
        ir_add_child(p->ir, parent, n);
        if (template_i != SIZE_MAX) parse_custom_decorator_template_decl(p, n, template_i, template_close);
        if (params_open != SIZE_MAX && params_close > params_open + 1) parse_parameters(p, n, params_open + 1, params_close - 1);
        {
            int64_t target = ir_add_node(p->ir, N_DECORATOR_TARGET, target_open - 1, target_close, n);
            node_set_name(p->ir, target, "func");
            node_set_value(p->ir, target, token_range_text(p, target_open - 1, target_close));
            ir_add_child(p->ir, n, target);
            if (target_close == target_open + 2 && str_eq(pt(p, target_open + 1), "...")) p->ir->nodes[target].flags |= NF_DECORATOR_VARIADIC;
            else if (target_close > target_open + 1) parse_parameters(p, target, target_open + 1, target_close - 1);
        }
        return 1;
    }
}

static void parse_region_items(Parser *p, size_t a, size_t b, int64_t parent, int class_context, const char *class_name) {
    size_t i = a;
    while (i <= b) {
        if (pk(p, i) == TK_PP) {
            parse_preprocessor(p, i, parent);
            i++;
            continue;
        }

        if (str_eq(pt(p, i), "@")) {
            parse_decorator_sequence(p, &i, b, parent);
            continue;
        }

        /* CXXE custom decorator declaration: return_type @name(...) -> func(...) { ... } */
        {
            size_t custom_end = SIZE_MAX;
            if (looks_like_custom_decorator_decl(p, i, b, parent, &custom_end)) {
                if (parse_custom_decorator_decl(p, i, custom_end, parent)) {
                    i = custom_end + 1;
                    continue;
                }
            }
        }

        if (str_eq(pt(p, i), "[[")) {
            parse_attributes(p, i, b, parent);
            i++;
            continue;
        }

        if (str_eq(pt(p, i), "template")) {
            size_t j = i + 1;
            int depth = 0;
            while (j <= b) {
                if (str_eq(pt(p, j), "<")) depth++;
                else if (str_eq(pt(p, j), ">") && depth) { if (--depth == 0) break; }
                j++;
            }
            if (j <= b) {
                size_t e = find_stmt_end(p, j + 1, b + 1);
                int64_t n = ir_add_node(p->ir, N_TEMPLATE, i, e, parent);
                node_set_value(p->ir, n, token_range_text(p, i, e));
                ir_add_child(p->ir, parent, n);
                i = e + 1;
                continue;
            }
        }

        if (str_eq(pt(p, i), "namespace")) {
            size_t name_i = (i + 1 <= b && is_identifier(p, i + 1)) ? i + 1 : SIZE_MAX;
            size_t brace = i + 1;
            while (brace <= b && !str_eq(pt(p, brace), "{")) brace++;
            if (brace <= b) {
                size_t close = find_matching(p, brace, b + 1);
                int64_t n = ir_add_node(p->ir, N_NAMESPACE, i, close <= b ? close : b, parent);
                if (name_i != SIZE_MAX) {
                    node_set_name(p->ir, n, pt(p, name_i));
                    node_set_qualified(p->ir, n, scope_qualified(p, pt(p, name_i)));
                    scope_push(p, pt(p, name_i));
                }
                node_set_value(p->ir, n, token_range_text(p, i, close <= b ? close : b));
                ir_add_child(p->ir, parent, n);
                if (close > brace) parse_region_items(p, brace + 1, close - 1, n, 0, NULL);
                if (name_i != SIZE_MAX) scope_pop(p);
                i = (close <= b ? close + 1 : b + 1);
                continue;
            }
        }

        if (str_eq(pt(p, i), "class") || str_eq(pt(p, i), "struct") || str_eq(pt(p, i), "union")) {
            NodeKind k = str_eq(pt(p, i), "class") ? N_CLASS : (str_eq(pt(p, i), "struct") ? N_STRUCT : N_UNION);
            size_t e = find_stmt_end(p, i, b + 1);
            size_t brace = i;
            while (brace <= b && !str_eq(pt(p, brace), "{")) brace++;
            if (brace <= b) {
                size_t close = find_matching(p, brace, b + 1);
                parse_class(p, i, close <= b ? close : e, parent, k);
                /* parse_class consumes and applies pending decorators itself. */
                i = (close <= b ? close + 1 : e + 1);
                continue;
            }
        }

        if (str_eq(pt(p, i), "enum")) {
            size_t e = find_stmt_end(p, i, b + 1);
            {
                size_t before = p->ir->node_count;
                parse_enum(p, i, e, parent);
                if (p->ir->node_count > before) parser_apply_pending(p, (int64_t)before);
            }
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "typedef")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_TYPEDEF, i, e, parent);
            infer_decl_name_type(p, i, e, n);
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "using")) {
            size_t e = find_stmt_end(p, i, b + 1);
            NodeKind k = N_USING_DECL;
            size_t j;
            for (j = i + 1; j <= e; ++j) if (str_eq(pt(p, j), "namespace")) k = N_USING_DIRECTIVE;
            int64_t n = ir_add_node(p->ir, k, i, e, parent);
            if (i + 1 <= e && is_identifier(p, i + 1)) node_set_name(p->ir, n, pt(p, i + 1));
            node_set_value(p->ir, n, token_range_text(p, i, e));
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "concept")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_CONCEPT, i, e, parent);
            if (i + 1 <= e && is_identifier(p, i + 1)) node_set_name(p->ir, n, pt(p, i + 1));
            node_set_value(p->ir, n, token_range_text(p, i, e));
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "friend")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_FRIEND, i, e, parent);
            node_set_value(p->ir, n, token_range_text(p, i, e));
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        /* Find a top-level (' in this declaration/statement. */
        {
            size_t e = find_stmt_end(p, i, b + 1);
            size_t open = find_top_call_paren(p, i, e);
            if (open != SIZE_MAX) {
                /* A function definition/declaration is recognizable by { or ; after ). */
                size_t close = find_matching(p, open, e + 1);
                size_t after = close < e ? close + 1 : close;
                int functionish = 0;
                if (after <= e && (str_eq(pt(p, after), "{") || str_eq(pt(p, after), ";") || str_eq(pt(p, after), "const") || str_eq(pt(p, after), "noexcept") || str_eq(pt(p, after), "->") || str_eq(pt(p, after), "override") || str_eq(pt(p, after), "final"))) functionish = 1;
                if (functionish) {
                    int64_t fn_id = -1;
                    size_t function_end = e;
                    /* find_stmt_end() deliberately stops at the first top-level
                       semicolon, which is not enough for a function body.  Extend
                       the parser range to the matching closing brace so CXXE-only
                       statements (such as match) inside the function are seen. */
                    if (after <= e && str_eq(pt(p, after), "{")) {
                        size_t body_close = find_matching(p, after, b + 1);
                        if (body_close <= b) function_end = body_close;
                    }
                    if (parse_function_like(p, i, function_end, parent, class_name, open, &fn_id)) {
                        if (fn_id >= 0) parser_apply_pending(p, fn_id);
                        if (fn_id >= 0) i = (size_t)p->ir->nodes[fn_id].last_tok + 1;
                        else i = function_end + 1;
                        continue;
                    }
                }
            }

            /* Control statements / blocks. */
            if (str_eq(pt(p, i), "if") || str_eq(pt(p, i), "for") || str_eq(pt(p, i), "while") || str_eq(pt(p, i), "switch") || str_eq(pt(p, i), "catch")) {
                size_t open = i + 1;
                while (open <= e && !str_eq(pt(p, open), "(")) open++;
                if (open <= e) {
                    size_t c = find_matching(p, open, e + 1);
                    size_t body = c + 1;
                    if (body <= e && str_eq(pt(p, body), "{")) {
                        size_t bc = find_matching(p, body, e + 1);
                        NodeKind sk = str_eq(pt(p, i), "if") ? N_IF_STMT : str_eq(pt(p, i), "for") ? N_FOR_STMT : str_eq(pt(p, i), "while") ? N_WHILE_STMT : str_eq(pt(p, i), "switch") ? N_SWITCH_STMT : N_CATCH;
                        int64_t n = ir_add_node(p->ir, sk, i, bc <= e ? bc : e, parent);
                        node_set_value(p->ir, n, token_range_text(p, i, bc <= e ? bc : e));
                        ir_add_child(p->ir, parent, n);
                        if (bc > body) parse_region_items(p, body + 1, bc - 1, n, 0, NULL);
                        i = (bc <= e ? bc + 1 : e + 1);
                        continue;
                    }
                }
            }

            /* Generic declaration / expression. */
            if (looks_like_var_decl(p, i, e)) {
                int64_t n = ir_add_node(p->ir, decl_kind_for_context(class_context), i, e, parent);
                infer_decl_name_type(p, i, e, n);
                ir_add_child(p->ir, parent, n);
                parser_apply_pending(p, n);
                if (looks_like_assignment(p, i, e, NULL)) {
                    int64_t a = ir_add_node(p->ir, N_VAR_ASSIGN, i, e, n);
                    node_set_value(p->ir, a, token_range_text(p, i, e));
                    ir_add_child(p->ir, n, a);
                }
            } else {
                size_t before = p->ir->node_count;
                parse_simple_statement(p, i, e, parent);
                if (p->ir->node_count > before) parser_apply_pending(p, (int64_t)before);
            }
            i = e + 1;
        }
    }
}

static void parse_source(IR *ir) {
    Parser p;
    int64_t root;
    memset(&p, 0, sizeof(p));
    p.ir = ir;
    p.end = ir->tokens.n ? ir->tokens.n - 1 : 0;
    root = ir_add_node(ir, N_TRANSLATION_UNIT, 0, p.end, -1);
    p.root = root;
    parse_region_items(&p, 0, p.end ? p.end - 1 : 0, root, 0, NULL);
    while (p.scope_n) scope_pop(&p);
    free(p.pending_decorators);
    free(p.scope);
}

/* ------------------------------------------------------------------------- */
/* Include/decorator discovery                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    char **visited;
    size_t visited_n, visited_cap;
    char **include_dirs;
    size_t include_dir_n, include_dir_cap;
} IncludeContext;

/* The CXXE standard headers are installed next to cxxe.exe: <exe>/stde.
   Keep the resolved executable directory globally so every parse/roundtrip
   automatically gets the same standard include search path without requiring
   an explicit -I argument. */
static char *g_cxxe_executable_dir = NULL;

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

static char *path_dirname0(const char *path);
static char *normalize_path0(const char *path);

static char *cxxe_executable_dir0(const char *argv0) {
    char buf[32768];
#ifdef _WIN32
    {
        DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf));
        if (len > 0 && len < sizeof(buf)) {
            buf[len] = 0;
            {
                char *dir = path_dirname0(buf);
                char *norm = normalize_path0(dir);
                free(dir);
                return norm;
            }
        }
    }
#else
    {
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = 0;
            {
                char *dir = path_dirname0(buf);
                char *norm = normalize_path0(dir);
                free(dir);
                return norm;
            }
        }
    }
#endif

    /* Fallback for platforms without a reliable executable-path API. */
    if (argv0 && *argv0) {
        int has_sep = strchr(argv0, '/') != NULL || strchr(argv0, '\\') != NULL;
        if (has_sep) {
            char *dir = path_dirname0(argv0);
            char *norm = normalize_path0(dir);
            free(dir);
            return norm;
        }
    }

    return xstrdup0(".");
}

static char *path_dirname0(const char *path) {
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = a ? (b ? (a > b ? a : b) : a) : b;
    if (!p) return xstrdup0(".");
    if (p == path) return xstrndup0(path, 1);
    return xstrndup0(path, (size_t)(p - path));
}

static char *path_join0(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    Str s; str_init(&s);
    str_putn(&s, dir, a);
    if (a && dir[a - 1] != '/' && dir[a - 1] != '\\') str_ch(&s, '/');
    str_putn(&s, name, b);
    return str_take(&s);
}

static char *normalize_path0(const char *path) {
    Str out; size_t i = 0, n = strlen(path); int absolute = 0;
    char **parts = NULL; size_t pn = 0, pc = 0;
    char *tmp = xstrdup0(path), *p = tmp;
    str_init(&out);
    for (i = 0; i < n; ++i) if (tmp[i] == '\\') tmp[i] = '/';
    if (tmp[0] == '/' || (isalpha((unsigned char)tmp[0]) && tmp[1] == ':' && tmp[2] == '/')) absolute = 1;
    while (*p) {
        char *start;
        while (*p == '/') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (pn && strcmp(parts[pn - 1], "..") != 0) { free(parts[--pn]); }
            else if (!absolute) {
                if (pn == pc) { pc = pc ? pc * 2 : 8; parts = (char **)xrealloc(parts, pc * sizeof(*parts)); }
                parts[pn++] = xstrdup0("..");
            }
            continue;
        }
        if (pn == pc) { pc = pc ? pc * 2 : 8; parts = (char **)xrealloc(parts, pc * sizeof(*parts)); }
        parts[pn++] = xstrdup0(start);
    }
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':') {
        str_ch(&out, tmp[0]); str_ch(&out, ':');
        if (absolute) str_ch(&out, '/');
    } else if (absolute) str_ch(&out, '/');
    for (i = 0; i < pn; ++i) {
        if (i) str_ch(&out, '/');
        str_put(&out, parts[i]);
        free(parts[i]);
    }
    free(parts); free(tmp);
    if (out.len == 0) str_put(&out, absolute ? "/" : ".");
    return str_take(&out);
}

static int file_exists0(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
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
    return strcmp(dot, ".h") == 0 || strcmp(dot, ".hpp") == 0 || strcmp(dot, ".he") == 0 ||
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

static void import_custom_decorators(IR *dst, const IR *src) {
    size_t i;
    for (i = 0; i < src->node_count; ++i) {
        const IRNode *orig = &src->nodes[i];
        int64_t n;
        if (orig->kind != N_CUSTOM_DECORATOR) continue;
        n = clone_ir_subtree(dst, src, (int64_t)i, 0, NF_EXTERNAL_DECL);
        ir_add_child(dst, 0, n);
    }
}

static void discover_includes_recursive(IR *dst, const char *current_file, IncludeContext *ctx);

static void discover_one_include(IR *dst, const char *current_file, const char *raw, IncludeContext *ctx, IRNode *include_node) {
    char *name = NULL, *resolved = NULL;
    int angled = 0, next = 0;
    IR hdr;
    if (!parse_include_spec0(raw, &name, &angled, &next)) return;
    (void)next;
    resolved = resolve_include_path0(current_file, name, angled, ctx);
    if (!resolved) {
        if (!angled) die("cannot resolve include \"%s\" included from '%s'", name ? name : "", current_file);
        free(name);
        return; /* system/SDK angle include: intentionally opaque to CXXE */
    }
    free(name);
    if (include_node) node_set_resolved(dst, (int64_t)(include_node - dst->nodes), resolved);
    if (include_visited(ctx, resolved)) { free(resolved); return; }
    include_mark_visited(ctx, resolved);
    if (!header_extension_candidate0(resolved)) { free(resolved); return; }
    ir_init(&hdr);
    hdr.source = read_bytes(resolved, &hdr.source_size);
    lex_cpp(hdr.source, hdr.source_size, &hdr.tokens);
    parse_source(&hdr);
    discover_includes_recursive(&hdr, resolved, ctx);
    import_custom_decorators(dst, &hdr);
    ir_free(&hdr);
    free(resolved);
}

static void discover_includes_recursive(IR *dst, const char *current_file, IncludeContext *ctx) {
    size_t i;
    for (i = 0; i < dst->node_count; ++i) {
        const IRNode *n = &dst->nodes[i];
        if ((n->kind != N_INCLUDE && n->kind != N_IMPORT) || !n->value) continue;
        discover_one_include(dst, current_file, n->value, ctx, (IRNode *)n);
    }
}

static void discover_included_decorators(IR *ir, const char *main_path, const char *const *include_dirs, size_t include_dir_n) {
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


/* ------------------------------------------------------------------------- */
/* Exact source reconstruction                                                */
/* ------------------------------------------------------------------------- */




static int get_member_pattern(const IR *ir, const IRNode *n, const char **member, size_t *member_tok, size_t *close_tok) {
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

static int custom_decorator_template_param_ids(const IR *ir, const IRNode *cd, int64_t *ids, size_t cap);
static int decorator_template_arg_ranges(const IR *ir, const IRNode *dec, size_t *starts, size_t *ends, size_t cap);
static size_t custom_decorator_template_param_count(const IR *ir, const IRNode *cd);

static void resolve_native_features(IR *ir) {
    size_t i;
    /* Decorators become semantic flags on their targets. */
    for (i = 0; i < ir->node_count; ++i) {
        IRNode *d = &ir->nodes[i];
        if (d->kind != N_DECORATOR || d->aux == UINT64_MAX || d->aux >= ir->node_count) continue;
        {
            IRNode *target = &ir->nodes[d->aux];
            if (d->flags & NF_DECORATOR_REGISTER) {
                if (!(target->kind == N_CLASS || target->kind == N_STRUCT || target->kind == N_UNION || target->kind == N_ENUM))
                    die("@register can only be applied to a class, struct, union, or enum (token %" PRIu64 ")", target->first_tok);
                if (target->kind == N_ENUM && (!target->name || !*target->name))
                    die("@register requires a named enum (token %" PRIu64 ")", target->first_tok);
                target->flags |= NF_REGISTERED;
            }
            if (d->flags & NF_DECORATOR_EXPOSED) {
                if (!(target->kind == N_FIELD_DECL || target->kind == N_METHOD))
                    die("@exposed can only be applied to a public member variable or method (token %" PRIu64 ")", target->first_tok);
                if ((target->flags & NF_ACCESS_MASK) != NF_ACCESS_PUBLIC)
                    die("@exposed member '%s' must be public", target->name ? target->name : "<unnamed>");
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
        size_t j;
        int found = 0;
        const char *dkey;
        if (d->kind != N_DECORATOR || d->aux == UINT64_MAX) continue;
        if (d->flags & NF_DECORATOR_NATIVE) continue;
        dkey = d->qualified ? d->qualified : d->name;
        for (j = 0; j < ir->node_count; ++j) {
            IRNode *cd = &ir->nodes[j];
            const char *ckey = cd->qualified ? cd->qualified : cd->name;
            if (cd->kind == N_CUSTOM_DECORATOR && dkey && ckey && strcmp(ckey, dkey) == 0) {
                d->flags |= NF_DECORATOR_CUSTOM;
                d->resolved_name = d->resolved_name ? d->resolved_name : xstrdup0(ckey);
                found = 1;
                break;
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

static int resolve_named_args(IR *ir) {
    size_t i;
    int changed = 0;
    for (i = 0; i < ir->node_count; ++i) {
        IRNode *call = &ir->nodes[i];
        size_t f;
        if (call->kind != N_FUNC_CALL) continue;
        for (f = 0; f < ir->node_count; ++f) {
            IRNode *fn = &ir->nodes[f];
            int matches = 0;
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
                            break;
                        }
                        pi++;
                    }
                    param = pn->next_sibling;
                }
            }
            if (changed) break;
        }
    }
    return changed;
}

static int node_is_named_call(const IR *ir, int64_t id) {
    const IRNode *n = &ir->nodes[id];
    int64_t c;
    for (c = n->first_child; c >= 0; c = ir->nodes[c].next_sibling)
        if (ir->nodes[c].kind == N_NAMED_ARG) return 1;
    return 0;
}

static int find_matching_function(const IR *ir, const IRNode *call, int64_t *out) {
    size_t i;
    int best = -1;
    size_t named_count = 0, call_arg_count = 0;
    int64_t c;
    for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind == N_NAMED_ARG || ir->nodes[c].kind == N_CALL_ARG) call_arg_count++;
        if (ir->nodes[c].kind == N_NAMED_ARG) named_count++;
    }
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *fn = &ir->nodes[i];
        size_t params = 0;
        int matches = 1;
        if (!(fn->kind == N_FUNCTION || fn->kind == N_METHOD || fn->kind == N_CONSTRUCTOR ||
              fn->kind == N_OPERATOR_FUNCTION)) continue;
        if (call->resolved_name && *call->resolved_name && fn->qualified && *fn->qualified) {
            if (strcmp(call->resolved_name, fn->qualified) != 0) {
                if (!(call->name && fn->name && strcmp(call->name, fn->name) == 0)) continue;
            }
        } else if (call->name && fn->name && strcmp(call->name, fn->name) != 0) {
            continue;
        }
        {
            int64_t p = fn->first_child;
            while (p >= 0) {
                if (ir->nodes[p].kind == N_PARAM_DECL) params++;
                p = ir->nodes[p].next_sibling;
            }
        }
        if (named_count > params) continue;
        if (call_arg_count > params) continue;
        for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
            const IRNode *arg = &ir->nodes[c];
            int64_t q;
            if (arg->kind != N_NAMED_ARG) continue;
            int found = 0;
            for (q = fn->first_child; q >= 0; q = ir->nodes[q].next_sibling) {
                const IRNode *param = &ir->nodes[q];
                if (param->kind == N_PARAM_DECL && param->name && arg->name && strcmp(param->name, arg->name) == 0) {
                    found = 1; break;
                }
            }
            if (!found) { matches = 0; break; }
        }
        if (!matches) continue;
        best = (int)i;
        break;
    }
    if (best >= 0 && out) *out = best;
    return best >= 0;
}

static int64_t function_param_by_name(const IR *ir, const IRNode *fn, const char *name, size_t *index_out) {
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

static size_t function_param_count(const IR *ir, const IRNode *fn) {
    size_t n = 0; int64_t p;
    for (p = fn->first_child; p >= 0; p = ir->nodes[p].next_sibling)
        if (ir->nodes[p].kind == N_PARAM_DECL) n++;
    return n;
}

static void append_token_bytes(const IR *ir, Str *out, size_t a, size_t b) {
    if (a > b || a >= ir->tokens.n) return;
    if (b >= ir->tokens.n) b = ir->tokens.n - 1;
    if (ir->tokens.v[b].byte_end <= ir->tokens.v[a].byte_start) return;
    str_putn(out, (const char *)ir->source + ir->tokens.v[a].byte_start,
             (size_t)(ir->tokens.v[b].byte_end - ir->tokens.v[a].byte_start));
}


static void emit_unresolved_named_call(const IR *ir, const IRNode *call, Str *out) {
    size_t open = SIZE_MAX, close = SIZE_MAX, i;
    if (call->first_tok >= ir->tokens.n || call->last_tok >= ir->tokens.n) return;
    for (i = (size_t)call->first_tok; i <= (size_t)call->last_tok; ++i) {
        if (strcmp(ir->tokens.v[i].text, "(") == 0) { open = i; break; }
    }
    if (open == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    {
        int d = 0;
        for (i = open; i <= (size_t)call->last_tok; ++i) {
            if (strcmp(ir->tokens.v[i].text, "(") == 0) d++;
            else if (strcmp(ir->tokens.v[i].text, ")") == 0 && --d == 0) { close = i; break; }
        }
    }
    if (close == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    if (open > (size_t)call->first_tok) append_token_bytes(ir, out, (size_t)call->first_tok, open - 1);
    str_ch(out, '(');
    {
        int64_t c; int emitted = 0;
        for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
            const IRNode *arg = &ir->nodes[c];
            if (arg->kind != N_CALL_ARG && arg->kind != N_NAMED_ARG) continue;
            if (emitted) str_put(out, ", ");
            if (arg->kind == N_NAMED_ARG) str_put(out, arg->value ? arg->value : "");
            else append_token_bytes(ir, out, (size_t)arg->first_tok, (size_t)arg->last_tok);
            emitted = 1;
        }
    }
    str_ch(out, ')');
    if (close + 1 <= (size_t)call->last_tok) append_token_bytes(ir, out, close + 1, (size_t)call->last_tok);
}

static size_t call_argument_nodes(const IR *ir, const IRNode *call, int64_t **out) {
    size_t n = 0, cap = 0;
    int64_t c;
    int64_t *v = NULL;
    for (c = call->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind != N_CALL_ARG && ir->nodes[c].kind != N_NAMED_ARG) continue;
        if (n == cap) { cap = cap ? cap * 2 : 8; v = (int64_t *)xrealloc(v, cap * sizeof(*v)); }
        v[n++] = c;
    }
    *out = v;
    return n;
}

static void emit_named_call(const IR *ir, const IRNode *call, Str *out) {
    int64_t fn_id;
    const IRNode *fn = NULL;
    size_t open = SIZE_MAX, close = SIZE_MAX;
    size_t i;
    if (!find_matching_function(ir, call, &fn_id)) {
        emit_unresolved_named_call(ir, call, out);
        return;
    }
    fn = &ir->nodes[fn_id];
    for (i = (size_t)call->first_tok; i <= (size_t)call->last_tok && i < ir->tokens.n; ++i) {
        if (strcmp(ir->tokens.v[i].text, "(") == 0) { open = i; break; }
    }
    if (open == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    {
        int d = 0;
        for (i = open; i <= (size_t)call->last_tok && i < ir->tokens.n; ++i) {
            if (strcmp(ir->tokens.v[i].text, "(") == 0) d++;
            else if (strcmp(ir->tokens.v[i].text, ")") == 0 && --d == 0) { close = i; break; }
        }
    }
    if (close == SIZE_MAX) { append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok); return; }
    if (open > (size_t)call->first_tok) append_token_bytes(ir, out, (size_t)call->first_tok, open - 1);
    str_ch(out, '(');
    {
        int64_t *args = NULL; size_t an = call_argument_nodes(ir, call, &args), idx;
        size_t pc = function_param_count(ir, fn);
        int *used = (int *)xcalloc(pc ? pc : 1, sizeof(*used));
        int64_t *chosen = (int64_t *)xcalloc(pc ? pc : 1, sizeof(*chosen));
        for (idx = 0; idx < pc; ++idx) chosen[idx] = -1;
        size_t next_pos = 0;
        int impossible = 0;
        for (idx = 0; idx < an; ++idx) {
            const IRNode *arg = &ir->nodes[args[idx]];
            if (arg->kind == N_NAMED_ARG) {
                size_t pi;
                if (function_param_by_name(ir, fn, arg->name, &pi) >= 0 && pi < pc && !used[pi]) {
                    chosen[pi] = args[idx]; used[pi] = 1;
                } else impossible = 1;
            } else {
                while (next_pos < pc && used[next_pos]) next_pos++;
                if (next_pos < pc) { chosen[next_pos] = args[idx]; used[next_pos] = 1; next_pos++; }
                else impossible = 1;
            }
        }
        if (impossible) {
            /* Keep the CXXE call semantics best-effort rather than emitting an invalid ordering. */
            str_free(out);
            str_init(out);
            emit_unresolved_named_call(ir, call, out);
        } else {
            int emitted = 0;
            for (idx = 0; idx < pc; ++idx) {
                const IRNode *arg;
                if (chosen[idx] < 0) {
                    int64_t param_id = -1; size_t pi2 = 0; int64_t c;
                    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
                        if (ir->nodes[c].kind == N_PARAM_DECL) {
                            if (pi2 == idx) { param_id = c; break; }
                            pi2++;
                        }
                    }
                    if (param_id >= 0) {
                        const IRNode *param = &ir->nodes[param_id]; size_t eq = SIZE_MAX, q;
                        for (q = (size_t)param->first_tok; q <= (size_t)param->last_tok && q < ir->tokens.n; ++q) {
                            if (strcmp(ir->tokens.v[q].text, "=") == 0) { eq = q; break; }
                        }
                        if (eq != SIZE_MAX && eq < (size_t)param->last_tok) {
                            if (emitted) str_put(out, ", ");
                            append_token_bytes(ir, out, eq + 1, (size_t)param->last_tok);
                            emitted = 1; continue;
                        }
                    }
                    continue;
                }
                arg = &ir->nodes[chosen[idx]];
                if (emitted) str_put(out, ", ");
                if (arg->kind == N_NAMED_ARG) str_put(out, arg->value ? arg->value : "");
                else append_token_bytes(ir, out, (size_t)arg->first_tok, (size_t)arg->last_tok);
                emitted = 1;
            }
        }
        free(chosen); free(used); free(args);
    }
    str_ch(out, ')');
    if (close + 1 <= (size_t)call->last_tok)
        append_token_bytes(ir, out, close + 1, (size_t)call->last_tok);
}

static void emit_cxxe_string(Str *out, const char *s) {
    size_t i;
    str_ch(out, '"');
    if (s) {
        for (i = 0; s[i]; ++i) {
            unsigned char c = (unsigned char)s[i];
            if (c == '\\' || c == '"') { str_ch(out, '\\'); str_ch(out, (char)c); }
            else if (c == '\n') str_put(out, "\\n");
            else if (c == '\r') str_put(out, "\\r");
            else if (c == '\t') str_put(out, "\\t");
            else str_ch(out, (char)c);
        }
    }
    str_ch(out, '"');
}

static int class_has_runtime_features(const IR *ir) {
    size_t i;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if ((n->kind == N_CLASS || n->kind == N_STRUCT || n->kind == N_UNION || n->kind == N_ENUM) && (n->flags & NF_REGISTERED)) return 1;
        if (n->kind == N_GET_MEMBER && (n->flags & NF_GET_MEMBER_RUNTIME)) return 1;
        if (n->kind == N_FUNC_CALL && n->qualified &&
            (strcmp(n->qualified, "stde::factory_new") == 0 || strcmp(n->qualified, "stde::factory_find") == 0)) return 1;
        if (n->kind == N_MATCH_STMT) return 1;
    }
    for (i = 0; i < ir->tokens.n; ++i) if (strcmp(ir->tokens.v[i].text, "dynamic") == 0) return 1;
    return 0;
}

static void emit_method_callable_name(const IR *ir, const IRNode *method, Str *out) {
    size_t a = (size_t)method->first_tok, b = (size_t)method->last_tok, i;
    if (!method->name || strcmp(method->name, "operator") != 0) {
        str_put(out, method->name ? method->name : "");
        return;
    }
    for (i = a; i <= b && i < ir->tokens.n; ++i) {
        if (strcmp(ir->tokens.v[i].text, "operator") == 0) {
            /* For operator(), the actual parameter list is the second pair of
               parentheses. Taking source tokens until the final function-open
               paren gives the exact callable spelling. */
            size_t j = i + 1, last_open = SIZE_MAX;
            int d = 0;
            for (; j <= b; ++j) {
                const char *t = ir->tokens.v[j].text;
                if (strcmp(t, "(") == 0) { d++; last_open = j; }
                else if (strcmp(t, ")") == 0 && d > 0) d--;
                if (strcmp(t, "{") == 0 || strcmp(t, ";") == 0) break;
            }
            if (last_open != SIZE_MAX) {
                size_t end = last_open;
                while (end > i && (strcmp(ir->tokens.v[end - 1].text, ")") == 0 ||
                       strcmp(ir->tokens.v[end - 1].text, "]") == 0)) end--;
                for (j = i; j < end; ++j) {
                    str_put(out, ir->tokens.v[j].text);
                }
                return;
            }
            str_put(out, "operator");
            return;
        }
    }
    str_put(out, "operator");
}

static int field_is_readonly(const IRNode *field) {
    if (!field->type) return 0;
    if (strstr(field->type, "const") != NULL) return 1;
    if (strchr(field->type, '[') != NULL) return 1;
    return 0;
}

static void emit_registered_member_field(const IRNode *cls, const IRNode *field, Str *out) {
    const char *ctype = cls->qualified && *cls->qualified ? cls->qualified : cls->name;
    str_put(out, ".expose_field(");
    emit_cxxe_string(out, field->name ? field->name : "");
    str_put(out, ",\n        [](void* self) -> stde::DynamicValue {\n            auto* obj = static_cast<");
    str_put(out, ctype ? ctype : "void");
    str_put(out, "*>(self);\n            return stde::DynamicValue::from(obj->");
    str_put(out, field->name ? field->name : "");
    str_put(out, ");\n        }");

    if (!field_is_readonly(field)) {
        str_put(out, ",\n        [](void* self, const std::any& value) {\n            auto* obj = static_cast<");
        str_put(out, ctype ? ctype : "void");
        str_put(out, "*>(self);\n            using MemberType = std::decay_t<decltype(obj->");
        str_put(out, field->name ? field->name : "");
        str_put(out, ")>;\n            obj->");
        str_put(out, field->name ? field->name : "");
        str_put(out, " = std::any_cast<MemberType>(value);\n        }");
    }
    str_put(out, ")");
}

static size_t function_param_ids(const IR *ir, const IRNode *fn, int64_t *ids, size_t cap) {
    size_t n = 0;
    int64_t c;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        if (ir->nodes[c].kind != N_PARAM_DECL) continue;
        if (ids && n < cap) ids[n] = c;
        n++;
    }
    return n;
}

static int method_returns_void(const IR *ir, const IRNode *method) {
    size_t a = (size_t)method->first_tok, b = (size_t)method->last_tok, i;
    const char *rt = method->return_type ? method->return_type : "";
    while (*rt && isspace((unsigned char)*rt)) rt++;
    if (strncmp(rt, "void", 4) == 0 &&
        (rt[4] == 0 || isspace((unsigned char)rt[4]) || rt[4] == '*')) return 1;

    /* C++ trailing return type: auto f(...) -> void */
    for (i = a; i + 1 <= b && i < ir->tokens.n; ++i) {
        if (strcmp(ir->tokens.v[i].text, "->") == 0 && i + 1 <= b &&
            strcmp(ir->tokens.v[i + 1].text, "void") == 0) return 1;
    }
    return 0;
}

static void emit_registered_member_method(const IR *ir, const IRNode *cls, const IRNode *method, Str *out) {
    const char *ctype = cls->qualified && *cls->qualified ? cls->qualified : cls->name;
    int64_t params[64];
    size_t pn = function_param_ids(ir, method, params, 64), i;

    if (pn > 64) die("@exposed method '%s' has too many parameters", method->name ? method->name : "<unnamed>");

    str_put(out, ".expose_method(");
    Str method_name; str_init(&method_name);
    emit_method_callable_name(ir, method, &method_name);
    emit_cxxe_string(out, method_name.data ? method_name.data : (method->name ? method->name : ""));
    str_free(&method_name);
    str_put(out, ",\n        [](void* self, const std::vector<std::any>& args) -> stde::DynamicValue {\n            if (args.size() != ");
    {
        char num[32]; snprintf(num, sizeof(num), "%zu", pn); str_put(out, num);
    }
    str_put(out, ") throw std::runtime_error(\"CXXE: invalid argument count for exposed method\");\n            auto* obj = static_cast<");
    str_put(out, ctype ? ctype : "void");
    str_put(out, "*>(self);\n");

    for (i = 0; i < pn; ++i) {
        const IRNode *param = &ir->nodes[params[i]];
        if (!param->type || !*param->type) die("CXXE: cannot emit runtime wrapper for exposed method parameter '%s'", param->name ? param->name : "<unnamed>");
        str_put(out, "            using P");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " = ");
        str_put(out, param->type);
        str_put(out, ";\n            P");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " a");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " = stde::detail::unpack_argument<P");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, ">(args[");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, "]);\n");
    }

    if (method_returns_void(ir, method)) {
        str_put(out, "            obj->");
        emit_method_callable_name(ir, method, out);
        str_put(out, "(");
        for (i = 0; i < pn; ++i) {
            if (i) str_put(out, ", ");
            str_put(out, "a");
            { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        }
        str_put(out, ");\n            return stde::DynamicValue();\n");
    } else {
        str_put(out, "            return stde::DynamicValue::from(obj->");
        emit_method_callable_name(ir, method, out);
        str_put(out, "(");
        for (i = 0; i < pn; ++i) {
            if (i) str_put(out, ", ");
            str_put(out, "a");
            { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        }
        str_put(out, "));\n");
    }

    str_put(out, "        }");
    str_put(out, ")");
}

static void emit_registered_enum(const IR *ir, const IRNode *en, size_t index, Str *out) {
    int64_t c;
    str_put(out, "        auto cxxe_enum_");
    { char num[32]; snprintf(num, sizeof(num), "%zu", index); str_put(out, num); }
    str_put(out, " = stde::register_enum<"); str_put(out, en->qualified);
    str_put(out, ">("); emit_cxxe_string(out, en->qualified); str_put(out, ");\n");
    for (c = en->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *ev=&ir->nodes[c];
        if (ev->kind != N_ENUMERATOR || !ev->name || !*ev->name) continue;
        str_put(out, "        cxxe_enum_"); { char num[32]; snprintf(num,sizeof(num),"%zu",index); str_put(out,num); }
        str_put(out, ".value("); emit_cxxe_string(out, ev->name); str_put(out, ", ");
        str_put(out, en->qualified); str_put(out, "::"); str_put(out, ev->name); str_put(out, ");\n");
    }
}

static void emit_source_range_with_properties(const IR *ir, Str *out, uint64_t begin, uint64_t end);

static void emit_registered_classes(const IR *ir, Str *out) {
    size_t i;
    int any = 0;
    str_put(out, "\n\nnamespace cxxe_generated {\nstatic void register_all() {\n    static const bool initialized = []() {\n");
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *cls = &ir->nodes[i];
        int64_t c;
        if (!((cls->kind == N_CLASS || cls->kind == N_STRUCT || cls->kind == N_UNION || cls->kind == N_ENUM) && (cls->flags & NF_REGISTERED))) continue;
        if (!cls->name || !*cls->name || !cls->qualified || !*cls->qualified)
            die("@register requires a named type");
        any = 1;
        if (cls->kind == N_ENUM) { emit_registered_enum(ir, cls, i, out); continue; }
        str_put(out, "        auto cxxe_class_");
        { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
        str_put(out, " = stde::register_class<");
        str_put(out, cls->qualified);
        str_put(out, ">(");
        emit_cxxe_string(out, cls->qualified);
        str_put(out, ");\n");
        for (c = cls->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
            const IRNode *m = &ir->nodes[c];
            if (!(m->flags & NF_EXPOSED)) continue;
            str_put(out, "        cxxe_class_");
            { char num[32]; snprintf(num, sizeof(num), "%zu", i); str_put(out, num); }
            if (m->kind == N_FIELD_DECL) emit_registered_member_field(cls, m, out);
            else if (m->kind == N_METHOD) emit_registered_member_method(ir, cls, m, out);
            else die("CXXE: unsupported @exposed member kind");
            str_put(out, ";\n");
        }
    }
    str_put(out, "        return true;\n    }();\n    (void)initialized;\n}\n}\n");
    str_put(out, "namespace {\nstruct CxxeAutoRegistration {\n    CxxeAutoRegistration() { cxxe_generated::register_all(); }\n};\nstatic CxxeAutoRegistration cxxe_auto_registration;\n}\n");
    (void)any;
}

static int node_is_factory_runtime_call(const IRNode *n) {
    return n->kind == N_FUNC_CALL && n->qualified &&
        (strcmp(n->qualified, "stde::factory_new") == 0 || strcmp(n->qualified, "stde::factory_find") == 0);
}

static int find_call_close(const IR *ir, const IRNode *call, size_t *open_out, size_t *close_out) {
    size_t i, open = SIZE_MAX, close = SIZE_MAX;
    if (call->first_tok >= ir->tokens.n || call->last_tok >= ir->tokens.n) return 0;
    for (i = (size_t)call->first_tok; i <= (size_t)call->last_tok; ++i) {
        if (strcmp(ir->tokens.v[i].text, "(") == 0) { open = i; break; }
    }
    if (open == SIZE_MAX) return 0;
    {
        int d = 0;
        for (i = open; i <= (size_t)call->last_tok; ++i) {
            if (strcmp(ir->tokens.v[i].text, "(") == 0) d++;
            else if (strcmp(ir->tokens.v[i].text, ")") == 0 && --d == 0) { close = i; break; }
        }
    }
    if (close == SIZE_MAX) return 0;
    if (open_out) *open_out = open;
    if (close_out) *close_out = close;
    return 1;
}

static void emit_factory_runtime(const IR *ir, const IRNode *call, Str *out) {
    size_t open, close, i;
    if (!find_call_close(ir, call, &open, &close)) {
        append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok);
        return;
    }

    /* new stde::factory_find(name)() is CXXE syntax for a runtime factory. */
    if (strcmp(call->qualified, "stde::factory_find") == 0 && call->first_tok > 0 &&
        strcmp(ir->tokens.v[call->first_tok - 1].text, "new") == 0) {
        size_t next = close + 1;
        if (next + 1 < ir->tokens.n && strcmp(ir->tokens.v[next].text, "(") == 0 && strcmp(ir->tokens.v[next + 1].text, ")") == 0) {
            str_put(out, "(cxxe_generated::register_all(), stde::factory_new(");
            if (open + 1 <= close - 1) {
                for (i = open + 1; i <= close - 1; ++i) {
                    str_put(out, ir->tokens.v[i].trivia);
                    str_put(out, ir->tokens.v[i].text);
                }
            }
            str_put(out, "))");
            return;
        }
    }

    str_put(out, "(cxxe_generated::register_all(), ");
    append_token_bytes(ir, out, (size_t)call->first_tok, (size_t)call->last_tok);
    str_ch(out, ')');
}

static void emit_get_member_runtime(const IR *ir, const IRNode *node, Str *out) {
    size_t a = (size_t)node->first_tok, b = (size_t)node->last_tok, i;
    if (!get_member_pattern(ir, node, NULL, NULL, NULL)) {
        append_token_bytes(ir, out, a, b);
        return;
    }
    for (i = a + 1; i + 3 <= b; ++i) {
        if ((strcmp(ir->tokens.v[i].text, ".") == 0 || strcmp(ir->tokens.v[i].text, "->") == 0) &&
            strcmp(ir->tokens.v[i + 1].text, "get_member") == 0 && strcmp(ir->tokens.v[i + 2].text, "(") == 0) {
            size_t close = SIZE_MAX, j = i + 2;
            int d = 0;
            for (; j <= b; ++j) {
                if (strcmp(ir->tokens.v[j].text, "(") == 0) d++;
                else if (strcmp(ir->tokens.v[j].text, ")") == 0 && --d == 0) { close = j; break; }
            }
            if (close == SIZE_MAX) break;
            str_put(out, "(cxxe_generated::register_all(), stde::get_member(");
            append_token_bytes(ir, out, a, i - 1);
            str_put(out, ", ");
            append_token_bytes(ir, out, i + 3, close - 1);
            str_put(out, "))");
            if (close + 1 <= b) append_token_bytes(ir, out, close + 1, b);
            return;
        }
    }
    append_token_bytes(ir, out, a, b);
}


static const IRNode *find_custom_decorator_def(const IR *ir, const char *name, const char *qualified_name) {
    size_t i;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (n->kind != N_CUSTOM_DECORATOR) continue;
        if (qualified_name && n->qualified && strcmp(n->qualified, qualified_name) == 0) return n;
    }
    /* Backward-compatible fallback for older IRs which did not store qualified names. */
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (n->kind == N_CUSTOM_DECORATOR && n->name && name && strcmp(n->name, name) == 0) return n;
    }
    return NULL;
}

static int function_has_custom_decorator(const IR *ir, const IRNode *fn) {
    int64_t c;
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *d = &ir->nodes[c];
        if (d->kind == N_DECORATOR && (d->flags & NF_DECORATOR_CUSTOM)) return 1;
    }
    return 0;
}

static size_t function_body_open(const IR *ir, const IRNode *fn) {
    size_t i;
    for (i = (size_t)fn->first_tok; i <= (size_t)fn->last_tok && i < ir->tokens.n; ++i)
        if (strcmp(ir->tokens.v[i].text, "{") == 0) return i;
    return SIZE_MAX;
}

static size_t function_name_token(const IR *ir, const IRNode *fn) {
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

static void emit_tokens_range_raw(const IR *ir, Str *out, size_t a, size_t b, const char *replace_name, size_t replace_tok) {
    size_t i;
    if (a > b || b >= ir->tokens.n) return;
    for (i = a; i <= b; ++i) {
        str_put(out, ir->tokens.v[i].trivia);
        if (i == replace_tok && replace_name) str_put(out, replace_name);
        else str_put(out, ir->tokens.v[i].text);
    }
}

static void emit_tokens_range(const IR *ir, Str *out, size_t a, size_t b, const char *replace_name, size_t replace_tok) {
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
            if (find_property_by_name(ir, ir->tokens.v[i + 1].text, &pid)) {
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
static void emit_source_range_with_properties(const IR *ir, Str *out, uint64_t begin, uint64_t end) {
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
            if (find_property_by_name(ir, ir->tokens.v[i + 1].text, &pid)) {
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

static int decorator_template_arg_ranges(const IR *ir, const IRNode *dec, size_t *starts, size_t *ends, size_t cap) {
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

static size_t custom_decorator_template_param_count(const IR *ir, const IRNode *cd) {
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
    if (!decorator_invocation_args(ir, dec, dec_starts, dec_ends, 64, &dec_count)) die("CXXE: malformed custom decorator invocation '%s'", dec->name ? dec->name : "<unnamed>");
    if (dec_count != dpn) die("CXXE: decorator '%s' expects %zu argument(s), got %zu", cd->name ? cd->name : "<unnamed>", dpn, dec_count);
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
    if (!decorator_invocation_args(ir, dec, dec_starts, dec_ends, 64, &dec_count)) die("CXXE: malformed custom decorator invocation '%s'", dec->name ? dec->name : "<unnamed>");
    if (dec_count != dpn) die("CXXE: decorator '%s' expects %zu argument(s), got %zu", cd->name ? cd->name : "<unnamed>", dpn, dec_count);
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
                    if (close == SIZE_MAX) die("CXXE: malformed func(...) in custom decorator '%s'", cd->name ? cd->name : "<unnamed>");
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

static void emit_property_block(const IR *ir, const IRNode *block, Str *out) {
    size_t a=(size_t)block->first_tok,b=(size_t)block->last_tok,i,open=SIZE_MAX;
    for(i=a;i<=b && i<ir->tokens.n;++i) if(!strcmp(ir->tokens.v[i].text,"{")){open=i;break;}
    if(open==SIZE_MAX || b<=open) return;
    emit_tokens_range_raw(ir,out,open+1,b-1,NULL,SIZE_MAX);
}

static void emit_property(const IR *ir, const IRNode *prop, Str *out) {
    int64_t g=property_child_kind(ir,prop,N_PROPERTY_GET), s=property_child_kind(ir,prop,N_PROPERTY_SET);
    const char *name=prop->name?prop->name:"Property";
    if(g>=0){ str_put(out,"decltype(auto) get_"); str_put(out,name); str_put(out,"() {"); emit_property_block(ir,&ir->nodes[g],out); str_put(out,"}\n"); }
    if(s>=0){ str_put(out,"template <typename CXXEPropertyValue>\nvoid set_"); str_put(out,name); str_put(out,"(CXXEPropertyValue&& value) {"); emit_property_block(ir,&ir->nodes[s],out); str_put(out,"}\n"); }
}

static int property_access_is_write(const IR *ir, const IRNode *node) {
    size_t i=(size_t)node->last_tok+1;
    if (i>=ir->tokens.n) return 0;
    return !strcmp(ir->tokens.v[i].text,"=") || !strcmp(ir->tokens.v[i].text,"+=") || !strcmp(ir->tokens.v[i].text,"-=") ||
           !strcmp(ir->tokens.v[i].text,"*=") || !strcmp(ir->tokens.v[i].text,"/=") || !strcmp(ir->tokens.v[i].text,"%=");
}

static void emit_property_access(const IR *ir, const IRNode *node, Str *out) {
    size_t a=(size_t)node->first_tok,b=(size_t)node->last_tok;
    size_t sep=SIZE_MAX;
    if(b<a || node->aux>=ir->node_count){ append_token_bytes(ir,out,a,b); return; }
    {
        const IRNode *prop=&ir->nodes[node->aux];
        if(!property_has_get(ir,prop) || property_access_is_write(ir,node)){ append_token_bytes(ir,out,a,b); return; }
    }
    if (b > a && (!strcmp(ir->tokens.v[b-1].text,".") || !strcmp(ir->tokens.v[b-1].text,"->"))) sep=b-1;
    if (sep==SIZE_MAX) { append_token_bytes(ir,out,a,b); return; }
    append_token_bytes(ir,out,a,sep-1);
    str_put(out,ir->tokens.v[sep].text);
    str_put(out,"get_");
    str_put(out,ir->nodes[node->aux].name?ir->nodes[node->aux].name:node->name);
    str_put(out,"()");
}

static int node_is_property_assignment(const IR *ir, const IRNode *node, int64_t *prop_id_out, size_t *op_out, size_t *sep_out) {
    size_t a=(size_t)node->first_tok,b=(size_t)node->last_tok,i;
    int par=0,br=0,cur=0, top_semis=0;
    size_t op=SIZE_MAX, sep=SIZE_MAX; int64_t pid=-1;
    for(i=a;i<=b;++i){
        const char*t=ir->tokens.v[i].text;
        if(!strcmp(t,"("))par++; else if(!strcmp(t,")")&&par)par--; else if(!strcmp(t,"["))br++; else if(!strcmp(t,"]")&&br)br--; else if(!strcmp(t,"{"))cur++; else if(!strcmp(t,"}")&&cur)cur--;
        else if(!par&&!br&&!cur&&!strcmp(t,";")) top_semis++;
        else if(!par&&!br&&!cur&&!strcmp(t,"=")&&op==SIZE_MAX&&i>a&&ir->tokens.v[i-1].kind==TK_IDENTIFIER){
            size_t pn=i-1;
            if(pn>a && (!strcmp(ir->tokens.v[pn-1].text,".")||!strcmp(ir->tokens.v[pn-1].text,"->")) && find_property_by_name(ir,ir->tokens.v[pn].text,&pid)){
                op=i; sep=pn-1;
            }
        }
    }
    if(top_semis>1 || op==SIZE_MAX) return 0;
    if(prop_id_out)*prop_id_out=pid;
    if(op_out)*op_out=op;
    if(sep_out)*sep_out=sep;
    return 1;
}

static void emit_property_assignment(const IR *ir, const IRNode *node, Str *out) {
    size_t a=(size_t)node->first_tok,b=(size_t)node->last_tok,op=SIZE_MAX,sep=SIZE_MAX;
    int64_t pid=-1;
    if(!node_is_property_assignment(ir,node,&pid,&op,&sep)){ append_token_bytes(ir,out,a,b); return; }
    if(!property_has_set(ir,&ir->nodes[pid])) die("CXXE: property '%s' has no setter",ir->nodes[pid].name?ir->nodes[pid].name:"<unnamed>");
    append_token_bytes(ir,out,a,sep-1);
    str_put(out,ir->tokens.v[sep].text);
    str_put(out,"set_"); str_put(out,ir->nodes[pid].name?ir->nodes[pid].name:"Property"); str_ch(out,'(');
    if(op+1<=b){
        size_t rhs_b=b;
        if(rhs_b>=a && !strcmp(ir->tokens.v[rhs_b].text,";")) rhs_b--;
        if(op+1<=rhs_b) append_token_bytes(ir,out,op+1,rhs_b);
    }
    str_ch(out,')');
    if(b>=a && !strcmp(ir->tokens.v[b].text,";")) str_ch(out,';');
}

static void emit_function_with_custom_decorators(const IR *ir, const IRNode *fn, Str *out) {
    int64_t custom_ids[64];
    size_t custom_count = 0, i;
    int64_t c;
    size_t body_open = function_body_open(ir, fn);
    size_t name_tok = function_name_token(ir, fn);
    if (body_open == SIZE_MAX || name_tok == SIZE_MAX) die("CXXE: custom decorators require a function definition with a named function");
    if (!(fn->kind == N_FUNCTION || fn->kind == N_METHOD)) die("CXXE: custom decorators can only be applied to functions or methods");
    for (c = fn->first_child; c >= 0; c = ir->nodes[c].next_sibling) {
        const IRNode *d = &ir->nodes[c];
        if (d->kind == N_DECORATOR && (d->flags & NF_DECORATOR_CUSTOM)) {
            if (custom_count < 64) custom_ids[custom_count] = c;
            custom_count++;
        }
    }
    if (custom_count > 64) die("CXXE: too many custom decorators on function '%s'", fn->name ? fn->name : "<unnamed>");

    /* Base implementation: same signature/body, with a private unique name. */
    {
        char base_name[256];
        snprintf(base_name, sizeof(base_name), "%s__cxxe_impl_%" PRIu64, fn->name ? fn->name : "function", (uint64_t)(fn - ir->nodes));
        emit_tokens_range(ir, out, (size_t)fn->first_tok, body_open - 1, base_name, name_tok);
        str_put(out, " {");
        if (body_open + 1 <= (size_t)fn->last_tok) emit_tokens_range(ir, out, body_open + 1, (size_t)fn->last_tok - 1, NULL, SIZE_MAX);
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

static int match_pattern_is_type(const IR *ir, size_t a, size_t b) {
    size_t i;
    if (a > b || b >= ir->tokens.n) return 0;
    if (!strcmp(ir->tokens.v[a].text, "default") || !strcmp(ir->tokens.v[a].text, "_")) return 1;
    for (i = a; i <= b; ++i) {
        const char *t = ir->tokens.v[i].text;
        if (isalpha((unsigned char)t[0]) || t[0] == '_' || !strcmp(t, "::") || !strcmp(t, "<") ||
            !strcmp(t, ">") || !strcmp(t, ",") || !strcmp(t, "*") || !strcmp(t, "&") || !strcmp(t, "&&")) continue;
        return 0;
    }
    return 1;
}

static void emit_match_statement(const IR *ir, const IRNode *node, Str *out) {
    size_t a=(size_t)node->first_tok, b=(size_t)node->last_tok, brace=(node->aux<ir->tokens.n?(size_t)node->aux:SIZE_MAX), close=b;
    size_t subject_a=a+1, subject_b, i, labels=0, label_start[128], label_colon[128];
    size_t candidate;
    int par=0, br=0, cur=0, angle=0;
    const char *subject_name;
    if (brace==SIZE_MAX || brace<=a+1 || brace>=close) die("CXXE: invalid match statement");
    subject_b=brace-1;
    if (subject_a!=subject_b || ir->tokens.v[subject_a].kind!=TK_IDENTIFIER) die("CXXE: match currently requires a simple variable as its value");
    subject_name=ir->tokens.v[subject_a].text;
    candidate=brace+1;
    for (i=brace+1;i<close;++i) {
        const char *t=ir->tokens.v[i].text;
        if (!strcmp(t,"(")) par++;
        else if (!strcmp(t,")")&&par) par--;
        else if (!strcmp(t,"[")) br++;
        else if (!strcmp(t,"]")&&br) br--;
        else if (!strcmp(t,"{")) cur++;
        else if (!strcmp(t,"}")) { if (cur) { cur--; if (!cur) candidate=i+1; } }
        else if (!strcmp(t,"<")) angle++;
        else if (!strcmp(t,">")&&angle) angle--;
        else if (!strcmp(t,";")&&!par&&!br&&!cur&&!angle) candidate=i+1;
        else if (!strcmp(t,":")&&!par&&!br&&!cur&&!angle&&candidate<i&&match_pattern_is_type(ir,candidate,i-1)) {
            if (labels>=128) die("CXXE: too many match cases");
            label_start[labels]=candidate; label_colon[labels]=i; labels++; candidate=i+1;
        }
    }
    if (!labels) die("CXXE: match must contain at least one case");
    str_put(out,"{ auto&& __cxxe_match_value = ("); append_token_bytes(ir,out,subject_a,subject_b); str_put(out,");\n");
    for (i=0;i<labels;++i) {
        size_t body_a=label_colon[i]+1, body_b=(i+1<labels?label_start[i+1]-1:close-1);
        int is_default=!strcmp(ir->tokens.v[label_start[i]].text,"default")||!strcmp(ir->tokens.v[label_start[i]].text,"_");
        if (i==0) str_put(out,is_default?"    if (true) {\n":"    if (__cxxe_match_value.is<");
        else str_put(out,is_default?"    else {\n":"    else if (__cxxe_match_value.is<");
        if (!is_default) { append_token_bytes(ir,out,label_start[i],label_colon[i]-1); str_put(out,">()) {\n        auto&& "); str_put(out,subject_name); str_put(out," = __cxxe_match_value.as<"); append_token_bytes(ir,out,label_start[i],label_colon[i]-1); str_put(out,">();\n"); }
        if (body_a<=body_b) { uint64_t bs=ir->tokens.v[body_a].byte_start, be=ir->tokens.v[body_b].byte_end; emit_source_range_with_properties(ir,out,bs,be); str_ch(out,'\n'); }
        str_put(out,"    }\n");
    }
    str_put(out,"}\n");
}

static int node_needs_native_emit(const IR *ir, const IRNode *n) {
    if (n->kind == N_DECORATOR) return 1;
    if (n->kind == N_CUSTOM_DECORATOR) return 1;
    if (n->kind == N_PROPERTY) return 1;
    if (n->kind == N_PROPERTY_ACCESS) return !property_access_is_write(ir, n);
    if (n->kind == N_ASSIGN_EXPR) return node_is_property_assignment(ir, n, NULL, NULL, NULL);
    if ((n->kind == N_FUNCTION || n->kind == N_METHOD) && function_has_custom_decorator(ir, n)) return 1;
    if (n->kind == N_GET_MEMBER) return 1;
    if (n->kind == N_FUNC_CALL && node_is_factory_runtime_call(n)) return 1;
    if (n->kind == N_FUNC_CALL && node_is_named_call(ir, (int64_t)(n - ir->nodes))) return 1;
    if (n->kind == N_MATCH_STMT) return 1;
    return 0;
}

static void emit_cpp(const IR *ir, const char *path) {
    Str out;
    size_t cursor = 0, i;
    int runtime_features = class_has_runtime_features(ir);
    str_init(&out);
    if (runtime_features) {
        str_put(&out, "#include \"cxxe_runtime.hpp\"\nnamespace cxxe_generated { static void register_all(); }\n");
    }
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!node_needs_native_emit(ir, n)) continue;
        if (n->flags & NF_EXTERNAL_DECL) continue;
        if (n->first_tok >= ir->tokens.n || n->last_tok >= ir->tokens.n || n->first_tok > n->last_tok) continue;
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
            if (n->kind == N_DECORATOR || n->kind == N_CUSTOM_DECORATOR) { /* CXXE-only syntax. */ }
            else if (n->kind == N_MATCH_STMT) { emit_match_statement(ir, n, &out); }
            else if (n->kind == N_PROPERTY) { emit_property(ir, n, &out); }
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
    str_free(&out);
}

static void emit_exact(const IR *ir, const char *path) {
    emit_cpp(ir, path);
}

/* ------------------------------------------------------------------------- */
/* Binary CPIR serialization                                                  */
/* ------------------------------------------------------------------------- */

static void w_u32(FILE *f, uint32_t x) { if (fwrite(&x, sizeof(x), 1, f) != 1) die("write error"); }
static void w_u64(FILE *f, uint64_t x) { if (fwrite(&x, sizeof(x), 1, f) != 1) die("write error"); }
static void r_u32(FILE *f, uint32_t *x) { if (fread(x, sizeof(*x), 1, f) != 1) die("invalid CPIR"); }
static void r_u64(FILE *f, uint64_t *x) { if (fread(x, sizeof(*x), 1, f) != 1) die("invalid CPIR"); }

static void w_blob(FILE *f, const void *p, uint64_t n) {
    w_u64(f, n);
    if (n && fwrite(p, 1, (size_t)n, f) != (size_t)n) die("write error");
}

static uint8_t *r_blob(FILE *f, uint64_t *n_out) {
    uint64_t n;
    uint8_t *p;
    r_u64(f, &n);
    p = (uint8_t *)xmalloc((size_t)n + 1);
    if (n && fread(p, 1, (size_t)n, f) != (size_t)n) die("invalid CPIR");
    p[n] = 0;
    if (n_out) *n_out = n;
    return p;
}

static void w_str(FILE *f, const char *s) { w_blob(f, s ? s : "", s ? strlen(s) : 0); }

static char *r_str(FILE *f) {
    uint64_t n;
    uint8_t *p = r_blob(f, &n);
    return (char *)p;
}

static void save_ir(const IR *ir, const char *path) {
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f) die("cannot create '%s': %s", path, strerror(errno));
    if (fwrite(CPIR_MAGIC, 1, 4, f) != 4) die("write error");
    w_u32(f, CPIR_VERSION);
    w_u32(f, 0x01020304u);
    w_blob(f, ir->source, ir->source_size);
    w_u64(f, (uint64_t)ir->tokens.n);
    for (i = 0; i < ir->tokens.n; ++i) {
        const Token *t = &ir->tokens.v[i];
        w_u32(f, t->kind);
        w_u32(f, t->line);
        w_u32(f, t->column);
        w_u64(f, t->byte_start);
        w_u64(f, t->byte_end);
        w_str(f, t->trivia);
        w_str(f, t->text);
    }
    w_u64(f, (uint64_t)ir->node_count);
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        w_u32(f, (uint32_t)n->kind);
        w_u32(f, n->flags);
        w_u64(f, n->first_tok);
        w_u64(f, n->last_tok);
        w_u64(f, (uint64_t)n->parent);
        w_u64(f, (uint64_t)n->first_child);
        w_u64(f, (uint64_t)n->last_child);
        w_u64(f, (uint64_t)n->next_sibling);
        w_u64(f, (uint64_t)n->prev_sibling);
        w_str(f, n->name);
        w_str(f, n->qualified);
        w_str(f, n->type);
        w_str(f, n->return_type);
        w_str(f, n->operator_spelling);
        w_str(f, n->value);
        w_str(f, n->resolved_name);
        w_u64(f, n->aux);
    }
    if (fclose(f) != 0) die("cannot close '%s'", path);
}

static int64_t rd_i64(uint64_t x) { return (int64_t)x; }

static void load_ir(IR *ir, const char *path) {
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint32_t ver, endian;
    uint64_t n, i;
    if (!f) die("cannot open '%s': %s", path, strerror(errno));
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, CPIR_MAGIC, 4) != 0) die("'%s' is not CPIR", path);
    r_u32(f, &ver);
    r_u32(f, &endian);
    if (ver != CPIR_VERSION) die("unsupported CPIR version %u", ver);
    if (endian != 0x01020304u) die("unsupported CPIR byte order");
    ir->source = r_blob(f, &n);
    ir->source_size = (size_t)n;
    r_u64(f, &n);
    ir->tokens.v = (Token *)xcalloc((size_t)n, sizeof(*ir->tokens.v));
    ir->tokens.n = ir->tokens.cap = (size_t)n;
    for (i = 0; i < n; ++i) {
        Token *t = &ir->tokens.v[i];
        r_u32(f, &t->kind); r_u32(f, &t->line); r_u32(f, &t->column);
        r_u64(f, &t->byte_start); r_u64(f, &t->byte_end);
        t->trivia = r_str(f); t->text = r_str(f);
    }
    r_u64(f, &n);
    ir->nodes = (IRNode *)xcalloc((size_t)n, sizeof(*ir->nodes));
    ir->node_count = ir->node_cap = (size_t)n;
    for (i = 0; i < n; ++i) {
        IRNode *x = &ir->nodes[i];
        uint64_t q;
        uint32_t k;
        r_u32(f, &k); x->kind = (NodeKind)k;
        r_u32(f, &x->flags);
        r_u64(f, &x->first_tok); r_u64(f, &x->last_tok);
        r_u64(f, &q); x->parent = rd_i64(q);
        r_u64(f, &q); x->first_child = rd_i64(q);
        r_u64(f, &q); x->last_child = rd_i64(q);
        r_u64(f, &q); x->next_sibling = rd_i64(q);
        r_u64(f, &q); x->prev_sibling = rd_i64(q);
        x->name = r_str(f); x->qualified = r_str(f); x->type = r_str(f); x->return_type = r_str(f);
        x->operator_spelling = r_str(f); x->value = r_str(f); x->resolved_name = r_str(f);
        r_u64(f, &x->aux);
    }
    fclose(f);
}

/* ------------------------------------------------------------------------- */
/* Human-readable inspection                                                  */
/* ------------------------------------------------------------------------- */

static void dump_tree(const IR *ir, int64_t id, int depth) {
    size_t i;
    const IRNode *n = &ir->nodes[id];
    for (i = 0; i < (size_t)depth; ++i) fputs("  ", stdout);
    printf("%s [%" PRIu64 "..%" PRIu64 "]", node_kind_name(n->kind), n->first_tok, n->last_tok);
    if (n->name && *n->name) printf(" name=%s", n->name);
    if (n->qualified && *n->qualified) printf(" qualified=%s", n->qualified);
    if (n->type && *n->type) printf(" type=%s", n->type);
    if (n->return_type && *n->return_type) printf(" return=%s", n->return_type);
    if (n->resolved_name && *n->resolved_name) printf(" resolved=%s", n->resolved_name);
    if (n->kind == N_DECORATOR && n->aux != UINT64_MAX) printf(" target=%" PRIu64, n->aux);
    if (n->flags & NF_DECORATOR_CUSTOM) printf(" custom");
    if (n->flags & NF_REGISTERED) printf(" registered");
    if (n->flags & NF_EXPOSED) printf(" exposed");
    if ((n->flags & NF_ACCESS_MASK) == NF_ACCESS_PUBLIC && (n->kind == N_FIELD_DECL || n->kind == N_METHOD || n->kind == N_FUNCTION)) printf(" access=public");
    else if ((n->flags & NF_ACCESS_MASK) == NF_ACCESS_PROTECTED && (n->kind == N_FIELD_DECL || n->kind == N_METHOD || n->kind == N_FUNCTION)) printf(" access=protected");
    if (n->value && *n->value) {
        size_t len = strlen(n->value);
        printf(" value=");
        if (len > 120) fwrite(n->value, 1, 120, stdout), fputs("...", stdout);
        else fputs(n->value, stdout);
    }
    putchar('\n');
    for (id = n->first_child; id >= 0; id = ir->nodes[id].next_sibling) dump_tree(ir, id, depth + 1);
}

static void dump_ir(const IR *ir) {
    printf("CPIR version %u\n", CPIR_VERSION);
    printf("source bytes: %zu\n", ir->source_size);
    printf("tokens: %zu\n", ir->tokens.n);
    printf("nodes: %zu\n", ir->node_count);
    if (ir->node_count) dump_tree(ir, 0, 0);
}

static int command_usage(const char *command);

/* ------------------------------------------------------------------------- */
/* Embedded CXXE runtime                                                     */
/* ------------------------------------------------------------------------- */

static const char CXXE_RUNTIME_HPP[] =
"#ifndef CXXE_RUNTIME_HPP\n"
"#define CXXE_RUNTIME_HPP\n"
"\n"
"#include <any>\n"
"#include <memory>\n"
"#include <mutex>\n"
"#include <stdexcept>\n"
"#include <string>\n"
"#include <string_view>\n"
"#include <type_traits>\n"
"#include <typeindex>\n"
"#include <utility>\n"
"#include <vector>\n"
"namespace stde {\n"
"\n"
"class DynamicValue {\n"
"    std::any value_;\n"
"public:\n"
"    DynamicValue() = default;\n"
"    explicit DynamicValue(std::any value) : value_(std::move(value)) {}\n"
"    template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, DynamicValue>, int> = 0>\n"
"    DynamicValue(T&& value) : value_(std::forward<T>(value)) {}\n"
"\n"
"    template <class T> static DynamicValue from(T&& value) { return DynamicValue(std::forward<T>(value)); }\n"
"    bool has_value() const noexcept { return value_.has_value(); }\n"
"\n"
"    template <class T> bool is() const noexcept { return value_.has_value() && value_.type() == typeid(T); }\n"
"    template <class T> T& as() { if (!is<T>()) throw std::bad_any_cast(); return std::any_cast<T&>(value_); }\n"
"    template <class T> const T& as() const { if (!is<T>()) throw std::bad_any_cast(); return std::any_cast<const T&>(value_); }\n"
"    template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, DynamicValue>, int> = 0>\n"
"    DynamicValue& operator=(T&& value) { value_ = std::forward<T>(value); return *this; }\n"
"};\n"
"\n"
"namespace detail {\n"
"\n"
"template <class T>\n"
"std::any pack_argument(T&& value) {\n"
"    if constexpr (std::is_lvalue_reference_v<T&&>) {\n"
"        using U = std::remove_reference_t<T>;\n"
"        if constexpr (std::is_const_v<U>) return std::any(std::cref(value));\n"
"        else return std::any(std::ref(value));\n"
"    } else {\n"
"        return std::any(std::forward<T>(value));\n"
"    }\n"
"}\n"
"\n"
"template <class T>\n"
"decltype(auto) unpack_argument(const std::any& value) {\n"
"    using U = std::remove_reference_t<T>;\n"
"    using V = std::remove_cv_t<U>;\n"
"\n"
"    if constexpr (std::is_lvalue_reference_v<T>) {\n"
"        if constexpr (std::is_const_v<U>) {\n"
"            if (value.type() == typeid(std::reference_wrapper<const V>))\n"
"                return std::any_cast<std::reference_wrapper<const V>>(value).get();\n"
"            return std::any_cast<std::reference_wrapper<V>>(value).get();\n"
"        } else {\n"
"            return std::any_cast<std::reference_wrapper<V>>(value).get();\n"
"        }\n"
"    } else if constexpr (std::is_rvalue_reference_v<T>) {\n"
"        return std::move(std::any_cast<V&>(const_cast<std::any&>(value)));\n"
"    } else {\n"
"        return std::any_cast<std::decay_t<T>>(value);\n"
"    }\n"
"}\n"
"\n"
"} // namespace detail\n"
"\n"
"struct MemberInfo {\n"
"    enum class Kind { Field, Method };\n"
"    using Getter = DynamicValue (*)(void*);\n"
"    using Setter = void (*)(void*, const std::any&);\n"
"    using Invoker = DynamicValue (*)(void*, const std::vector<std::any>&);\n"
"\n"
"    Kind kind = Kind::Field;\n"
"    std::string name;\n"
"    Getter getter = nullptr;\n"
"    Setter setter = nullptr;\n"
"    Invoker invoker = nullptr;\n"
"};\n"
"\n"
"struct ClassInfo {\n"
"    using Create = void* (*)();\n"
"    using Destroy = void (*)(void*);\n"
"\n"
"    std::string name;\n"
"    std::type_index type = typeid(void);\n"
"    Create create = nullptr;\n"
"    Destroy destroy = nullptr;\n"
"    std::vector<std::shared_ptr<MemberInfo>> members;\n"
"};\n"
"\n"
"struct EnumValueInfo {\n"
"    std::string name;\n"
"    std::any value;\n"
"};\n"
"\n"
"struct EnumInfo {\n"
"    std::string name;\n"
"    std::type_index type = typeid(void);\n"
"    std::vector<EnumValueInfo> values;\n"
"};\n"
"\n"
"class EnumRegistry {\n"
"    mutable std::mutex mutex_;\n"
"    std::vector<std::shared_ptr<EnumInfo>> enums_;\n"
"\n"
"    EnumRegistry() = default;\n"
"public:\n"
"    EnumRegistry(const EnumRegistry&) = delete;\n"
"    EnumRegistry& operator=(const EnumRegistry&) = delete;\n"
"\n"
"    static EnumRegistry& instance() {\n"
"        static EnumRegistry registry;\n"
"        return registry;\n"
"    }\n"
"\n"
"    template <class E>\n"
"    std::shared_ptr<EnumInfo> register_enum(std::string_view name) {\n"
"        static_assert(std::is_enum_v<E>, \"stde::register_enum requires an enum type\");\n"
"        std::lock_guard<std::mutex> lock(mutex_);\n"
"        for (auto& item : enums_) {\n"
"            if (item->name == name && item->type == std::type_index(typeid(E))) {\n"
"                item->values.clear();\n"
"                return item;\n"
"            }\n"
"        }\n"
"        auto item = std::make_shared<EnumInfo>();\n"
"        item->name = std::string(name);\n"
"        item->type = std::type_index(typeid(E));\n"
"        enums_.push_back(item);\n"
"        return item;\n"
"    }\n"
"\n"
"    std::shared_ptr<EnumInfo> find(std::string_view name) const {\n"
"        std::lock_guard<std::mutex> lock(mutex_);\n"
"        for (const auto& item : enums_) if (item->name == name) return item;\n"
"        return {};\n"
"    }\n"
"\n"
"    std::shared_ptr<EnumInfo> find_type(std::type_index type) const {\n"
"        std::lock_guard<std::mutex> lock(mutex_);\n"
"        for (const auto& item : enums_) if (item->type == type) return item;\n"
"        return {};\n"
"    }\n"
"};\n"
"\n"
"class Registry {\n"
"    mutable std::mutex mutex_;\n"
"    std::vector<std::shared_ptr<ClassInfo>> classes_;\n"
"\n"
"    Registry() = default;\n"
"\n"
"public:\n"
"    Registry(const Registry&) = delete;\n"
"    Registry& operator=(const Registry&) = delete;\n"
"\n"
"    static Registry& instance() {\n"
"        static Registry registry;\n"
"        return registry;\n"
"    }\n"
"\n"
"    template <class T>\n"
"    std::shared_ptr<ClassInfo> register_class(std::string_view name) {\n"
"        std::lock_guard<std::mutex> lock(mutex_);\n"
"        for (auto& item : classes_) {\n"
"            if (item->name == name && item->type == std::type_index(typeid(T))) {\n"
"                item->create = make_create<T>();\n"
"                item->destroy = make_destroy<T>();\n"
"                item->members.clear();\n"
"                return item;\n"
"            }\n"
"        }\n"
"\n"
"        auto item = std::make_shared<ClassInfo>();\n"
"        item->name = std::string(name);\n"
"        item->type = std::type_index(typeid(T));\n"
"        item->create = make_create<T>();\n"
"        item->destroy = make_destroy<T>();\n"
"        classes_.push_back(item);\n"
"        return item;\n"
"    }\n"
"\n"
"    std::shared_ptr<ClassInfo> find(std::string_view name) const {\n"
"        std::lock_guard<std::mutex> lock(mutex_);\n"
"        for (const auto& item : classes_)\n"
"            if (item->name == name) return item;\n"
"        return {};\n"
"    }\n"
"\n"
"    std::shared_ptr<ClassInfo> find_type(std::type_index type) const {\n"
"        std::lock_guard<std::mutex> lock(mutex_);\n"
"        for (const auto& item : classes_)\n"
"            if (item->type == type) return item;\n"
"        return {};\n"
"    }\n"
"\n"
"private:\n"
"    template <class T>\n"
"    static ClassInfo::Create make_create() {\n"
"        return []() -> void* {\n"
"            if constexpr (std::is_default_constructible_v<T>) return static_cast<void*>(new T());\n"
"            else return nullptr;\n"
"        };\n"
"    }\n"
"\n"
"    template <class T>\n"
"    static ClassInfo::Destroy make_destroy() {\n"
"        return [](void* p) { delete static_cast<T*>(p); };\n"
"    }\n"
"};\n"
"\n"
"template <class T>\n"
"class ClassBuilder {\n"
"    std::shared_ptr<ClassInfo> info_;\n"
"public:\n"
"    explicit ClassBuilder(std::shared_ptr<ClassInfo> info) : info_(std::move(info)) {}\n"
"\n"
"    ClassBuilder& expose_field(std::string_view name, MemberInfo::Getter getter, MemberInfo::Setter setter = nullptr) {\n"
"        auto member = std::make_shared<MemberInfo>();\n"
"        member->kind = MemberInfo::Kind::Field;\n"
"        member->name = std::string(name);\n"
"        member->getter = getter;\n"
"        member->setter = setter;\n"
"        info_->members.push_back(std::move(member));\n"
"        return *this;\n"
"    }\n"
"\n"
"    ClassBuilder& expose_method(std::string_view name, MemberInfo::Invoker invoker) {\n"
"        auto member = std::make_shared<MemberInfo>();\n"
"        member->kind = MemberInfo::Kind::Method;\n"
"        member->name = std::string(name);\n"
"        member->invoker = invoker;\n"
"        info_->members.push_back(std::move(member));\n"
"        return *this;\n"
"    }\n"
"};\n"
"\n"
"template <class T>\n"
"ClassBuilder<T> register_class(std::string_view name) {\n"
"    return ClassBuilder<T>(Registry::instance().register_class<T>(name));\n"
"}\n"
"\n"
"\n"
"template <class E>\n"
"class EnumBuilder {\n"
"    std::shared_ptr<EnumInfo> info_;\n"
"public:\n"
"    explicit EnumBuilder(std::shared_ptr<EnumInfo> info) : info_(std::move(info)) {}\n"
"\n"
"    EnumBuilder& value(std::string_view name, E value) {\n"
"        if (!info_) throw std::runtime_error(\"CXXE: invalid enum builder\");\n"
"        EnumValueInfo item;\n"
"        item.name = std::string(name);\n"
"        item.value = value;\n"
"        info_->values.push_back(std::move(item));\n"
"        return *this;\n"
"    }\n"
"};\n"
"\n"
"template <class E>\n"
"EnumBuilder<E> register_enum(std::string_view name) {\n"
"    return EnumBuilder<E>(EnumRegistry::instance().register_enum<E>(name));\n"
"}\n"
"\n"
"template <class E>\n"
"std::shared_ptr<EnumInfo> get_enum_info() {\n"
"    static_assert(std::is_enum_v<E>, \"stde::get_enum_info requires an enum type\");\n"
"    return EnumRegistry::instance().find_type(std::type_index(typeid(E)));\n"
"}\n"
"\n"
"template <class E>\n"
"std::string enum_to_string(E value) {\n"
"    static_assert(std::is_enum_v<E>, \"stde::enum_to_string requires an enum type\");\n"
"    auto info = get_enum_info<E>();\n"
"    if (!info) throw std::runtime_error(\"CXXE: enum type is not registered\");\n"
"    for (const auto& item : info->values) {\n"
"        if (std::any_cast<E>(item.value) == value) return item.name;\n"
"    }\n"
"    throw std::runtime_error(\"CXXE: enum value is not registered: \" + std::to_string(static_cast<long long>(value)));\n"
"}\n"
"\n"
"template <class E>\n"
"E string_to_enum(std::string_view name) {\n"
"    static_assert(std::is_enum_v<E>, \"stde::string_to_enum requires an enum type\");\n"
"    auto info = get_enum_info<E>();\n"
"    if (!info) throw std::runtime_error(\"CXXE: enum type is not registered\");\n"
"    for (const auto& item : info->values) {\n"
"        if (item.name == name) return std::any_cast<E>(item.value);\n"
"    }\n"
"    throw std::runtime_error(\"CXXE: enum value not found: \" + std::string(name));\n"
"}\n"
"\n"
"template <class E>\n"
"bool enum_has_value(E value) {\n"
"    static_assert(std::is_enum_v<E>, \"stde::enum_has_value requires an enum type\");\n"
"    auto info = get_enum_info<E>();\n"
"    if (!info) return false;\n"
"    for (const auto& item : info->values) if (std::any_cast<E>(item.value) == value) return true;\n"
"    return false;\n"
"}\n"
"\n"
"template <class E>\n"
"bool enum_has_name(std::string_view name) {\n"
"    static_assert(std::is_enum_v<E>, \"stde::enum_has_name requires an enum type\");\n"
"    auto info = get_enum_info<E>();\n"
"    if (!info) return false;\n"
"    for (const auto& item : info->values) if (item.name == name) return true;\n"
"    return false;\n"
"}\n"
"\n"
"template <class E>\n"
"std::vector<std::string> enum_names() {\n"
"    static_assert(std::is_enum_v<E>, \"stde::enum_names requires an enum type\");\n"
"    auto info = get_enum_info<E>();\n"
"    if (!info) throw std::runtime_error(\"CXXE: enum type is not registered\");\n"
"    std::vector<std::string> result;\n"
"    result.reserve(info->values.size());\n"
"    for (const auto& item : info->values) result.push_back(item.name);\n"
"    return result;\n"
"}\n"
"\n"
"class FactoryResult {    void* pointer_ = nullptr;\n"
"    std::shared_ptr<ClassInfo> info_;\n"
"public:\n"
"    FactoryResult() = default;\n"
"    FactoryResult(void* pointer, std::shared_ptr<ClassInfo> info)\n"
"        : pointer_(pointer), info_(std::move(info)) {}\n"
"\n"
"    void* raw() const noexcept { return pointer_; }\n"
"\n"
"    template <class T>\n"
"    T* as() const {\n"
"        if (!pointer_) return nullptr;\n"
"        if (!info_ || info_->type != std::type_index(typeid(T)))\n"
"            throw std::runtime_error(\"CXXE factory type mismatch\");\n"
"        return static_cast<T*>(pointer_);\n"
"    }\n"
"\n"
"    template <class T>\n"
"    operator T*() const { return as<T>(); }\n"
"};\n"
"\n"
"inline FactoryResult factory_new(std::string_view name) {\n"
"    auto info = Registry::instance().find(name);\n"
"    if (!info) throw std::runtime_error(\"CXXE: factory class not registered: \" + std::string(name));\n"
"    if (!info->create) throw std::runtime_error(\"CXXE: class cannot be constructed: \" + std::string(name));\n"
"    void* object = info->create();\n"
"    if (!object) throw std::runtime_error(\"CXXE: class has no default constructor: \" + std::string(name));\n"
"    return FactoryResult(object, std::move(info));\n"
"}\n"
"\n"
"inline std::shared_ptr<ClassInfo> factory_find(std::string_view name) {\n"
"    auto info = Registry::instance().find(name);\n"
"    if (!info) throw std::runtime_error(\"CXXE: factory class not registered: \" + std::string(name));\n"
"    return info;\n"
"}\n"
"\n"
"class MemberProxy {\n"
"    void* object_ = nullptr;\n"
"    std::type_index type_ = typeid(void);\n"
"    std::string name_;\n"
"\n"
"    std::shared_ptr<ClassInfo> class_info() const {\n"
"        return Registry::instance().find_type(type_);\n"
"    }\n"
"\n"
"    std::shared_ptr<MemberInfo> find_member() const {\n"
"        auto info = class_info();\n"
"        if (!info) throw std::runtime_error(\"CXXE: object class is not registered\");\n"
"        for (const auto& member : info->members)\n"
"            if (member->name == name_) return member;\n"
"        throw std::runtime_error(\"CXXE: exposed member not found: \" + name_);\n"
"    }\n"
"\n"
"public:\n"
"    MemberProxy() = default;\n"
"    MemberProxy(void* object, std::type_index type, std::string_view name)\n"
"        : object_(object), type_(type), name_(name) {}\n"
"\n"
"    template <class T>\n"
"    operator T() const {\n"
"        auto member = find_member();\n"
"        if (member->kind != MemberInfo::Kind::Field || !member->getter)\n"
"            throw std::runtime_error(\"CXXE: exposed member is not a field: \" + name_);\n"
"        return member->getter(object_).template as<T>();\n"
"    }\n"
"\n"
"    template <class T>\n"
"    MemberProxy& operator=(T&& value) {\n"
"        auto member = find_member();\n"
"        if (member->kind != MemberInfo::Kind::Field)\n"
"            throw std::runtime_error(\"CXXE: exposed member is not a field: \" + name_);\n"
"        if (!member->setter)\n"
"            throw std::runtime_error(\"CXXE: exposed field is read-only: \" + name_);\n"
"        member->setter(object_, std::any(std::forward<T>(value)));\n"
"        return *this;\n"
"    }\n"
"\n"
"    template <class... Args>\n"
"    DynamicValue operator()(Args&&... args) const {\n"
"        auto info = class_info();\n"
"        if (!info) throw std::runtime_error(\"CXXE: object class is not registered\");\n"
"        std::vector<std::any> packed;\n"
"        packed.reserve(sizeof...(Args));\n"
"        (packed.emplace_back(detail::pack_argument(std::forward<Args>(args))), ...);\n"
"\n"
"        auto member = find_member();\n"
"        if (member->kind != MemberInfo::Kind::Method || !member->invoker)\n"
"            throw std::runtime_error(\"CXXE: exposed member is not a method: \" + name_);\n"
"\n"
"        try {\n"
"            return member->invoker(object_, packed);\n"
"        } catch (const std::bad_any_cast&) {\n"
"            throw;\n"
"        }\n"
"    }\n"
"};\n"
"\n"
"template <class T>\n"
"MemberProxy get_member(T* object, std::string_view name) {\n"
"    if (!object) throw std::runtime_error(\"CXXE: get_member called with null object\");\n"
"    return MemberProxy(static_cast<void*>(object), std::type_index(typeid(T)), name);\n"
"}\n"
"\n"
"template <class T>\n"
"MemberProxy get_member(T& object, std::string_view name) {\n"
"    return MemberProxy(static_cast<void*>(std::addressof(object)), std::type_index(typeid(T)), name);\n"
"}\n"
"\n"
"} // namespace stde\n"
"\n"
"#endif\n"
;

static const char CXXE_RUNTIME_CPP[] =
"#include \"cxxe_runtime.hpp\"\n"
"\n"
"// CXXE runtime is intentionally small and header-driven. This translation\n"
"// unit exists so projects can build a conventional runtime library (static\n"
"// or shared) instead of relying on header-only usage.\n"
;

#ifdef _WIN32
#include <direct.h>
#define CXXE_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define CXXE_MKDIR(path) mkdir((path), 0777)
#endif

static int cxxe_write_text_file(const char *path, const char *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cxxe: error: cannot create '%s': %s\\n", path, strerror(errno));
        return 0;
    }
    if (size && fwrite(data, 1, size, f) != size) {
        fclose(f);
        fprintf(stderr, "cxxe: error: cannot write '%s'\\n", path);
        return 0;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "cxxe: error: cannot close '%s'\\n", path);
        return 0;
    }
    return 1;
}

static int cxxe_mkdir_one(const char *path) {
    if (!path || !*path) return 1;
    if (CXXE_MKDIR(path) == 0) return 1;
    if (errno == EEXIST) return 1;
    return 0;
}

static int cxxe_make_directory_recursive(const char *path) {
    char *tmp;
    size_t n, i;
    if (!path || !*path) return 0;
    tmp = xstrdup0(path);
    n = strlen(tmp);
    for (i = 0; i < n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            if (i == 0) continue;
            if (i == 2 && tmp[1] == ':') continue; /* Windows drive prefix. */
            tmp[i] = 0;
            if (*tmp && !cxxe_mkdir_one(tmp)) {
                free(tmp);
                return 0;
            }
            tmp[i] = saved;
        }
    }
    if (!cxxe_mkdir_one(tmp)) {
        free(tmp);
        return 0;
    }
    free(tmp);
    return 1;
}

static int cxxe_runtime_emit_one(const char *kind, const char *output) {
    const char *data;
    size_t size;
    if (!kind || !output) return 0;
    if (strcmp(kind, "header") == 0) {
        data = CXXE_RUNTIME_HPP;
        size = sizeof(CXXE_RUNTIME_HPP) - 1;
    } else if (strcmp(kind, "source") == 0) {
        data = CXXE_RUNTIME_CPP;
        size = sizeof(CXXE_RUNTIME_CPP) - 1;
    } else {
        fprintf(stderr, "cxxe: error: unknown runtime artifact '%s'\\n", kind);
        return 0;
    }
    if (strcmp(output, "-") == 0) {
        if (size && fwrite(data, 1, size, stdout) != size) return 0;
        return 1;
    }
    return cxxe_write_text_file(output, data, size);
}

static int cxxe_runtime_export(const char *directory) {
    char *hpp, *cpp;
    size_t n;
    if (!directory || !*directory) return 0;
    if (!cxxe_make_directory_recursive(directory)) {
        fprintf(stderr, "cxxe: error: cannot create runtime directory '%s': %s\\n", directory, strerror(errno));
        return 0;
    }
    n = strlen(directory);
    hpp = (char *)xmalloc(n + strlen("/cxxe_runtime.hpp") + 1);
    cpp = (char *)xmalloc(n + strlen("/cxxe_runtime.cpp") + 1);
    sprintf(hpp, "%s/cxxe_runtime.hpp", directory);
    sprintf(cpp, "%s/cxxe_runtime.cpp", directory);
    if (!cxxe_runtime_emit_one("header", hpp) || !cxxe_runtime_emit_one("source", cpp)) {
        free(hpp); free(cpp);
        return 0;
    }
    free(hpp);
    free(cpp);
    return 1;
}

static int cxxe_runtime_command(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "cxxe: error: runtime expects 'export', 'header', or 'source'\\n");
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf(
            "Usage: cxxe runtime <command> <output>\n"
            "\n"
            "Commands:\n"
            "  export <directory>    Write cxxe_runtime.hpp and cxxe_runtime.cpp\n"
            "  header <output.hpp|->  Write only cxxe_runtime.hpp\n"
            "  source <output.cpp|->  Write only cxxe_runtime.cpp\n"
            "\n"
            "Use '-' as the output path to write the requested artifact to stdout.\n");
        return 0;
    }
    if (strcmp(argv[1], "export") == 0) {
        if (argc != 3 || !*argv[2]) return command_usage("runtime export");
        return cxxe_runtime_export(argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "header") == 0) {
        if (argc != 3 || !*argv[2]) return command_usage("runtime header");
        return cxxe_runtime_emit_one("header", argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "source") == 0) {
        if (argc != 3 || !*argv[2]) return command_usage("runtime source");
        return cxxe_runtime_emit_one("source", argv[2]) ? 0 : 1;
    }
    fprintf(stderr, "cxxe: error: unknown runtime command '%s'\\n", argv[1]);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\\n");
    return 2;
}

/* ------------------------------------------------------------------------- */
/* Entry points                                                               */
/* ------------------------------------------------------------------------- */

static void print_version(void) {
    printf("CXXE 0.1\n");
}

static void usage(FILE *stream) {
    fprintf(stream,
        "CXXE 0.1 - C++ Extended Compiler - C++ source <-> CPIR converter\n"
        "\n"
        "Usage:\n"
        "  cxxe [options] <command> [arguments]\n"
        "\n"
        "Commands:\n"
        "  parse <input.cpp> <output.cpir>    Parse C++ into CPIR\n"
        "  emit <input.cpir> <output.cpp>     Generate C++ from CPIR\n"
        "  roundtrip <input.cpp> <output.cpp> Parse then generate C++\n"
        "  dump <input.cpir>                  Display CPIR information\n"
        "  runtime export <directory>          Export cxxe_runtime.hpp/.cpp\n"
        "  runtime header <output.hpp|->       Export only the runtime header\n"
        "  runtime source <output.cpp|->       Export only the runtime source\n"
        "\n"
        "Options:\n"
        "  -h, --help                         Show this help message\n"
        "  -v, --version                      Show version\n"
        "  -I <dir>                           Add an include search directory\n"
        "\n"
        "Examples:\n"
        "  cxxe parse main.cpp main.cpir\n"
        "  cxxe emit main.cpir main.cpp\n"
        "  cxxe roundtrip main.cpp generated.cpp\n"
        "  cxxe dump main.cpir\n");
}

static int invalid_option(const char *arg) {
    fprintf(stderr, "CXXE: error: invalid option '%s'\n", arg);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\n");
    return 2;
}

static int command_usage(const char *command) {
    fprintf(stderr, "cxxe: error: invalid arguments for command '%s'\n", command);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\n");
    return 2;
}

static void build_ir_from_cpp(IR *ir, const char *path, const char *const *include_dirs, size_t include_dir_n) {
    ir_init(ir);
    ir->source = read_bytes(path, &ir->source_size);
    lex_cpp(ir->source, ir->source_size, &ir->tokens);
    parse_source(ir);
    discover_included_decorators(ir, path, include_dirs, include_dir_n);
    resolve_native_features(ir);
    resolve_named_args(ir);
}

int main(int argc, char **argv) {
    IR ir;
    const char **include_dirs = NULL;
    size_t include_dir_n = 0, include_dir_cap = 0;
    int argi = 1;
    const char *command;

    g_cxxe_executable_dir = cxxe_executable_dir0(argv[0]);

    if (argc < 2) { usage(stderr); free(g_cxxe_executable_dir); g_cxxe_executable_dir = NULL; return 2; }

    while (argi < argc) {
        if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            if (argi == 1) { usage(stdout); free(include_dirs); return 0; }
            break;
        }
        if (strcmp(argv[argi], "--version") == 0 || strcmp(argv[argi], "-v") == 0) {
            if (argi == 1) { print_version(); free(include_dirs); return 0; }
            break;
        }
        if (strcmp(argv[argi], "-I") == 0) {
            if (argi + 1 >= argc) { free(include_dirs); return invalid_option("-I"); }
            if (include_dir_n == include_dir_cap) {
                include_dir_cap = include_dir_cap ? include_dir_cap * 2 : 8;
                include_dirs = (const char **)xrealloc((void *)include_dirs, include_dir_cap * sizeof(*include_dirs));
            }
            include_dirs[include_dir_n++] = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(argv[argi], "-I", 2) == 0 && argv[argi][2] != 0) {
            if (include_dir_n == include_dir_cap) {
                include_dir_cap = include_dir_cap ? include_dir_cap * 2 : 8;
                include_dirs = (const char **)xrealloc((void *)include_dirs, include_dir_cap * sizeof(*include_dirs));
            }
            include_dirs[include_dir_n++] = argv[argi] + 2;
            argi++;
            continue;
        }
        break;
    }

    if (argi >= argc) { free(include_dirs); usage(stderr); return 2; }
    command = argv[argi++];

    if (strcmp(command, "runtime") == 0) {
        int r = cxxe_runtime_command(argc - argi + 1, argv + argi - 1);
        free(include_dirs);
        return r;
    }

    if (strcmp(command, "parse") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 2) { free(include_dirs); return command_usage("parse"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        if (argv[argi + 1][0] == '-') { int r = invalid_option(argv[argi + 1]); free(include_dirs); return r; }
        build_ir_from_cpp(&ir, argv[argi], include_dirs, include_dir_n);
        save_ir(&ir, argv[argi + 1]);
        printf("cxxe: parsed %s -> %s (%zu nodes, %zu tokens)\n", argv[argi], argv[argi + 1], ir.node_count, ir.tokens.n);
        ir_free(&ir); free(include_dirs); return 0;
    }

    if (strcmp(command, "emit") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 2) { free(include_dirs); return command_usage("emit"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        if (argv[argi + 1][0] == '-') { int r = invalid_option(argv[argi + 1]); free(include_dirs); return r; }
        ir_init(&ir); load_ir(&ir, argv[argi]); emit_exact(&ir, argv[argi + 1]);
        printf("cxxe: emitted %s -> %s\n", argv[argi], argv[argi + 1]);
        ir_free(&ir); free(include_dirs); return 0;
    }

    if (strcmp(command, "roundtrip") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 2) { free(include_dirs); return command_usage("roundtrip"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        if (argv[argi + 1][0] == '-') { int r = invalid_option(argv[argi + 1]); free(include_dirs); return r; }
        build_ir_from_cpp(&ir, argv[argi], include_dirs, include_dir_n);
        save_ir(&ir, ".cxxe.tmp.cpir");
        {
            IR reloaded; ir_init(&reloaded); load_ir(&reloaded, ".cxxe.tmp.cpir"); emit_exact(&reloaded, argv[argi + 1]); ir_free(&reloaded);
        }
        remove(".cxxe.tmp.cpir"); ir_free(&ir);
        printf("cxxe: round-trip %s -> %s\n", argv[argi], argv[argi + 1]);
        free(include_dirs); return 0;
    }

    if (strcmp(command, "dump") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 1) { free(include_dirs); return command_usage("dump"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        ir_init(&ir); load_ir(&ir, argv[argi]); dump_ir(&ir); ir_free(&ir); free(include_dirs); return 0;
    }

    free(include_dirs);
    fprintf(stderr, "cxxe: error: unknown command '%s'\n", command);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\n");
    return 2;
}
