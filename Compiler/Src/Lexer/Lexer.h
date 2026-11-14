/*
 * Lexer/Lexer.h
 *
 * Lossless C++ / CXXE lexer.
 */

#ifndef CXXE_LEXER_LEXER_H
#define CXXE_LEXER_LEXER_H

#include "Core/Common.h"
#include "Lexer/Token.h"

void lex_cpp(const uint8_t *src, size_t n, TokenVec *out);
void tokens_free(TokenVec *tv);

#endif /* CXXE_LEXER_LEXER_H */
