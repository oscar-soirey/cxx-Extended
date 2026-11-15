/*
 * Text.c
 *
 * Line maps (UTF-8 <-> UTF-16 columns), file URIs and path helpers.
 */

#include "Text.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"

void linemap_build(LineMap *m, const char *text, size_t len) {
    size_t i, cap = 64;
    m->text = text;
    m->len = len;
    m->line_start = (size_t *)xmalloc(cap * sizeof(size_t));
    m->line_count = 0;
    m->line_start[m->line_count++] = 0;
    for (i = 0; i < len; ++i) {
        if (text[i] != '\n') continue;
        if (m->line_count == cap) { cap *= 2; m->line_start = (size_t *)xrealloc(m->line_start, cap * sizeof(size_t)); }
        m->line_start[m->line_count++] = i + 1;
    }
}

void linemap_free(LineMap *m) {
    free(m->line_start);
    memset(m, 0, sizeof(*m));
}

static size_t line_end(const LineMap *m, size_t line) {
    size_t e = line + 1 < m->line_count ? m->line_start[line + 1] - 1 : m->len;
    if (e > m->line_start[line] && m->text[e - 1] == '\r') e--;
    return e;
}

/* Byte length of the UTF-8 sequence starting with c. */
static size_t utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t linemap_offset(const LineMap *m, long line, long character) {
    size_t off, end;
    long units = 0;
    if (line < 0) return 0;
    if ((size_t)line >= m->line_count) return m->len;
    off = m->line_start[line];
    end = line_end(m, (size_t)line);
    while (off < end && units < character) {
        size_t n = utf8_len((unsigned char)m->text[off]);
        units += n == 4 ? 2 : 1;
        off += n;
    }
    return off > end ? end : off;
}

void linemap_position(const LineMap *m, size_t offset, long *line, long *character) {
    size_t lo = 0, hi = m->line_count, off;
    long units = 0;
    if (offset > m->len) offset = m->len;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (m->line_start[mid] <= offset) lo = mid; else hi = mid;
    }
    for (off = m->line_start[lo]; off < offset;) {
        size_t n = utf8_len((unsigned char)m->text[off]);
        units += n == 4 ? 2 : 1;
        off += n;
    }
    *line = (long)lo;
    *character = units;
}

size_t linemap_offset_from_compiler(const LineMap *m, unsigned line, unsigned column) {
    size_t off;
    if (line == 0) return 0;
    if (line - 1 >= m->line_count) return m->len;
    off = m->line_start[line - 1] + (column ? column - 1 : 0);
    return off > m->len ? m->len : off;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *uri_to_path(const char *uri) {
    Str s;
    const char *p = uri;
    str_init(&s);
    if (!uri) return xstrdup0("");
    if (!strncmp(p, "file://", 7)) p += 7;
    for (; *p; ++p) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { str_ch(&s, (char)(hexval(p[1]) * 16 + hexval(p[2]))); p += 2; }
        else str_ch(&s, *p);
    }
    if (!s.data) str_put(&s, "");
#ifdef _WIN32
    /* /c:/dir/file -> c:\dir\file */
    if (s.len >= 3 && s.data[0] == '/' && isalpha((unsigned char)s.data[1]) && s.data[2] == ':') {
        memmove(s.data, s.data + 1, s.len);
        s.len--;
    }
    {
        size_t i;
        for (i = 0; i < s.len; ++i) if (s.data[i] == '/') s.data[i] = '\\';
    }
#endif
    return str_take(&s);
}

char *path_to_uri(const char *path) {
    Str s;
    const unsigned char *p = (const unsigned char *)path;
    str_init(&s);
    str_put(&s, "file://");
    if (!(p[0] == '/' || p[0] == '\\')) str_ch(&s, '/');
    for (; *p; ++p) {
        unsigned char c = *p;
        if (c == '\\') c = '/';
        if (isalnum(c) || strchr("/-._~:", c)) str_ch(&s, (char)c);
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); str_put(&s, b); }
    }
    return str_take(&s);
}

int path_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
#ifdef _WIN32
    for (; *a && *b; ++a, ++b) {
        char x = *a == '/' ? '\\' : (char)tolower((unsigned char)*a);
        char y = *b == '/' ? '\\' : (char)tolower((unsigned char)*b);
        if (x != y) return 0;
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

char *path_parent(const char *path) {
    const char *slash = NULL, *p;
    for (p = path; *p; ++p) if (*p == '/' || *p == '\\') slash = p;
    if (!slash) return xstrdup0(".");
    if (slash == path) return xstrdup0("/");
    return xstrndup0(path, (size_t)(slash - path));
}

const char *path_basename(const char *path) {
    const char *b = path, *p;
    for (p = path; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

int path_has_extension(const char *path, const char *ext) {
    size_t pl = strlen(path), el = strlen(ext);
    size_t i;
    if (pl < el) return 0;
    for (i = 0; i < el; ++i)
        if (tolower((unsigned char)path[pl - el + i]) != tolower((unsigned char)ext[i])) return 0;
    return 1;
}
