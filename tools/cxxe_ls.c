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
#include <setjmp.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#define CPIR_MAGIC "CPIR"
#define CPIR_VERSION 11u
#define ARRAY_GROW_MIN 64u

/* Standalone executable: enable the LSP build automatically. */
#ifndef CXXE_LSP_BUILD
#define CXXE_LSP_BUILD 1
#endif

/* ------------------------------------------------------------------------- */
/* Memory / diagnostics                                                      */
/* ------------------------------------------------------------------------- */

#ifdef CXXE_LSP_BUILD
static jmp_buf g_lsp_fatal_jmp;
static char g_lsp_fatal_message[4096];
static int g_lsp_in_parse = 0;
#endif

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#ifdef CXXE_LSP_BUILD
    vsnprintf(g_lsp_fatal_message, sizeof(g_lsp_fatal_message), fmt, ap);
    va_end(ap);
    if (g_lsp_in_parse) longjmp(g_lsp_fatal_jmp, 1);
#else
    fprintf(stderr, "cxxe: error: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
#endif
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
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

static void node_set_value_range(Parser *p, int64_t id, size_t a, size_t b) {
    char *s = token_range_text(p, a, b);
    node_set_value(p->ir, id, s);
    free(s);
}

static void node_set_type_range(Parser *p, int64_t id, size_t a, size_t b) {
    char *s = token_range_text(p, a, b);
    node_set_type(p->ir, id, s);
    free(s);
}

static void node_set_return_range(Parser *p, int64_t id, size_t a, size_t b) {
    char *s = token_range_text(p, a, b);
    node_set_return(p->ir, id, s);
    free(s);
}

static void node_set_qualified_scope(Parser *p, int64_t id, const char *name) {
    char *s = scope_qualified(p, name);
    node_set_qualified(p->ir, id, s);
    free(s);
}

static void node_set_name_range_owned(IR *ir, int64_t id, char *s) {
    node_set_name(ir, id, s);
    free(s);
}

static void node_set_resolved_owned_scope(Parser *p, int64_t id, const char *name) {
    char *s = scope_qualified(p, name);
    node_set_resolved(p->ir, id, s);
    free(s);
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
    node_set_value_range(p, tpl, open, close);
    ir_add_child(p->ir, parent, tpl);
    if (close <= open + 1) return;
    n = split_template_args_parser(p, open + 1, close - 1, starts, ends, 128);
    for (i = 0; i < n; ++i) {
        int64_t arg = ir_add_node(p->ir, N_DECORATOR_TEMPLATE_ARG, starts[i], ends[i], tpl);
        node_set_value_range(p, arg, starts[i], ends[i]);
        ir_add_child(p->ir, tpl, arg);
    }
}

static void parse_custom_decorator_template_decl(Parser *p, int64_t parent,
                                                  size_t template_i, size_t template_close) {
    int64_t tpl = ir_add_node(p->ir, N_TEMPLATE, template_i, template_close, parent);
    size_t open = template_i + 1, starts[128], ends[128], n = 0, i;
    node_set_value_range(p, tpl, template_i, template_close);
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
            node_set_value_range(p, pn, aa, bb);
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
        node_set_value_range(p, n, a, end);
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
                    node_set_value_range(p, an, start, e);
                }
                ir_add_child(p->ir, call, an);
            }
            start = i + 1;
        }
        i++;
    }
}

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

static int resolve_property_for_member_ir(const IR *ir, size_t sep_tok, const char *receiver,
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
    if (!type.len) { str_free(&type); return 0; }
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *p = &ir->nodes[i];
        if (p->kind != N_PROPERTY || !p->name || strcmp(p->name, member)) continue;
        if (property_matches_type_context(ir, fn_id, type.data, (int64_t)i)) {
            if (out_id) *out_id = (int64_t)i;
            str_free(&type);
            return 1;
        }
    }
    str_free(&type);
    return 0;
}

static int find_property_for_parser_member(Parser *p, size_t sep_tok, const char *member, int64_t *out_id) {
    const char *receiver;
    if (!p || sep_tok == 0 || sep_tok >= p->ir->tokens.n || !member) return 0;
    receiver = p->ir->tokens.v[sep_tok - 1].text;
    return resolve_property_for_member_ir(p->ir, sep_tok, receiver, member, out_id);
}

