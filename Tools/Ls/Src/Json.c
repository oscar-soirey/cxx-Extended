/*
 * Json.c
 *
 * Recursive-descent JSON parser (RFC 8259) producing a small DOM.
 */

#include "Json.h"
#include "Core/Memory.h"

typedef struct { const char *p, *end; int failed; } JsonParser;

static void ws(JsonParser *jp) {
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) jp->p++;
}

static Json *node(JsonType t) {
    Json *j = (Json *)xcalloc(1, sizeof(Json));
    j->type = t;
    return j;
}

static void put_utf8(Str *s, unsigned cp) {
    if (cp < 0x80) str_ch(s, (char)cp);
    else if (cp < 0x800) { str_ch(s, (char)(0xC0 | (cp >> 6))); str_ch(s, (char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { str_ch(s, (char)(0xE0 | (cp >> 12))); str_ch(s, (char)(0x80 | ((cp >> 6) & 0x3F))); str_ch(s, (char)(0x80 | (cp & 0x3F))); }
    else { str_ch(s, (char)(0xF0 | (cp >> 18))); str_ch(s, (char)(0x80 | ((cp >> 12) & 0x3F))); str_ch(s, (char)(0x80 | ((cp >> 6) & 0x3F))); str_ch(s, (char)(0x80 | (cp & 0x3F))); }
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static char *parse_string_raw(JsonParser *jp) {
    Str s;
    str_init(&s);
    jp->p++; /* opening quote */
    while (jp->p < jp->end && *jp->p != '"') {
        char c = *jp->p++;
        if (c != '\\') { str_ch(&s, c); continue; }
        if (jp->p >= jp->end) break;
        c = *jp->p++;
        switch (c) {
        case '"': str_ch(&s, '"'); break;
        case '\\': str_ch(&s, '\\'); break;
        case '/': str_ch(&s, '/'); break;
        case 'b': str_ch(&s, '\b'); break;
        case 'f': str_ch(&s, '\f'); break;
        case 'n': str_ch(&s, '\n'); break;
        case 'r': str_ch(&s, '\r'); break;
        case 't': str_ch(&s, '\t'); break;
        case 'u': {
            unsigned cp, lo;
            if (jp->end - jp->p < 4 || !hex4(jp->p, &cp)) { jp->failed = 1; break; }
            jp->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && jp->end - jp->p >= 6 && jp->p[0] == '\\' && jp->p[1] == 'u' && hex4(jp->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                jp->p += 6;
            }
            put_utf8(&s, cp);
            break;
        }
        default: jp->failed = 1; break;
        }
    }
    if (jp->p >= jp->end) jp->failed = 1; else jp->p++;
    if (!s.data) str_put(&s, "");
    return str_take(&s);
}

static Json *parse_value(JsonParser *jp, int depth) {
    Json *j = NULL;
    const char *begin;
    ws(jp);
    if (jp->p >= jp->end || depth > 200) { jp->failed = 1; return NULL; }
    begin = jp->p;
    if (*jp->p == '{' || *jp->p == '[') {
        int is_obj = *jp->p == '{';
        Json *last = NULL;
        j = node(is_obj ? JSON_OBJECT : JSON_ARRAY);
        jp->p++;
        ws(jp);
        if (jp->p < jp->end && *jp->p == (is_obj ? '}' : ']')) { jp->p++; }
        else for (;;) {
            char *key = NULL;
            Json *v;
            ws(jp);
            if (is_obj) {
                if (jp->p >= jp->end || *jp->p != '"') { jp->failed = 1; break; }
                key = parse_string_raw(jp);
                ws(jp);
                if (jp->p >= jp->end || *jp->p != ':') { free(key); jp->failed = 1; break; }
                jp->p++;
            }
            v = parse_value(jp, depth + 1);
            if (!v) { free(key); jp->failed = 1; break; }
            v->key = key;
            if (last) last->next = v; else j->child = v;
            last = v;
            ws(jp);
            if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
            if (jp->p < jp->end && *jp->p == (is_obj ? '}' : ']')) { jp->p++; break; }
            jp->failed = 1;
            break;
        }
    } else if (*jp->p == '"') {
        j = node(JSON_STRING);
        j->string = parse_string_raw(jp);
    } else if (jp->end - jp->p >= 4 && !strncmp(jp->p, "true", 4)) {
        j = node(JSON_BOOL); j->boolean = 1; jp->p += 4;
    } else if (jp->end - jp->p >= 5 && !strncmp(jp->p, "false", 5)) {
        j = node(JSON_BOOL); jp->p += 5;
    } else if (jp->end - jp->p >= 4 && !strncmp(jp->p, "null", 4)) {
        j = node(JSON_NULL); jp->p += 4;
    } else if (*jp->p == '-' || (*jp->p >= '0' && *jp->p <= '9')) {
        char buf[64];
        size_t n = 0;
        while (jp->p < jp->end && n + 1 < sizeof(buf) && strchr("+-0123456789.eE", *jp->p)) buf[n++] = *jp->p++;
        buf[n] = 0;
        j = node(JSON_NUMBER);
        j->number = strtod(buf, NULL);
    } else {
        jp->failed = 1;
        return NULL;
    }
    j->raw_begin = begin;
    j->raw_end = jp->p;
    return j;
}

Json *json_parse(const char *text, size_t len) {
    JsonParser jp;
    Json *j;
    jp.p = text; jp.end = text + len; jp.failed = 0;
    j = parse_value(&jp, 0);
    if (jp.failed) { json_free(j); return NULL; }
    return j;
}

void json_free(Json *j) {
    while (j) {
        Json *next = j->next;
        json_free(j->child);
        free(j->string);
        free(j->key);
        free(j);
        j = next;
    }
}

Json *json_get(const Json *obj, const char *key) {
    Json *c;
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (c = obj->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

Json *json_path(const Json *obj, const char *path) {
    char part[128];
    const Json *cur = obj;
    while (cur && *path) {
        size_t n = 0;
        while (path[n] && path[n] != '.' && n + 1 < sizeof(part)) { part[n] = path[n]; n++; }
        part[n] = 0;
        cur = json_get(cur, part);
        path += n;
        if (*path == '.') path++;
    }
    return (Json *)cur;
}

const char *json_str(const Json *j, const char *path) {
    const Json *v = path ? json_path(j, path) : j;
    return v && v->type == JSON_STRING ? v->string : NULL;
}

long json_int(const Json *j, const char *path, long fallback) {
    const Json *v = path ? json_path(j, path) : j;
    return v && v->type == JSON_NUMBER ? (long)v->number : fallback;
}

int json_bool(const Json *j, const char *path, int fallback) {
    const Json *v = path ? json_path(j, path) : j;
    return v && v->type == JSON_BOOL ? v->boolean : fallback;
}

size_t json_array_len(const Json *arr) {
    size_t n = 0;
    const Json *c;
    if (!arr || arr->type != JSON_ARRAY) return 0;
    for (c = arr->child; c; c = c->next) n++;
    return n;
}

void json_put_string(Str *out, const char *s) {
    str_ch(out, '"');
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': str_put(out, "\\\""); break;
        case '\\': str_put(out, "\\\\"); break;
        case '\n': str_put(out, "\\n"); break;
        case '\r': str_put(out, "\\r"); break;
        case '\t': str_put(out, "\\t"); break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); str_put(out, b); }
            else str_ch(out, (char)c);
        }
    }
    str_ch(out, '"');
}

void json_put_raw(Str *out, const Json *j) {
    if (!j) { str_put(out, "null"); return; }
    str_putn(out, j->raw_begin, (size_t)(j->raw_end - j->raw_begin));
}
