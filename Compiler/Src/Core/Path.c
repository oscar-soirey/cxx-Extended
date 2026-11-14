/*
 * Core/Path.c
 *
 * Path manipulation and executable directory discovery.
 */

#include "Core/Common.h"
#include "Core/Path.h"
#include "Core/Memory.h"
#include "Core/StrBuf.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

char *g_cxxe_executable_dir = NULL;

char *cxxe_executable_dir0(const char *argv0) {
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

char *path_dirname0(const char *path) {
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = a ? (b ? (a > b ? a : b) : a) : b;
    if (!p) return xstrdup0(".");
    if (p == path) return xstrndup0(path, 1);
    return xstrndup0(path, (size_t)(p - path));
}

char *path_join0(const char *dir, const char *name) {
    size_t a = strlen(dir), b = strlen(name);
    Str s; str_init(&s);
    str_putn(&s, dir, a);
    if (a && dir[a - 1] != '/' && dir[a - 1] != '\\') str_ch(&s, '/');
    str_putn(&s, name, b);
    return str_take(&s);
}

char *normalize_path0(const char *path) {
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

int file_exists0(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