static void add_expr_nodes(Parser *p, int64_t parent, size_t a, size_t b) {
    IR *ir = p->ir;
    size_t i;
    if (a > b || b >= ir->tokens.n) return;

    /* Assignment nodes describe one expression/statement range. This helper
       is also called for an entire compound function body, so creating one
       node merely because an `=` exists somewhere in that body produces a
       span over all following statements. The emitter then sees that broad
       native node and skips unrelated constructs. Only create the node when
       the range contains at most one top-level statement terminator and no
       top-level block boundary. */
    {
        size_t op, j;
        int top_semis = 0, top_blocks = 0;
        int par = 0, br = 0, cur = 0;
        for (j = a; j <= b; ++j) {
            const char *jt = pt(p, j);
            if (str_eq(jt, "(")) par++;
            else if (str_eq(jt, ")") && par) par--;
            else if (str_eq(jt, "[")) br++;
            else if (str_eq(jt, "]") && br) br--;
            else if (str_eq(jt, "{")) { if (!par && !br) top_blocks++; cur++; }
            else if (str_eq(jt, "}") && cur) { cur--; }
            else if (str_eq(jt, ";") && !par && !br && !cur) top_semis++;
        }
        if (!top_blocks && top_semis <= 1 && looks_like_assignment(p, a, b, &op)) {
            int64_t n = ir_add_node(ir, N_ASSIGN_EXPR, a, b, parent);
            ir->nodes[n].flags = 1;
            node_set_name(ir, n, pt(p, op));
            node_set_value_range(p, n, a, b);
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
                    node_set_value_range(p, mn, i, close);
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
            if (i > a && find_property_for_parser_member(p, i, pt(p, i + 1), &prop)) {
                int64_t n = ir_add_node(ir, N_PROPERTY_ACCESS, i - 1, i + 1, parent);
                node_set_name(ir, n, pt(p, i + 1));
                node_set_qualified(ir, n, ir->nodes[prop].qualified);
                ir->nodes[n].aux = (uint64_t)prop;
                node_set_value_range(p, n, i - 1, i + 1);
                ir_add_child(ir, parent, n);
                /* Postfix property mutation is a distinct expression. Keep the
                   full `receiver.member++/--` span so the backend can lower it
                   without ever producing `get_member()++`. */
                if (i + 2 <= b && (str_eq(pt(p, i + 2), "++") || str_eq(pt(p, i + 2), "--"))) {
                    int64_t un = ir_add_node(ir, N_UNARY_EXPR, i - 1, i + 2, parent);
                    node_set_name(ir, un, pt(p, i + 2));
                    node_set_value_range(p, un, i - 1, i + 2);
                    ir->nodes[un].aux = (uint64_t)prop;
                    ir_add_child(ir, parent, un);
                }
            }
        } else if ((str_eq(t, "++") || str_eq(t, "--")) && i + 3 <= b &&
                   is_identifier(p, i + 1) && (str_eq(pt(p, i + 2), ".") || str_eq(pt(p, i + 2), "->")) &&
                   pk(p, i + 3) == TK_IDENTIFIER) {
            int64_t prop = -1;
            if (find_property_for_parser_member(p, i + 2, pt(p, i + 3), &prop)) {
                int64_t un = ir_add_node(ir, N_UNARY_EXPR, i, i + 3, parent);
                node_set_name(ir, un, t);
                node_set_value_range(p, un, i, i + 3);
                ir->nodes[un].aux = (uint64_t)prop;
                ir_add_child(ir, parent, un);
                i += 3;
                continue;
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
                    node_set_value_range(p, n, i, close);
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
                        node_set_value_range(p, n, i, close);
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
                node_set_resolved_owned_scope(p, n, qname);
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
                node_set_value_range(p, n, i, close);
                ir_add_child(ir, parent, n);
                i = close;
            }
        }

        if (str_eq(t, "new")) {
            int64_t n = ir_add_node(ir, N_NEW_EXPR, i, b, parent);
            node_set_value_range(p, n, i, b);
            ir_add_child(ir, parent, n);
        } else if (str_eq(t, "delete")) {
            int64_t n = ir_add_node(ir, N_DELETE_EXPR, i, b, parent);
            node_set_value_range(p, n, i, b);
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
            node_set_qualified_scope(p, n, pt(p, i));
            break;
        }
        if (i == a) break;
    }
    if (eq != SIZE_MAX && eq > a) node_set_value_range(p, n, eq + 1, b);
    else node_set_value_range(p, n, a, b);
    {
        size_t name_i = SIZE_MAX;
        for (i = a; i <= b; ++i) if (is_identifier(p, i) && strcmp(pt(p, i), p->ir->nodes[n].name ? p->ir->nodes[n].name : "") == 0) { name_i = i; break; }
        if (name_i != SIZE_MAX && name_i > a) node_set_type_range(p, n, a, name_i - 1);
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
        if (name_i > a) node_set_return_range(p, fn, a, name_i - 1);
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
                node_set_value_range(p, n, i, j + 1);
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
            if (e) node_set_name_range_owned(p->ir, n, xstrndup0(q + 1, (size_t)(e - (q + 1))));
        } else if (*q == '<') {
            const char *e = strchr(q + 1, '>');
            if (e) node_set_name_range_owned(p->ir, n, xstrndup0(q + 1, (size_t)(e - (q + 1))));
        } else if (*q) {
            const char *e = q;
            while (*e && !isspace((unsigned char)*e)) e++;
            node_set_name_range_owned(p->ir, n, xstrndup0(q, (size_t)(e - q)));
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
            if (q > name_start) node_set_name_range_owned(p->ir, n, xstrndup0(name_start, (size_t)(q - name_start)));
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
        node_set_value_range(p, n, a, b);
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
        node_set_qualified_scope(p, prop, pt(p, name_i));
        node_set_value_range(p, prop, a, close);
        mark_access(p->ir, prop, access);
        ir_add_child(p->ir, cls, prop);
        for (i = brace + 1; i < close; ) {
            if ((str_eq(pt(p, i), "get") || str_eq(pt(p, i), "set")) && i + 1 < close && str_eq(pt(p, i + 1), "{")) {
                size_t oc = find_matching(p, i + 1, close + 1);
                if (oc <= close) {
                    NodeKind nk = str_eq(pt(p, i), "get") ? N_PROPERTY_GET : N_PROPERTY_SET;
                    int64_t child = ir_add_node(p->ir, nk, i, oc, prop);
                    node_set_name(p->ir, child, pt(p, i));
                    node_set_value_range(p, child, i, oc);
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
                    if(pn>start && (str_eq(pt(p,pn-1),".")||str_eq(pt(p,pn-1),"->")) &&
                       resolve_property_for_member_ir(p->ir, pn - 1, pt(p, pn - 2), pt(p, pn), &pid)){
                        int64_t n=ir_add_node(p->ir,N_ASSIGN_EXPR,start,end,parent);
                        node_set_name(p->ir,n,pt(p,op));
                        node_set_value_range(p, n, start, end);
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
                node_set_value_range(p, raw, start, i - 1);
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
                /* Consume exactly a member-function body here. The general
                   find_stmt_end() helper intentionally continues through
                   nested braces until a top-level semicolon, but a class
                   body has subsequent members that must remain visible. */
                if (str_eq(pt(p, i), "{")) {
                    size_t body_close = find_matching(p, i, b + 1);
                    if (body_close <= b) e = body_close;
                }
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
        node_set_value_range(p, raw, start, b);
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
        node_set_value_range(p, n, a, b);
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
        node_set_value_range(p, n, a, close);
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
        node_set_qualified_scope(p, en, pt(p, name_i));
    }
    ir_add_child(p->ir, parent, en);
    size_t s = brace + 1;
    for (i = brace + 1; i <= close; ++i) {
        if (str_eq(pt(p, i), ",") || i == close) {
            if (s < i) {
                int64_t e = ir_add_node(p->ir, N_ENUMERATOR, s, i == close ? i - 1 : i - 1, en);
                if (is_identifier(p, s)) node_set_name(p->ir, e, pt(p, s));
                node_set_value_range(p, e, s, i - 1);
                ir_add_child(p->ir, en, e);
            }
            s = i + 1;
        }
    }
}

static int looks_like_var_decl(Parser *p, size_t a, size_t b) {
    size_t i, semi = b;
    if (a > b) return 0;
    if (str_eq(pt(p, a), "return") || str_eq(pt(p, a), "if") || str_eq(pt(p, a), "for") ||
        str_eq(pt(p, a), "while") || str_eq(pt(p, a), "switch") || str_eq(pt(p, a), "throw")) return 0;

    /* Existing declaration forms with an obvious type/name boundary. */
    for (i = a; i <= b; ++i) {
        if (str_eq(pt(p, i), ";")) { semi = i; break; }
        if (str_eq(pt(p, i), "=") || str_eq(pt(p, i), ":")) continue;
        if (pk(p, i) == TK_IDENTIFIER || pk(p, i) == TK_PUNCT) {
            if (i > a && is_identifier(p, i) &&
                (str_eq(pt(p, i - 1), "*") || str_eq(pt(p, i - 1), "&") || is_keyword(pt(p, i - 1)))) return 1;
        }
    }
    if (is_keyword(pt(p, a)) || str_eq(pt(p, a), "dynamic")) return 1;

    /* Qualified / user-defined local declarations such as
       `Outer::Player p;` and `Character c;`. At this stage semantic lookup is
       deliberately deferred; the parser only needs the unambiguous syntactic
       property that the final declarator name is an identifier not preceded
       by member/qualified-access punctuation. Expression statements such as
       `object.member;` and `object->member;` therefore stay expressions. */
    if (semi > a) {
        size_t last = semi;
        if (last > a && str_eq(pt(p, last), ";")) --last;
        if (last > a && is_identifier(p, last) &&
            !str_eq(pt(p, last - 1), ".") && !str_eq(pt(p, last - 1), "->") &&
            !str_eq(pt(p, last - 1), "::")) {
            int has_type_tokens = 0;
            for (i = a; i < last; ++i) {
                if (is_identifier(p, i) || str_eq(pt(p, i), "::") || str_eq(pt(p, i), "*") ||
                    str_eq(pt(p, i), "&") || str_eq(pt(p, i), "const") || str_eq(pt(p, i), "volatile")) {
                    has_type_tokens = 1;
                    break;
                }
            }
            if (has_type_tokens) return 1;
        }
    }
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
        else if (str_eq(t, "{") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            /* An ordinary top-level function/class body proves this is not a
               custom decorator declaration. */
            return 0;
        }
        else if (str_eq(t, "{")) cur++;
        else if (str_eq(t, "}") && cur) cur--;
        else if (str_eq(t, "<")) angle++;
        else if (str_eq(t, ">") && angle) angle--;
        else if (str_eq(t, ";") && par == 0 && br == 0 && cur == 0 && angle == 0) {
            /* A completed top-level declaration before `@` is not a custom
               decorator declaration. */
            /* A custom decorator declaration must reach `@` before it reaches
               the first top-level terminator/body. Otherwise an ordinary
               function definition followed by `@decorator` would be consumed
               as one decorator declaration. */
            return 0;
        }
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
        {
            char *decl_return = token_range_text(p, decl_start, at - 1);
            str_put(&rt, decl_return);
            free(decl_return);
        }
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
        node_set_value_range(p, n, a, body_close);
        ir_add_child(p->ir, parent, n);
        if (template_i != SIZE_MAX) parse_custom_decorator_template_decl(p, n, template_i, template_close);
        if (params_open != SIZE_MAX && params_close > params_open + 1) parse_parameters(p, n, params_open + 1, params_close - 1);
        {
            int64_t target = ir_add_node(p->ir, N_DECORATOR_TARGET, target_open - 1, target_close, n);
            node_set_name(p->ir, target, "func");
            node_set_value_range(p, target, target_open - 1, target_close);
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
                node_set_value_range(p, n, i, e);
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
                    node_set_qualified_scope(p, n, pt(p, name_i));
                    scope_push(p, pt(p, name_i));
                }
                node_set_value_range(p, n, i, close <= b ? close : b);
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
            node_set_value_range(p, n, i, e);
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "concept")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_CONCEPT, i, e, parent);
            if (i + 1 <= e && is_identifier(p, i + 1)) node_set_name(p->ir, n, pt(p, i + 1));
            node_set_value_range(p, n, i, e);
            ir_add_child(p->ir, parent, n);
            parser_apply_pending(p, n);
            i = e + 1;
            continue;
        }

        if (str_eq(pt(p, i), "friend")) {
            size_t e = find_stmt_end(p, i, b + 1);
            int64_t n = ir_add_node(p->ir, N_FRIEND, i, e, parent);
            node_set_value_range(p, n, i, e);
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
                        node_set_value_range(p, n, i, bc <= e ? bc : e);
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
                    node_set_value_range(p, a, i, e);
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

static int find_custom_decorator_use(const IR *ir, const IRNode *use, int64_t *out_id) {
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


#ifdef CXXE_LSP_BUILD

/* ========================================================================== */
/* CXXE Language Server                                                        */
/* ========================================================================== */

#define CXXELS_VERSION "0.1"
#define LS_MAX_DIAGS 64

#ifndef _WIN32
#include <sys/stat.h>
#endif

typedef struct {
    char *message;
    int severity;
    size_t tok;
} LSDiagnostic;

typedef struct {
    LSDiagnostic v[LS_MAX_DIAGS];
    size_t n;
} LSDiagnostics;

typedef struct {
    char *uri;
    char *path;
    char *text;
    uint64_t version;
    IR ir;
    int parsed;
    LSDiagnostics diagnostics;
} LSDocument;

typedef struct {
    LSDocument *docs;
    size_t n;
    size_t cap;
    char *workspace_root_uri;
    char *workspace_root_path;
    int shutdown;
} LSServer;

static char *ls_strndup(const char *s, size_t n) {
    char *r = (char *)xmalloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static void ls_diag_clear(LSDiagnostics *d) {
    size_t i;
    for (i = 0; i < d->n; ++i) free(d->v[i].message);
    d->n = 0;
}

static void ls_diag_add(LSDiagnostics *d, const char *message, int severity, size_t tok) {
    if (d->n >= LS_MAX_DIAGS) return;
    d->v[d->n].message = xstrdup0(message ? message : "CXXE syntax error");
    d->v[d->n].severity = severity;
    d->v[d->n].tok = tok;
    d->n++;
}

static void ls_doc_free(LSDocument *d) {
    if (!d) return;
    free(d->uri);
    free(d->path);
    free(d->text);
    if (d->parsed) ir_free(&d->ir);
    else memset(&d->ir, 0, sizeof(d->ir));
    ls_diag_clear(&d->diagnostics);
    memset(d, 0, sizeof(*d));
}

static void ls_server_free(LSServer *s) {
    size_t i;
    for (i = 0; i < s->n; ++i) ls_doc_free(&s->docs[i]);
    free(s->docs);
    free(s->workspace_root_uri);
    free(s->workspace_root_path);
    memset(s, 0, sizeof(*s));
}

static LSDocument *ls_doc_find(LSServer *s, const char *uri) {
    size_t i;
    for (i = 0; i < s->n; ++i)
        if (strcmp(s->docs[i].uri, uri) == 0) return &s->docs[i];
    return NULL;
}

static LSDocument *ls_doc_get(LSServer *s, const char *uri) {
    LSDocument *d = ls_doc_find(s, uri);
    if (d) return d;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->docs = (LSDocument *)xrealloc(s->docs, s->cap * sizeof(*s->docs));
    }
    d = &s->docs[s->n++];
    memset(d, 0, sizeof(*d));
    d->uri = xstrdup0(uri);
    ir_init(&d->ir);
    return d;
}

static void ls_doc_remove(LSServer *s, const char *uri) {
    size_t i;
    for (i = 0; i < s->n; ++i) {
        if (strcmp(s->docs[i].uri, uri) == 0) {
            ls_doc_free(&s->docs[i]);
            if (i + 1 < s->n) memmove(&s->docs[i], &s->docs[i + 1], (s->n - i - 1) * sizeof(*s->docs));
            s->n--;
            return;
        }
    }
}

static int ls_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *ls_uri_to_path(const char *uri) {
    const char *p = uri;
    Str s;
    str_init(&s);
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        if (strncmp(p, "localhost/", 10) == 0) p += 9;
#ifdef _WIN32
        if (*p == '/') p++;
#endif
    }
    while (*p) {
        if (p[0] == '%' && p[1] && p[2]) {
            int a = ls_hex((unsigned char)p[1]), b = ls_hex((unsigned char)p[2]);
            if (a >= 0 && b >= 0) {
                str_ch(&s, (char)((a << 4) | b));
                p += 3;
                continue;
            }
        }
        str_ch(&s, *p++);
    }
#ifdef _WIN32
    /* file:///C:/... is the canonical Windows URI form. */
    if (s.len >= 3 && s.data[0] == '/' && isalpha((unsigned char)s.data[1]) && s.data[2] == ':')
        memmove(s.data, s.data + 1, s.len--);
#endif
    return str_take(&s);
}

static char *ls_path_dirname(const char *path) {
    size_t n = strlen(path), i;
    if (!n) return xstrdup0(".");
    for (i = n; i > 0; --i) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') {
            if (i == 1) return ls_strndup(path, 1);
            return ls_strndup(path, i - 1);
        }
    }
    return xstrdup0(".");
}

static void ls_set_workspace(LSServer *s, const char *uri) {
    free(s->workspace_root_uri);
    free(s->workspace_root_path);
    s->workspace_root_uri = xstrdup0(uri ? uri : "");
    s->workspace_root_path = uri ? ls_uri_to_path(uri) : NULL;
}

/* ------------------------------ tiny JSON reader ------------------------- */

static const char *ls_json_skip_ws(const char *p, const char *end) {
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

static const char *ls_json_skip_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return p;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += p + 1 < end ? 2 : 1;
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return end;
}

static const char *ls_json_skip_value(const char *p, const char *end) {
    p = ls_json_skip_ws(p, end);
    if (p >= end) return end;
    if (*p == '"') return ls_json_skip_string(p, end);
    if (*p == '{') {
        int d = 1;
        p++;
        while (p < end && d) {
            if (*p == '"') { p = ls_json_skip_string(p, end); continue; }
            if (*p == '{') d++;
            else if (*p == '}') d--;
            p++;
        }
        return p;
    }
    if (*p == '[') {
        int d = 1;
        p++;
        while (p < end && d) {
            if (*p == '"') { p = ls_json_skip_string(p, end); continue; }
            if (*p == '[') d++;
            else if (*p == ']') d--;
            p++;
        }
        return p;
    }
    while (p < end && !strchr(",]} \t\r\n", *p)) p++;
    return p;
}

static int ls_json_get_value(const char *obj, const char *end, const char *wanted,
                             const char **vstart, const char **vend) {
    const char *p = ls_json_skip_ws(obj, end);
    if (p >= end || *p != '{') return 0;
    p++;
    while (p < end) {
        const char *ks, *ke, *vs, *ve;
        size_t klen;
        p = ls_json_skip_ws(p, end);
        if (p >= end || *p == '}') return 0;
        if (*p != '"') return 0;
        ks = p + 1;
        ke = ls_json_skip_string(p, end);
        if (ke <= ks || ke > end || ke[-1] != '"') return 0;
        klen = (size_t)((ke - 1) - ks);
        p = ls_json_skip_ws(ke, end);
        if (p >= end || *p != ':') return 0;
        p = ls_json_skip_ws(p + 1, end);
        vs = p;
        ve = ls_json_skip_value(p, end);
        if (klen == strlen(wanted) && memcmp(ks, wanted, klen) == 0) {
            if (vstart) *vstart = vs;
            if (vend) *vend = ve;
            return 1;
        }
        p = ls_json_skip_ws(ve, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') return 0;
        return 0;
    }
    return 0;
}

static char *ls_json_unquote(const char *p, const char *end) {
    Str s;
    str_init(&s);
    if (p >= end || *p != '"') return NULL;
    p++;
    while (p < end && *p != '"') {
        if (*p != '\\') { str_ch(&s, *p++); continue; }
        p++;
        if (p >= end) break;
        switch (*p) {
            case '"': str_ch(&s, '"'); p++; break;
            case '\\': str_ch(&s, '\\'); p++; break;
            case '/': str_ch(&s, '/'); p++; break;
            case 'b': str_ch(&s, '\b'); p++; break;
            case 'f': str_ch(&s, '\f'); p++; break;
            case 'n': str_ch(&s, '\n'); p++; break;
            case 'r': str_ch(&s, '\r'); p++; break;
            case 't': str_ch(&s, '\t'); p++; break;
            case 'u':
                /* LSP method/URI/text fields are normally UTF-8 JSON. Keep
                   non-BMP escapes losslessly enough for identifiers by
                   representing the code point in UTF-8. */
                if (p + 4 < end) {
                    unsigned v = 0; int i;
                    for (i = 1; i <= 4; ++i) {
                        int h = ls_hex((unsigned char)p[i]);
                        if (h < 0) { v = 0; break; }
                        v = (v << 4) | (unsigned)h;
                    }
                    if (v <= 0x7F) str_ch(&s, (char)v);
                    else if (v <= 0x7FF) { str_ch(&s, (char)(0xC0 | (v >> 6))); str_ch(&s, (char)(0x80 | (v & 63))); }
                    else { str_ch(&s, (char)(0xE0 | (v >> 12))); str_ch(&s, (char)(0x80 | ((v >> 6) & 63))); str_ch(&s, (char)(0x80 | (v & 63))); }
                    p += 5;
                } else p = end;
                break;
            default: str_ch(&s, *p++); break;
        }
    }
    return str_take(&s);
}

static int ls_json_get_string(const char *obj, const char *end, const char *key, char **out) {
    const char *vs, *ve;
    if (!ls_json_get_value(obj, end, key, &vs, &ve)) return 0;
    if (vs >= ve || *vs != '"') return 0;
    *out = ls_json_unquote(vs, ve);
    return *out != NULL;
}

static int ls_json_get_number(const char *obj, const char *end, const char *key, long long *out) {
    const char *vs, *ve;
    char *tmp;
    char *ep;
    if (!ls_json_get_value(obj, end, key, &vs, &ve)) return 0;
    tmp = ls_strndup(vs, (size_t)(ve - vs));
    *out = strtoll(tmp, &ep, 10);
    { int ok = ep && *ep == 0; free(tmp); return ok; }
}

/* ------------------------------ JSON writer -------------------------------- */

static void ls_json_escape(Str *s, const char *p) {
    const unsigned char *u = (const unsigned char *)(p ? p : "");
    while (*u) {
        switch (*u) {
            case '"': str_put(s, "\\\""); break;
            case '\\': str_put(s, "\\\\"); break;
            case '\n': str_put(s, "\\n"); break;
            case '\r': str_put(s, "\\r"); break;
            case '\t': str_put(s, "\\t"); break;
            case '\b': str_put(s, "\\b"); break;
            case '\f': str_put(s, "\\f"); break;
            default:
                if (*u < 0x20) {
                    char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned)*u); str_put(s, b);
                } else str_ch(s, (char)*u);
                break;
        }
        u++;
    }
}

