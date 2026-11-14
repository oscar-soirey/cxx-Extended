/*
 * Core/StrBuf.h
 *
 * Growable byte/string buffer (Str).
 */

#ifndef CXXE_CORE_STR_BUF_H
#define CXXE_CORE_STR_BUF_H

#include "Core/Common.h"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Str;

void str_init(Str *s);
void str_putn(Str *s, const char *p, size_t n);
void str_put(Str *s, const char *p);
void str_ch(Str *s, char c);
char *str_take(Str *s);
void str_free(Str *s);
void str_trim_ascii(Str *s);

#endif /* CXXE_CORE_STR_BUF_H */
