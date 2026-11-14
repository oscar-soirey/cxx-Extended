/*
 * Lexer/Token.h
 *
 * Lossless token model: every byte of input (trivia included) is kept.
 */

#ifndef CXXE_LEXER_TOKEN_H
#define CXXE_LEXER_TOKEN_H

#include "Core/Common.h"

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

#endif /* CXXE_LEXER_TOKEN_H */