static void ls_send_json(const char *json) {
    size_t n = strlen(json);
    printf("Content-Length: %zu\r\n\r\n", n);
    fwrite(json, 1, n, stdout);
    fflush(stdout);
}

static void ls_send_response(const char *idraw, const char *result) {
    Str s;
    str_init(&s);
    str_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    str_put(&s, idraw ? idraw : "null");
    str_put(&s, ",\"result\":");
    str_put(&s, result ? result : "null");
    str_put(&s, "}");
    ls_send_json(s.data ? s.data : "{}");
    str_free(&s);
}

static void ls_send_error(const char *idraw, int code, const char *message) {
    Str s;
    str_init(&s);
    str_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    str_put(&s, idraw ? idraw : "null");
    str_put(&s, ",\"error\":{\"code\":");
    { char b[64]; snprintf(b, sizeof(b), "%d", code); str_put(&s, b); }
    str_put(&s, ",\"message\":\"");
    ls_json_escape(&s, message ? message : "error");
    str_put(&s, "\"}}");
    ls_send_json(s.data ? s.data : "{}");
    str_free(&s);
}

static void ls_send_notification(const char *method, const char *params_json) {
    Str s;
    str_init(&s);
    str_put(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"");
    ls_json_escape(&s, method);
    str_put(&s, "\",\"params\":");
    str_put(&s, params_json ? params_json : "{}");
    str_put(&s, "}");
    ls_send_json(s.data ? s.data : "{}");
    str_free(&s);
}

/* ------------------------------ LSP positions ------------------------------ */

