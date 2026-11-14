/*
 * Core/StrBuf.c
 *
 * Growable byte/string buffer (Str).
 */

#include "Core/Common.h"
#include "Core/StrBuf.h"
#include "Core/Memory.h"

void str_init(Str *s) { memset(s, 0, sizeof(*s)); }

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

void str_putn(Str *s, const char *p, size_t n) {
    if (!n) return;
    str_reserve(s, n);
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = 0;
}

void str_put(Str *s, const char *p) {
    if (p) str_putn(s, p, strlen(p));
}

void str_ch(Str *s, char c) {
    str_reserve(s, 1);
    s->data[s->len++] = c;
    s->data[s->len] = 0;
}

char *str_take(Str *s) {
    char *r = s->data ? s->data : xstrdup0("");
    s->data = NULL;
    s->len = s->cap = 0;
    return r;
}

void str_free(Str *s) {
    free(s->data);
    memset(s, 0, sizeof(*s));
}

void str_trim_ascii(Str *s) {
    size_t a = 0, b = s->len;
    while (a < b && isspace((unsigned char)s->data[a])) a++;
    while (b > a && isspace((unsigned char)s->data[b - 1])) b--;
    if (a) memmove(s->data, s->data + a, b - a);
    s->len = b - a;
    if (s->data) s->data[s->len] = 0;
}
