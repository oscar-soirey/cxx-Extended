/*
 * Lexer/Lexer.c
 *
 * Lossless C++ / CXXE lexer.
 */

#include "Core/Common.h"
#include "Lexer/Lexer.h"
#include "Core/Memory.h"
#include "Lexer/Token.h"

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

void lex_cpp(const uint8_t *src, size_t n, TokenVec *out) {
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
                t.byte_end = tok_end;
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

void tokens_free(TokenVec *tv) {
    size_t i;
    for (i = 0; i < tv->n; ++i) {
        free(tv->v[i].trivia);
        free(tv->v[i].text);
    }
    free(tv->v);
    memset(tv, 0, sizeof(*tv));
}