static int ls_utf8_width(const unsigned char *p, size_t n, size_t *used, int *valid) {
    unsigned c;
    *used = 1; *valid = 1;
    if (!n) return 0;
    c = p[0];
    if (c < 0x80) return (int)c;
    if ((c & 0xE0) == 0xC0 && n >= 2 && (p[1] & 0xC0) == 0x80) {
        *used = 2; return ((int)(c & 31) << 6) | (p[1] & 63);
    }
    if ((c & 0xF0) == 0xE0 && n >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *used = 3; return ((int)(c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
    }
    if ((c & 0xF8) == 0xF0 && n >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *used = 4; return ((int)(c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
    }
    *valid = 0;
    return 0xFFFD;
}

static size_t ls_offset_from_position(const char *text, size_t len, int line, int character) {
    size_t i = 0, current_line = 0;
    if (line < 0) line = 0;
    if (character < 0) character = 0;
    while (i < len && (int)current_line < line) {
        if (text[i++] == '\n') current_line++;
    }
    {
        size_t start = i, units = 0;
        while (i < len && text[i] != '\n') {
            size_t used; int valid, cp = ls_utf8_width((const unsigned char *)text + i, len - i, &used, &valid);
            int w = cp > 0xFFFF ? 2 : 1;
            if (units + (size_t)w > (size_t)character) break;
            units += (size_t)w;
            i += used;
        }
        (void)start;
    }
    return i;
}

static void ls_position_from_offset(const char *text, size_t len, size_t off, int *line, int *character) {
    size_t i = 0;
    int l = 0, c = 0;
    if (off > len) off = len;
    while (i < off) {
        if (text[i] == '\n') { l++; c = 0; i++; continue; }
        {
            size_t used; int valid, cp = ls_utf8_width((const unsigned char *)text + i, off - i, &used, &valid);
            c += cp > 0xFFFF ? 2 : 1;
            i += used;
        }
    }
    *line = l; *character = c;
}

static size_t ls_token_at_offset(const IR *ir, size_t off) {
    size_t lo = 0, hi = ir->tokens.n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        const Token *t = &ir->tokens.v[m];
        if (off < t->byte_start) hi = m;
        else if (off >= t->byte_end && t->kind != TK_EOF) lo = m + 1;
        else return m;
    }
    if (lo < ir->tokens.n) return lo;
    return ir->tokens.n ? ir->tokens.n - 1 : 0;
}

static void ls_range_for_tokens(const IR *ir, const LSDocument *doc, size_t a, size_t b, int *sl, int *sc, int *el, int *ec) {
    size_t as, be;
    if (ir->tokens.n == 0) { *sl = *sc = *el = *ec = 0; return; }
    if (a >= ir->tokens.n) a = ir->tokens.n - 1;
    if (b >= ir->tokens.n) b = ir->tokens.n - 1;
    as = (size_t)ir->tokens.v[a].byte_start;
    be = (size_t)ir->tokens.v[b].byte_end;
    ls_position_from_offset(doc->text, strlen(doc->text), as, sl, sc);
    ls_position_from_offset(doc->text, strlen(doc->text), be, el, ec);
}

static int64_t ls_parent_symbol(const IR *ir, int64_t id) {
    int64_t p = ir->nodes[id].parent;
    while (p >= 0) {
        switch (ir->nodes[p].kind) {
            case N_NAMESPACE: case N_CLASS: case N_STRUCT: case N_UNION: case N_ENUM:
                return p;
            default: p = ir->nodes[p].parent; break;
        }
    }
    return -1;
}

static const char *ls_kind_name(NodeKind k) {
    switch (k) {
        case N_NAMESPACE: return "namespace";
        case N_CLASS: case N_STRUCT: case N_UNION: return "type";
        case N_ENUM: return "enum";
        case N_FUNCTION: case N_METHOD: case N_CONSTRUCTOR: case N_DESTRUCTOR: case N_OPERATOR_FUNCTION: return "function";
        case N_FIELD_DECL: return "field";
        case N_PROPERTY: return "property";
        case N_VAR_DECL: case N_PARAM_DECL: return "variable";
        default: return "symbol";
    }
}

static int ls_is_declaration_kind(NodeKind k) {
    switch (k) {
        case N_NAMESPACE: case N_CLASS: case N_STRUCT: case N_UNION: case N_ENUM:
        case N_FUNCTION: case N_METHOD: case N_CONSTRUCTOR: case N_DESTRUCTOR: case N_OPERATOR_FUNCTION:
        case N_FIELD_DECL: case N_PROPERTY: case N_VAR_DECL: case N_TYPEDEF: case N_ALIAS: case N_CONCEPT:
            return 1;
        default: return 0;
    }
}

static int ls_symbol_kind(NodeKind k) {
    /* LSP SymbolKind values. */
    switch (k) {
        case N_NAMESPACE: return 3;
        case N_CLASS: case N_STRUCT: case N_UNION: return 5;
        case N_ENUM: return 10;
        case N_FUNCTION: case N_METHOD: case N_CONSTRUCTOR: case N_DESTRUCTOR: case N_OPERATOR_FUNCTION: return 12;
        case N_FIELD_DECL: return 8;
        case N_PROPERTY: return 7;
        case N_VAR_DECL: return 13;
        case N_PARAM_DECL: return 26;
        case N_TYPEDEF: case N_ALIAS: return 26;
        case N_CONCEPT: return 5;
        default: return 13;
    }
}

static int ls_find_name_token(const IR *ir, const IRNode *n) {
    size_t i;
    if (!n->name || !*n->name) return (int)n->first_tok;
    for (i = (size_t)n->first_tok; i <= (size_t)n->last_tok && i < ir->tokens.n; ++i)
        if (strcmp(ir->tokens.v[i].text, n->name) == 0) return (int)i;
    return (int)n->first_tok;
}

static const IRNode *ls_find_declaration(const IR *ir, const char *name, int64_t scope_id) {
    size_t i;
    const IRNode *fallback = NULL;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!ls_is_declaration_kind(n->kind) || !n->name || strcmp(n->name, name) != 0) continue;
        if (n->parent == scope_id) return n;
        if (!fallback) fallback = n;
    }
    return fallback;
}

static int64_t ls_scope_at_token(const IR *ir, size_t tok) {
    size_t i;
    int64_t best = -1;
    uint64_t best_span = UINT64_MAX;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!(n->kind == N_NAMESPACE || n->kind == N_CLASS || n->kind == N_STRUCT || n->kind == N_UNION || n->kind == N_ENUM)) continue;
        if (n->first_tok <= tok && tok <= n->last_tok) {
            uint64_t span = n->last_tok - n->first_tok;
            if (span < best_span) { best_span = span; best = (int64_t)i; }
        }
    }
    return best;
}

static int ls_find_node_token(const IR *ir, size_t tok, int64_t *out_id) {
    size_t i;
    int64_t best = -1;
    uint64_t span = UINT64_MAX;
    for (i = 0; i < ir->node_count; ++i) {
        const IRNode *n = &ir->nodes[i];
        if (!n->name || n->first_tok > tok || tok > n->last_tok) continue;
        if (strcmp(n->name, ir->tokens.v[tok].text) != 0) continue;
        if ((uint64_t)(n->last_tok - n->first_tok) < span) { span = n->last_tok - n->first_tok; best = (int64_t)i; }
    }
    if (best >= 0) { if (out_id) *out_id = best; return 1; }
    return 0;
}

/* ------------------------------ parsing / diagnostics --------------------- */

static size_t ls_extract_token_number(const char *message) {
    const char *p = strstr(message ? message : "", "token ");
    if (!p) return SIZE_MAX;
    p += 6;
    return (size_t)strtoull(p, NULL, 10);
}

static void ls_check_balance(LSDocument *doc) {
    typedef struct { const char *t; size_t tok; } Open;
    Open stack[256]; size_t top = 0, i;
    int pp_depth = 0;
    for (i = 0; i < doc->ir.tokens.n; ++i) {
        const Token *t = &doc->ir.tokens.v[i];
        if (t->kind == TK_PP) {
            const char *kw = pp_keyword(t->text);
            if (strcmp(kw, "if") == 0 || strcmp(kw, "ifdef") == 0 || strcmp(kw, "ifndef") == 0) pp_depth++;
            else if (strcmp(kw, "endif") == 0 && pp_depth > 0) pp_depth--;
            else if (strcmp(kw, "endif") == 0) ls_diag_add(&doc->diagnostics, "unmatched #endif", 1, i);
            continue;
        }
        if (t->kind != TK_PUNCT) continue;
        if (!strcmp(t->text, "(") || !strcmp(t->text, "[") || !strcmp(t->text, "{")) {
            if (top < sizeof(stack) / sizeof(stack[0])) { stack[top].t = t->text; stack[top].tok = i; top++; }
        } else if (!strcmp(t->text, ")") || !strcmp(t->text, "]") || !strcmp(t->text, "}")) {
            const char *want = !strcmp(t->text, ")") ? "(" : !strcmp(t->text, "]") ? "[" : "{";
            if (!top || strcmp(stack[top - 1].t, want) != 0) {
                ls_diag_add(&doc->diagnostics, "unmatched closing delimiter", 1, i);
            } else top--;
        }
    }
    while (top) {
        char b[128]; snprintf(b, sizeof(b), "unclosed delimiter '%s'", stack[top - 1].t);
        ls_diag_add(&doc->diagnostics, b, 1, stack[top - 1].tok);
        top--;
    }
    if (pp_depth) ls_diag_add(&doc->diagnostics, "unterminated preprocessor conditional", 1, doc->ir.tokens.n ? doc->ir.tokens.n - 1 : 0);
}

static int ls_parse_core_caught(IR *ir, char *message, size_t message_cap) {
    g_lsp_in_parse = 1;
    if (setjmp(g_lsp_fatal_jmp) == 0) {
        lex_cpp(ir->source, ir->source_size, &ir->tokens);
        parse_source(ir);
        g_lsp_in_parse = 0;
        return 1;
    }
    snprintf(message, message_cap, "%s", g_lsp_fatal_message[0] ? g_lsp_fatal_message : "CXXE parser error");
    g_lsp_in_parse = 0;
    return 0;
}

