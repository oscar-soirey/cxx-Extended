/*
 * Json.h
 *
 * Minimal JSON DOM for the language server protocol: parse a message into a
 * tree, query it, and build replies with Str + json_put_string.
 */

#ifndef CXXE_LS_JSON_H
#define CXXE_LS_JSON_H

#include "Core/Common.h"
#include "Core/StrBuf.h"

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct Json Json;
struct Json {
    JsonType type;
    double number;
    int boolean;
    char *string;          /* JSON_STRING (decoded UTF-8) */
    char *key;             /* member name when inside an object */
    Json *child, *next;    /* first child / next sibling */
    const char *raw_begin; /* exact source span, used to echo request ids */
    const char *raw_end;
};

Json *json_parse(const char *text, size_t len);
void json_free(Json *j);

Json *json_get(const Json *obj, const char *key);
/* Dotted path lookup: json_path(msg, "params.textDocument.uri"). */
Json *json_path(const Json *obj, const char *path);
const char *json_str(const Json *j, const char *path);           /* NULL if absent */
long json_int(const Json *j, const char *path, long fallback);
int json_bool(const Json *j, const char *path, int fallback);
size_t json_array_len(const Json *arr);

void json_put_string(Str *out, const char *s);
void json_put_raw(Str *out, const Json *j);  /* re-emit the exact source text */

#endif