static int ls_include_caught(IR *ir, const char *path, const char *const *dirs, size_t dir_count, char *message, size_t message_cap) {
    g_lsp_in_parse = 1;
    if (setjmp(g_lsp_fatal_jmp) == 0) {
        discover_included_decorators(ir, path, dirs, dir_count);
        g_lsp_in_parse = 0;
        return 1;
    }
    snprintf(message, message_cap, "%s", g_lsp_fatal_message[0] ? g_lsp_fatal_message : "CXXE include error");
    g_lsp_in_parse = 0;
    return 0;
}

static int ls_semantics_caught(IR *ir, char *message, size_t message_cap) {
    g_lsp_in_parse = 1;
    if (setjmp(g_lsp_fatal_jmp) == 0) {
        resolve_native_features(ir);
        resolve_named_args(ir);
        g_lsp_in_parse = 0;
        return 1;
    }
    snprintf(message, message_cap, "%s", g_lsp_fatal_message[0] ? g_lsp_fatal_message : "CXXE semantic error");
    g_lsp_in_parse = 0;
    return 0;
}

static void ls_parse_document(LSServer *server, LSDocument *doc) {
    char fatal[sizeof(g_lsp_fatal_message)];
    const char *dirs[2];
    size_t dir_count = 0;
    char *doc_dir = NULL;
    size_t text_len = strlen(doc->text ? doc->text : "");
    ls_diag_clear(&doc->diagnostics);
    if (doc->parsed) { ir_free(&doc->ir); doc->parsed = 0; }
    ir_init(&doc->ir);
    doc->ir.source = (uint8_t *)xmalloc(text_len + 1);
    doc->ir.source_size = text_len;
    memcpy(doc->ir.source, doc->text ? doc->text : "", text_len);
    doc->ir.source[text_len] = 0;

    if (!ls_parse_core_caught(&doc->ir, fatal, sizeof(fatal))) {
        ls_diag_add(&doc->diagnostics, fatal, 1, ls_extract_token_number(fatal));
        doc->parsed = 1;
        return;
    }
    doc->parsed = 1;
    ls_check_balance(doc);

    if (doc->path && *doc->path) {
        doc_dir = ls_path_dirname(doc->path);
        dirs[dir_count++] = doc_dir;
        if (server->workspace_root_path && *server->workspace_root_path && dir_count < 2) dirs[dir_count++] = server->workspace_root_path;
        if (!ls_include_caught(&doc->ir, doc->path, dirs, dir_count, fatal, sizeof(fatal)))
            ls_diag_add(&doc->diagnostics, fatal, 1, ls_extract_token_number(fatal));
        free(doc_dir);
    }

    if (!ls_semantics_caught(&doc->ir, fatal, sizeof(fatal)))
        ls_diag_add(&doc->diagnostics, fatal, 1, ls_extract_token_number(fatal));
}

static void ls_publish_diagnostics(const LSDocument *doc) {
    Str p;
    size_t i;
    str_init(&p);
    str_put(&p, "{\"uri\":\""); ls_json_escape(&p, doc->uri); str_put(&p, "\",\"diagnostics\":[");
    for (i = 0; i < doc->diagnostics.n; ++i) {
        const LSDiagnostic *d = &doc->diagnostics.v[i];
        int sl = 0, sc = 0, el = 0, ec = 0;
        size_t tok = d->tok;
        if (tok == SIZE_MAX || !doc->ir.tokens.n) tok = 0;
        if (doc->ir.tokens.n) ls_range_for_tokens(&doc->ir, doc, tok, tok, &sl, &sc, &el, &ec);
        if (i) str_ch(&p, ',');
        str_put(&p, "{\"range\":{\"start\":{");
        { char b[128]; snprintf(b, sizeof(b), "\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}", sl, sc, el, ec); str_put(&p, b); }
        str_put(&p, "},\"severity\":");
        { char b[32]; snprintf(b, sizeof(b), "%d", d->severity); str_put(&p, b); }
        str_put(&p, ",\"source\":\"cxxe\",\"message\":\"");
        ls_json_escape(&p, d->message); str_put(&p, "\"}");
    }
    str_put(&p, "]}");
    ls_send_notification("textDocument/publishDiagnostics", p.data ? p.data : "{}");
    str_free(&p);
}

/* ------------------------------ protocol payloads ------------------------- */

static void ls_write_range(Str *s, int sl, int sc, int el, int ec) {
    char b[256];
    snprintf(b, sizeof(b), "{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}", sl, sc, el, ec);
    str_put(s, b);
}

static void ls_write_location(Str *s, const LSDocument *doc, const IRNode *n) {
    int sl, sc, el, ec;
    ls_range_for_tokens(&doc->ir, doc, (size_t)n->first_tok, (size_t)n->last_tok, &sl, &sc, &el, &ec);
    str_put(s, "{\"uri\":\""); ls_json_escape(s, doc->uri); str_put(s, "\",\"range\":");
    ls_write_range(s, sl, sc, el, ec); str_put(s, "}");
}

static void ls_document_symbols(const LSDocument *doc, Str *out) {
    size_t i, count = 0;
    str_put(out, "[");
    for (i = 0; i < doc->ir.node_count; ++i) {
        const IRNode *n = &doc->ir.nodes[i];
        int ntok;
        int sl, sc, el, ec, nsl, nsc, nel, nec;
        int64_t parent;
        if (!ls_is_declaration_kind(n->kind) || !n->name || !*n->name) continue;
        if (n->kind == N_VAR_DECL && n->parent < 0) continue;
        ntok = ls_find_name_token(&doc->ir, n);
        ls_range_for_tokens(&doc->ir, doc, (size_t)n->first_tok, (size_t)n->last_tok, &sl, &sc, &el, &ec);
        ls_range_for_tokens(&doc->ir, doc, (size_t)ntok, (size_t)ntok, &nsl, &nsc, &nel, &nec);
        if (count++) str_ch(out, ',');
        str_put(out, "{\"name\":\""); ls_json_escape(out, n->name); str_put(out, "\",\"kind\":");
        { char b[32]; snprintf(b, sizeof(b), "%d", ls_symbol_kind(n->kind)); str_put(out, b); }
        str_put(out, ",\"range\":"); ls_write_range(out, sl, sc, el, ec);
        str_put(out, ",\"selectionRange\":"); ls_write_range(out, nsl, nsc, nel, nec);
        parent = ls_parent_symbol(&doc->ir, (int64_t)i);
        if (parent >= 0 && doc->ir.nodes[parent].name) {
            str_put(out, ",\"containerName\":\""); ls_json_escape(out, doc->ir.nodes[parent].name); str_put(out, "\"");
        }
        str_put(out, "}");
    }
    str_put(out, "]");
}

static const IRNode *ls_symbol_at_position(const LSDocument *doc, int line, int character, size_t *tok_out) {
    size_t off = ls_offset_from_position(doc->text, strlen(doc->text), line, character);
    size_t tok = ls_token_at_offset(&doc->ir, off);
    if (tok_out) *tok_out = tok;
    if (tok >= doc->ir.tokens.n) return NULL;
    {
        int64_t id;
        if (ls_find_node_token(&doc->ir, tok, &id)) return &doc->ir.nodes[id];
        if (tok > 0 && strcmp(doc->ir.tokens.v[tok - 1].text, "@") == 0) {
            size_t i;
            for (i = 0; i < doc->ir.node_count; ++i) {
                const IRNode *n = &doc->ir.nodes[i];
                if (n->kind == N_DECORATOR && n->first_tok <= tok && tok <= n->last_tok) return n;
            }
        }
        if (is_identifier((Parser *)&(Parser){ .ir = (IR *)&doc->ir }, tok)) {
            int64_t scope = ls_scope_at_token(&doc->ir, tok);
            return ls_find_declaration(&doc->ir, doc->ir.tokens.v[tok].text, scope);
        }
    }
    return NULL;
}

static void ls_hover(const LSDocument *doc, int line, int character, Str *out) {
    size_t tok;
    const IRNode *n = ls_symbol_at_position(doc, line, character, &tok);
    if (!n) { str_put(out, "null"); return; }
    str_put(out, "{\"contents\":{\"kind\":\"markdown\",\"value\":\"");
    if (n->kind == N_DECORATOR) {
        str_put(out, "**C++E decorator** `@"); ls_json_escape(out, n->name ? n->name : ""); str_put(out, "`\\n\\n");
        if (n->qualified && strcmp(n->qualified, n->name) != 0) { str_put(out, "Qualified name: `"); ls_json_escape(out, n->qualified); str_put(out, "`"); }
    } else {
        str_put(out, "```cpp\\n");
        if (n->return_type && *n->return_type) { ls_json_escape(out, n->return_type); str_put(out, " "); }
        else if (n->type && *n->type) { ls_json_escape(out, n->type); str_put(out, " "); }
        ls_json_escape(out, n->name ? n->name : doc->ir.tokens.v[tok].text);
        if (n->kind == N_FUNCTION || n->kind == N_METHOD || n->kind == N_CONSTRUCTOR || n->kind == N_DESTRUCTOR || n->kind == N_OPERATOR_FUNCTION) str_put(out, "(...)");
        str_put(out, "\\n```");
        if (n->kind == N_PROPERTY) {
            str_put(out, "\\n\\nC++E property");
            if (n->value) { str_put(out, ".\\n\\n```cpp\\n"); ls_json_escape(out, n->value); str_put(out, "\\n```"); }
        }
        if (n->flags & NF_REGISTERED) str_put(out, "\\n\\n`@register` runtime type");
        if (n->flags & NF_EXPOSED) str_put(out, "\\n\\n`@exposed` reflection member");
    }
    str_put(out, "\"},\"range\":");
    { int sl,sc,el,ec; ls_range_for_tokens(&doc->ir, doc, (size_t)n->first_tok, (size_t)n->last_tok, &sl,&sc,&el,&ec); ls_write_range(out, sl,sc,el,ec); }
    str_put(out, "}");
}

static void ls_completion_item(Str *s, const char *label, const char *detail, int kind, const char *insert) {
    str_put(s, "{\"label\":\""); ls_json_escape(s, label); str_put(s, "\",\"kind\":");
    { char b[32]; snprintf(b, sizeof(b), "%d", kind); str_put(s, b); }
    if (detail) { str_put(s, ",\"detail\":\""); ls_json_escape(s, detail); str_put(s, "\""); }
    if (insert) { str_put(s, ",\"insertText\":\""); ls_json_escape(s, insert); str_put(s, "\""); }
    str_put(s, "}");
}

static void ls_completion(const LSDocument *doc, int line, int character, Str *out) {
    size_t off = ls_offset_from_position(doc->text, strlen(doc->text), line, character);
    const char *text = doc->text;
    const char *p = off ? text + off - 1 : text;
    int member_context = 0, named_arg_context = 0, decorator_context = 0;
    char receiver[256] = {0};
    char function_name[256] = {0};
    const char *line_start = text + off;
    while (line_start > text && line_start[-1] != '\n') line_start--;

    /* Work from the complete text immediately before the cursor. This handles
       both `object.` and `object->` without depending on tokenization of an
       incomplete statement. */
    {
        const char *q = p;
        while (q >= line_start && isspace((unsigned char)*q)) {
            if (q == line_start) { q = line_start - 1; break; }
            q--;
        }
        if (q >= line_start && *q == '@') decorator_context = 1;
        if (q >= line_start && *q == '.') {
            member_context = 1;
            q--;
            while (q >= line_start && isspace((unsigned char)*q)) q--;
            {
                const char *end_word = q + 1;
                while (q >= line_start && (isalnum((unsigned char)*q) || *q == '_')) q--;
                q++;
                if (q < end_word) {
                    size_t n = (size_t)(end_word - q);
                    if (n >= sizeof(receiver)) n = sizeof(receiver) - 1;
                    memcpy(receiver, q, n); receiver[n] = 0;
                }
            }
        } else if (q >= line_start && *q == '>' && q > line_start && q[-1] == '-') {
            member_context = 1;
            q -= 2;
            while (q >= line_start && isspace((unsigned char)*q)) q--;
            {
                const char *end_word = q + 1;
                while (q >= line_start && (isalnum((unsigned char)*q) || *q == '_')) q--;
                q++;
                if (q < end_word) {
                    size_t n = (size_t)(end_word - q);
                    if (n >= sizeof(receiver)) n = sizeof(receiver) - 1;
                    memcpy(receiver, q, n); receiver[n] = 0;
                }
            }
        }
    }

    if (!member_context && !decorator_context) {
        const char *q = p;
        int depth = 0;
        while (q >= text) {
            if (*q == ')') depth++;
            else if (*q == '(') {
                if (depth == 0) {
                    const char *r = q - 1;
                    while (r >= text && isspace((unsigned char)*r)) r--;
                    while (r >= text && (isalnum((unsigned char)*r) || *r == '_')) r--;
                    r++;
                    if (r < q) {
                        size_t n = (size_t)(q - r);
                        if (n >= sizeof(function_name)) n = sizeof(function_name) - 1;
                        memcpy(function_name, r, n); function_name[n] = 0;
                        named_arg_context = 1;
                    }
                    break;
                }
                depth--;
            }
            if (*q == ';' || *q == '{' || *q == '}') break;
            if (q == text) break;
            q--;
        }
    }

    str_put(out, "{\"isIncomplete\":false,\"items\":[");
    size_t count = 0, i;
#define LS_ADD(label, detail, kind, insert) do { if (count++) str_ch(out, ','); ls_completion_item(out, (label), (detail), (kind), (insert)); } while (0)
    if (decorator_context) {
        static const char *native[] = { "register", "exposed", "deprecated", "since", "experimental", NULL };
        for (i = 0; native[i]; ++i) LS_ADD(native[i], "C++E decorator", 25, native[i]);
        for (i = 0; i < doc->ir.node_count; ++i) {
            const IRNode *n = &doc->ir.nodes[i];
            if (n->kind == N_CUSTOM_DECORATOR && n->name) LS_ADD(n->name, "C++E custom decorator", 25, n->name);
        }
    } else if (member_context) {
        const IRNode *owner = NULL;
        size_t best_depth = SIZE_MAX;
        /* Local variables are not represented by a declaration node in every
           parser path. Infer the receiver type directly from a declaration
           shaped like `Type receiver`, `Type* receiver`, or `A::Type receiver`. */
        {
            size_t limit = ls_token_at_offset(&doc->ir, off);
            for (i = 0; i < limit && i < doc->ir.tokens.n; ++i) {
                if (strcmp(doc->ir.tokens.v[i].text, receiver) != 0) continue;
                if (i > 0 && (strcmp(doc->ir.tokens.v[i - 1].text, ".") == 0 || strcmp(doc->ir.tokens.v[i - 1].text, "->") == 0 || strcmp(doc->ir.tokens.v[i - 1].text, "::") == 0)) continue;
                if (i + 1 >= doc->ir.tokens.n) continue;
                if (strcmp(doc->ir.tokens.v[i + 1].text, ";") != 0 && strcmp(doc->ir.tokens.v[i + 1].text, "=") != 0 && strcmp(doc->ir.tokens.v[i + 1].text, "{") != 0 && strcmp(doc->ir.tokens.v[i + 1].text, ",") != 0) continue;
                {
                    size_t k = i - 1;
                    while (k > 0 && (strcmp(doc->ir.tokens.v[k].text, "*") == 0 || strcmp(doc->ir.tokens.v[k].text, "&") == 0 || strcmp(doc->ir.tokens.v[k].text, "const") == 0)) k--;
                    if (doc->ir.tokens.v[k].kind == TK_IDENTIFIER || doc->ir.tokens.v[k].kind == TK_NUMBER) {
                        const char *type_name = doc->ir.tokens.v[k].text;
                        size_t j;
                        for (j = 0; j < doc->ir.node_count; ++j) {
                            const IRNode *t = &doc->ir.nodes[j];
                            if ((t->kind == N_CLASS || t->kind == N_STRUCT || t->kind == N_UNION) && t->name && strcmp(t->name, type_name) == 0) {
                                size_t depth = 0; int64_t qid = t->parent;
                                while (qid >= 0) { depth++; qid = doc->ir.nodes[qid].parent; }
                                if (depth < best_depth) { best_depth = depth; owner = t; }
                            }
                        }
                    }
                }
            }
        }
        if (owner) {
            int64_t owner_id = (int64_t)(owner - doc->ir.nodes);
            for (i = 0; i < doc->ir.node_count; ++i) {
                const IRNode *n = &doc->ir.nodes[i];
                if (n->parent == owner_id &&
                    (n->kind == N_FIELD_DECL || n->kind == N_PROPERTY || n->kind == N_METHOD || n->kind == N_FUNCTION) && n->name) {
                    int kind = n->kind == N_METHOD || n->kind == N_FUNCTION ? 2 : 10;
                    LS_ADD(n->name, ls_kind_name(n->kind), kind, n->name);
                }
            }
        }
    } else if (named_arg_context && function_name[0]) {
        for (i = 0; i < doc->ir.node_count; ++i) {
            const IRNode *n = &doc->ir.nodes[i];
            if (!n->name || strcmp(n->name, function_name) != 0) continue;
            if (!(n->kind == N_FUNCTION || n->kind == N_METHOD || n->kind == N_CONSTRUCTOR || n->kind == N_OPERATOR_FUNCTION)) continue;
            {
                int64_t c;
                for (c = n->first_child; c >= 0; c = doc->ir.nodes[c].next_sibling) {
                    const IRNode *pn = &doc->ir.nodes[c];
                    if (pn->kind == N_PARAM_DECL && pn->name) {
                        Str ins; str_init(&ins);
                        str_put(&ins, pn->name); str_put(&ins, "=");
                        LS_ADD(pn->name, pn->type ? pn->type : "parameter", 6, ins.data);
                        str_free(&ins);
                    }
                }
            }
        }
    } else {
        static const char *kw[] = {
            "class","struct","union","enum","namespace","using","template","concept","requires",
            "if","else","for","while","switch","case","default","return","break","continue","try","catch","throw",
            "match","property","signal","defer","new","delete","this","nullptr","const","constexpr","static","virtual","override","final", NULL
        };
        for (i = 0; kw[i]; ++i) LS_ADD(kw[i], "C++E/C++ keyword", 14, kw[i]);
        for (i = 0; i < doc->ir.node_count; ++i) {
            const IRNode *n = &doc->ir.nodes[i];
            if (ls_is_declaration_kind(n->kind) && n->name) LS_ADD(n->name, ls_kind_name(n->kind), ls_symbol_kind(n->kind), n->name);
        }
    }
#undef LS_ADD
    str_put(out, "]}");
}

static void ls_definition(const LSDocument *doc, int line, int character, Str *out) {
    size_t tok, off = ls_offset_from_position(doc->text, strlen(doc->text), line, character);
    const char *name;
    const IRNode *decl;
    int64_t scope;
    tok = ls_token_at_offset(&doc->ir, off);
    if (tok >= doc->ir.tokens.n || !is_identifier((Parser *)&(Parser){ .ir = (IR *)&doc->ir }, tok)) { str_put(out, "[]"); return; }
    name = doc->ir.tokens.v[tok].text;
    scope = ls_scope_at_token(&doc->ir, tok);
    decl = ls_find_declaration(&doc->ir, name, scope);
    if (!decl) { str_put(out, "[]"); return; }
    str_put(out, "["); ls_write_location(out, doc, decl); str_put(out, "]");
}

static void ls_references(const LSDocument *doc, int line, int character, Str *out) {
    size_t off = ls_offset_from_position(doc->text, strlen(doc->text), line, character);
    size_t tok = ls_token_at_offset(&doc->ir, off);
    const char *name;
    size_t i, count = 0;
    if (tok >= doc->ir.tokens.n || !is_identifier((Parser *)&(Parser){ .ir = (IR *)&doc->ir }, tok)) { str_put(out, "[]"); return; }
    name = doc->ir.tokens.v[tok].text;
    str_put(out, "[");
    for (i = 0; i < doc->ir.tokens.n; ++i) {
        const Token *t = &doc->ir.tokens.v[i];
        if (t->kind != TK_IDENTIFIER || strcmp(t->text, name) != 0) continue;
        int sl,sc,el,ec;
        ls_range_for_tokens(&doc->ir, doc, i, i, &sl,&sc,&el,&ec);
        if (count++) str_ch(out, ',');
        str_put(out, "{\"uri\":\""); ls_json_escape(out, doc->uri); str_put(out, "\",\"range\":"); ls_write_range(out, sl,sc,el,ec); str_put(out, "}");
    }
    str_put(out, "]");
}

static void ls_workspace_symbols(const LSServer *s, const char *query, Str *out) {
    size_t d, i, count = 0;
    str_put(out, "[");
    for (d = 0; d < s->n; ++d) {
        const LSDocument *doc = &s->docs[d];
        for (i = 0; i < doc->ir.node_count; ++i) {
            const IRNode *n = &doc->ir.nodes[i];
            if (!ls_is_declaration_kind(n->kind) || !n->name) continue;
            if (query && *query && !strstr(n->name, query)) continue;
            {
                int sl,sc,el,ec;
                ls_range_for_tokens(&doc->ir, doc, (size_t)n->first_tok, (size_t)n->last_tok, &sl,&sc,&el,&ec);
                if (count++) str_ch(out, ',');
                str_put(out, "{\"name\":\""); ls_json_escape(out, n->name); str_put(out, "\",\"kind\":");
                { char b[32]; snprintf(b, sizeof(b), "%d", ls_symbol_kind(n->kind)); str_put(out, b); }
                str_put(out, ",\"location\":"); ls_write_location(out, doc, n); str_put(out, "}");
            }
        }
    }
    str_put(out, "]");
}

static void ls_semantic_tokens(const LSDocument *doc, Str *out) {
    size_t i;
    int prev_line = 0, prev_char = 0;
    str_put(out, "{\"data\":[");
    int first = 1;
    for (i = 0; i < doc->ir.tokens.n; ++i) {
        const Token *t = &doc->ir.tokens.v[i];
        int type = -1, line, ch;
        size_t used, cpbytes;
        if (t->kind == TK_EOF || t->kind == TK_PP) continue;
        ls_position_from_offset(doc->text, strlen(doc->text), (size_t)t->byte_start, &line, &ch);
        if (t->kind == TK_NUMBER) type = 7;
        else if (t->kind == TK_STRING || t->kind == TK_CHAR) type = 8;
        else if (t->kind == TK_IDENTIFIER) {
            if (is_keyword(t->text)) type = 12;
            else if (i > 0 && strcmp(doc->ir.tokens.v[i - 1].text, "@") == 0) type = 13;
            else {
                size_t j;
                type = 5; /* variable */
                for (j = 0; j < doc->ir.node_count; ++j) {
                    const IRNode *n = &doc->ir.nodes[j];
                    if ((size_t)n->first_tok > i || i > (size_t)n->last_tok || !n->name || strcmp(n->name, t->text) != 0) continue;
                    if (n->kind == N_CLASS || n->kind == N_STRUCT || n->kind == N_UNION) type = 0;
                    else if (n->kind == N_ENUM) type = 1;
                    else if (n->kind == N_NAMESPACE) type = 2;
                    else if (n->kind == N_FUNCTION || n->kind == N_OPERATOR_FUNCTION) type = 10;
                    else if (n->kind == N_METHOD) type = 11;
                    else if (n->kind == N_PROPERTY) type = 4;
                    else if (n->kind == N_PARAM_DECL) type = 6;
                    else if (n->kind == N_FIELD_DECL) type = 4;
                }
            }
        } else continue;
        cpbytes = 0;
        if (t->byte_end > t->byte_start) {
            size_t off = (size_t)t->byte_start;
            size_t n = (size_t)(t->byte_end - t->byte_start);
            while (n) {
                int valid;
                int code = ls_utf8_width((const unsigned char *)doc->text + off, n, &used, &valid);
                cpbytes += code > 0xFFFF ? 2 : 1;
                off += used; n -= used;
            }
        }
        if (cpbytes == 0) continue;
        if (!first) str_ch(out, ',');
        {
            int dl = line - prev_line;
            int dc = dl == 0 ? ch - prev_char : ch;
            char b[128]; snprintf(b, sizeof(b), "%d,%d,%zu,%d,0", dl, dc, cpbytes, type); str_put(out, b);
        }
        prev_line = line; prev_char = ch; first = 0;
    }
    str_put(out, "]}");
}

static void ls_put_trimmed(Str *s, const char *text) {
    const char *a, *b;
    if (!text) return;
    a = text;
    while (*a && isspace((unsigned char)*a)) a++;
    b = a + strlen(a);
    while (b > a && isspace((unsigned char)b[-1])) b--;
    if (b > a) str_putn(s, a, (size_t)(b - a));
}

static void ls_signature_help(const LSDocument *doc, int line, int character, Str *out) {
    size_t off = ls_offset_from_position(doc->text, strlen(doc->text), line, character);
    const char *text = doc->text;
    const char *q = off ? text + off - 1 : text;
    char fn[256] = {0};
    const IRNode *best = NULL;
    while (q >= text) {
        if (*q == '(') {
            const char *r = q - 1;
            while (r >= text && isspace((unsigned char)*r)) r--;
            while (r >= text && (isalnum((unsigned char)*r) || *r == '_')) r--;
            r++;
            if (r < q) { size_t n = (size_t)(q-r); if (n >= sizeof(fn)) n=sizeof(fn)-1; memcpy(fn,r,n); fn[n]=0; }
            break;
        }
        if (*q == ';' || *q == '{' || *q == '}') break;
        if (q == text) break;
        q--;
    }
    if (!fn[0]) { str_put(out, "null"); return; }
    {
        size_t i;
        for (i = 0; i < doc->ir.node_count; ++i) {
            const IRNode *n = &doc->ir.nodes[i];
            if ((n->kind == N_FUNCTION || n->kind == N_METHOD || n->kind == N_CONSTRUCTOR || n->kind == N_OPERATOR_FUNCTION) && n->name && strcmp(n->name, fn) == 0) { best = n; break; }
        }
    }
    if (!best) { str_put(out, "null"); return; }
    {
        Str sig; int64_t c; int first = 1;
        int active_parameter = 0;
        size_t call_start = (size_t)(q - text);
        int nesting = 0;
        size_t j;
        /* Count commas since the innermost opening parenthesis before the cursor. */
        for (j = off; j > 0; --j) {
            if (text[j - 1] == ')') nesting++;
            else if (text[j - 1] == '(') {
                if (nesting == 0) { call_start = j - 1; break; }
                nesting--;
            }
        }
        nesting = 0;
        for (j = call_start + 1; j < off; ++j) {
            if (text[j] == '(' || text[j] == '[' || text[j] == '{') nesting++;
            else if (text[j] == ')' || text[j] == ']' || text[j] == '}') { if (nesting > 0) nesting--; }
            else if (text[j] == ',' && nesting == 0) active_parameter++;
        }
        str_init(&sig);
        if (best->return_type && *best->return_type) { ls_put_trimmed(&sig, best->return_type); str_ch(&sig, ' '); }
        str_put(&sig, best->name); str_ch(&sig, '(');
        for (c = best->first_child; c >= 0; c = doc->ir.nodes[c].next_sibling) {
            const IRNode *pn = &doc->ir.nodes[c];
            if (pn->kind != N_PARAM_DECL) continue;
            if (!first) str_put(&sig, ", ");
            first = 0;
            if (pn->type) ls_put_trimmed(&sig, pn->type);
            if (pn->name) { str_ch(&sig, ' '); str_put(&sig, pn->name); }
        }
        str_ch(&sig, ')');
        str_put(out, "{\"signatures\":[{\"label\":\""); ls_json_escape(out, sig.data ? sig.data : "");
        { char b[128]; snprintf(b, sizeof(b), "\"}],\"activeSignature\":0,\"activeParameter\":%d}", active_parameter); str_put(out, b); }
        str_free(&sig);
    }
}

static void ls_initialize_result(Str *out) {
    str_put(out,
        "{\"serverInfo\":{\"name\":\"cxxe-language-server\",\"version\":\"" CXXELS_VERSION "\"},"
        "\"capabilities\":{"
        "\"textDocumentSync\":1,"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"referencesProvider\":true,"
        "\"documentSymbolProvider\":true,"
        "\"workspaceSymbolProvider\":true,"
        "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\",\">\",\"@\",\"(\",\",\"]},"
        "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
        "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"class\",\"enum\",\"namespace\",\"typeParameter\",\"property\",\"variable\",\"parameter\",\"number\",\"string\",\"type\",\"function\",\"method\",\"keyword\",\"decorator\"],\"tokenModifiers\":[]},\"full\":true}"
        "}}"
    );
}

static int ls_read_message(char **payload_out, size_t *len_out) {
    char line[512];
    size_t content_length = 0;
    int have_length = 0;
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\r' || line[n-1] == '\n')) line[--n] = 0;
        if (n == 0) break;
        if (strncmp(line, "Content-Length:", 15) == 0) {
            content_length = (size_t)strtoull(line + 15, NULL, 10);
            have_length = 1;
        }
    }
    if (!have_length) return 0;
    {
        char *p = (char *)xmalloc(content_length + 1);
        size_t got = fread(p, 1, content_length, stdin);
        if (got != content_length) { free(p); return 0; }
        p[content_length] = 0;
        *payload_out = p; *len_out = content_length;
        return 1;
    }
}

static void ls_handle_message(LSServer *s, const char *json, size_t len, int *running) {
    char *method = NULL;
    char *uri = NULL;
    const char *idstart = NULL, *idend = NULL;
    if (!ls_json_get_string(json, json + len, "method", &method)) { free(method); return; }
    if (ls_json_get_value(json, json + len, "id", &idstart, &idend) == 0) { idstart = NULL; idend = NULL; }

    if (strcmp(method, "initialize") == 0) {
        const char *params_s, *params_e;
        char *root_uri = NULL;
        if (ls_json_get_value(json, json + len, "params", &params_s, &params_e)) {
            if (ls_json_get_string(params_s, params_e, "rootUri", &root_uri) && root_uri) ls_set_workspace(s, root_uri);
            else if (ls_json_get_string(params_s, params_e, "rootPath", &root_uri) && root_uri) {
                free(s->workspace_root_uri); s->workspace_root_uri = xstrdup0("");
                free(s->workspace_root_path); s->workspace_root_path = xstrdup0(root_uri);
            }
        }
        Str r; str_init(&r); ls_initialize_result(&r);
        { char *idcopy = idstart ? ls_strndup(idstart, (size_t)(idend-idstart)) : xstrdup0("null"); ls_send_response(idcopy, r.data); free(idcopy); }
        str_free(&r); free(root_uri); free(method); return;
    }
    if (strcmp(method, "shutdown") == 0) {
        s->shutdown = 1;
        ls_send_response(idstart ? ls_strndup(idstart, (size_t)(idend-idstart)) : "null", "null");
        free(method); return;
    }
    if (strcmp(method, "exit") == 0) { *running = 0; free(method); return; }

    if (strcmp(method, "textDocument/didOpen") == 0) {
        const char *params_s, *params_e, *td_s, *td_e;
        char *text = NULL;
        long long version = 0;
        if (ls_json_get_value(json, json + len, "params", &params_s, &params_e) &&
            ls_json_get_value(params_s, params_e, "textDocument", &td_s, &td_e)) {
            ls_json_get_string(td_s, td_e, "uri", &uri);
            ls_json_get_string(td_s, td_e, "text", &text);
            ls_json_get_number(td_s, td_e, "version", &version);
            if (uri && text) {
                LSDocument *d = ls_doc_get(s, uri);
                free(d->text); d->text = text; text = NULL; d->version = (uint64_t)version;
                free(d->path); d->path = ls_uri_to_path(uri);
                ls_parse_document(s, d); ls_publish_diagnostics(d);
            }
        }
        free(text); free(uri); free(method); return;
    }
    if (strcmp(method, "textDocument/didChange") == 0) {
        const char *params_s, *params_e, *td_s, *td_e, *cc_s, *cc_e;
        char *text = NULL;
        long long version = 0;
        if (ls_json_get_value(json, json + len, "params", &params_s, &params_e) &&
            ls_json_get_value(params_s, params_e, "textDocument", &td_s, &td_e)) {
            ls_json_get_string(td_s, td_e, "uri", &uri);
            ls_json_get_number(td_s, td_e, "version", &version);
            if (ls_json_get_value(params_s, params_e, "contentChanges", &cc_s, &cc_e) && cc_s < cc_e && *cc_s == '[') {
                const char *p = ls_json_skip_ws(cc_s + 1, cc_e);
                if (p < cc_e && *p == '{') {
                    const char *ce = ls_json_skip_value(p, cc_e);
                    ls_json_get_string(p, ce, "text", &text);
                }
            }
            if (uri && text) {
                LSDocument *d = ls_doc_get(s, uri);
                free(d->text); d->text = text; text = NULL; d->version = (uint64_t)version;
                if (!d->path) d->path = ls_uri_to_path(uri);
                ls_parse_document(s, d); ls_publish_diagnostics(d);
            }
        }
        free(text); free(uri); free(method); return;
    }
    if (strcmp(method, "textDocument/didClose") == 0) {
        const char *params_s, *params_e, *td_s, *td_e;
        if (ls_json_get_value(json, json + len, "params", &params_s, &params_e) && ls_json_get_value(params_s, params_e, "textDocument", &td_s, &td_e)) {
            ls_json_get_string(td_s, td_e, "uri", &uri);
            if (uri) {
                LSDocument *d = ls_doc_find(s, uri);
                if (d) {
                    Str p; str_init(&p); str_put(&p, "{\"uri\":\""); ls_json_escape(&p, uri); str_put(&p, "\",\"diagnostics\":[]}"); ls_send_notification("textDocument/publishDiagnostics", p.data); str_free(&p);
                }
                ls_doc_remove(s, uri);
            }
        }
        free(uri); free(method); return;
    }
    if (strcmp(method, "textDocument/hover") == 0 || strcmp(method, "textDocument/definition") == 0 ||
        strcmp(method, "textDocument/references") == 0 || strcmp(method, "textDocument/completion") == 0 ||
        strcmp(method, "textDocument/signatureHelp") == 0 || strcmp(method, "textDocument/semanticTokens/full") == 0) {
        const char *params_s, *params_e, *td_s, *td_e, *pos_s, *pos_e;
        int line = 0, character = 0;
        if (ls_json_get_value(json, json + len, "params", &params_s, &params_e) && ls_json_get_value(params_s, params_e, "textDocument", &td_s, &td_e)) {
            ls_json_get_string(td_s, td_e, "uri", &uri);
            if (ls_json_get_value(params_s, params_e, "position", &pos_s, &pos_e)) {
                long long x; if (ls_json_get_number(pos_s, pos_e, "line", &x)) line = (int)x;
                if (ls_json_get_number(pos_s, pos_e, "character", &x)) character = (int)x;
            }
            {
                LSDocument *d = uri ? ls_doc_find(s, uri) : NULL;
                Str r; str_init(&r);
                if (!d) str_put(&r, "null");
                else if (strcmp(method, "textDocument/hover") == 0) ls_hover(d,line,character,&r);
                else if (strcmp(method, "textDocument/definition") == 0) ls_definition(d,line,character,&r);
                else if (strcmp(method, "textDocument/references") == 0) ls_references(d,line,character,&r);
                else if (strcmp(method, "textDocument/completion") == 0) ls_completion(d,line,character,&r);
                else if (strcmp(method, "textDocument/signatureHelp") == 0) ls_signature_help(d,line,character,&r);
                else ls_semantic_tokens(d,&r);
                { char *idcopy = idstart ? ls_strndup(idstart, (size_t)(idend-idstart)) : xstrdup0("null"); ls_send_response(idcopy, r.data); free(idcopy); }
                str_free(&r);
            }
        } else if (idstart) {
            char *idcopy = ls_strndup(idstart, (size_t)(idend-idstart)); ls_send_response(idcopy, "null"); free(idcopy);
        }
        free(uri); free(method); return;
    }
    if (strcmp(method, "textDocument/documentSymbol") == 0) {
        const char *params_s,*params_e,*td_s,*td_e;
        if (ls_json_get_value(json,json+len,"params",&params_s,&params_e) && ls_json_get_value(params_s,params_e,"textDocument",&td_s,&td_e)) {
            Str r; str_init(&r); ls_json_get_string(td_s,td_e,"uri",&uri); { LSDocument *d=uri?ls_doc_find(s,uri):NULL; if(d) ls_document_symbols(d,&r); else str_put(&r,"[]"); }
            { char *idcopy=idstart?ls_strndup(idstart,(size_t)(idend-idstart)):xstrdup0("null"); ls_send_response(idcopy,r.data); free(idcopy); } str_free(&r);
        }
        free(uri); free(method); return;
    }
    if (strcmp(method, "workspace/symbol") == 0) {
        const char *params_s,*params_e; char *query=NULL;
        if (ls_json_get_value(json,json+len,"params",&params_s,&params_e)) ls_json_get_string(params_s,params_e,"query",&query);
        { Str r; str_init(&r); ls_workspace_symbols(s,query?query:"",&r); { char *idcopy=idstart?ls_strndup(idstart,(size_t)(idend-idstart)):xstrdup0("null"); ls_send_response(idcopy,r.data); free(idcopy);} str_free(&r); }
        free(query); free(method); return;
    }
    if (idstart) { char *idcopy = ls_strndup(idstart, (size_t)(idend-idstart)); ls_send_error(idcopy, -32601, "Method not supported by cxxe-language-server"); free(idcopy); }
    free(uri); free(method);
}

int main(int argc, char **argv) {
    LSServer server;
    char *payload = NULL;
    size_t len = 0;
    int running = 1;
    memset(&server, 0, sizeof(server));
    g_cxxe_executable_dir = cxxe_executable_dir0(argc > 0 ? argv[0] : "cxxe-language-server");
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
        printf("cxxe-language-server %s\n", CXXELS_VERSION);
        free(g_cxxe_executable_dir); g_cxxe_executable_dir = NULL; return 0;
    }
    while (running && ls_read_message(&payload, &len)) {
        ls_handle_message(&server, payload, len, &running);
        free(payload); payload = NULL; len = 0;
    }
    ls_server_free(&server);
    free(g_cxxe_executable_dir); g_cxxe_executable_dir = NULL;
    return 0;
}

#endif /* CXXE_LSP_BUILD */
