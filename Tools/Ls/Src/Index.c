/*
 * Index.c
 *
 * Workspace index: every .cppe/.hppe/.he file under the workspace folder,
 * plus headers reached through includes of open documents. Files are lexed
 * and parsed (no semantics) and refreshed when their mtime changes.
 */

#include "Server.h"
#include "Core/Memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static long long file_mtime(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
#endif
}

static char *read_text(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    Str t;
    char buf[8192];
    size_t r;
    if (!f) return NULL;
    str_init(&t);
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) str_putn(&t, buf, r);
    fclose(f);
    *len = t.len;
    return t.data ? str_take(&t) : xstrdup0("");
}

static LsIndexFile *find_file(LsServer *s, const char *path) {
    size_t i;
    for (i = 0; i < s->file_n; ++i) if (path_equal(s->files[i]->unit.path, path)) return s->files[i];
    return NULL;
}

static LsIndexFile *touch_file(LsServer *s, const char *path, int external) {
    LsIndexFile *f = find_file(s, path);
    long long mt = file_mtime(path);
    if (mt < 0) return NULL;
    if (!f) {
        f = (LsIndexFile *)xcalloc(1, sizeof(LsIndexFile));
        f->mtime = -2;
        f->external = external;
        f->unit.path = xstrdup0(path);
        s->files = (LsIndexFile **)xrealloc(s->files, (s->file_n + 1) * sizeof(LsIndexFile *));
        s->files[s->file_n++] = f;
    }
    f->seen = 1;
    if (f->mtime != mt) {
        size_t len = 0;
        char *text = read_text(path, &len);
        if (text) unit_parse_text(&f->unit, path, text, len);
        f->mtime = mt;
    }
    return f;
}

static int indexed_extension(const char *name) {
    return path_has_extension(name, ".cppe") || path_has_extension(name, ".hppe") || path_has_extension(name, ".he");
}

static int skipped_directory(const char *name) {
    static const char *const skip[] = { ".git", ".vs", ".vscode", ".idea", "node_modules", "build", "out", "dist", "bin", "obj", NULL };
    size_t i;
    if (name[0] == '.') return 1;
    if (!strncmp(name, "cmake-build", 11) || !strncmp(name, "build-", 6)) return 1;
    for (i = 0; skip[i]; ++i) if (!strcmp(name, skip[i])) return 1;
    return 0;
}

static void scan_dir(LsServer *s, const char *dir, int depth, size_t *budget) {
    if (depth > 12 || !*budget) return;
#ifdef _WIN32
    {
        char pattern[1200];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            char full[1200];
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!skipped_directory(fd.cFileName)) scan_dir(s, full, depth + 1, budget);
            } else if (indexed_extension(fd.cFileName) && *budget) {
                (*budget)--;
                touch_file(s, full, 0);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (!d) return;
        while ((e = readdir(d)) != NULL) {
            char full[4096];
            struct stat st;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            if (stat(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                if (!skipped_directory(e->d_name)) scan_dir(s, full, depth + 1, budget);
            } else if (indexed_extension(e->d_name) && *budget) {
                (*budget)--;
                touch_file(s, full, 0);
            }
        }
        closedir(d);
    }
#endif
}

void index_refresh(LsServer *s) {
    size_t i, j, budget = 3000;
    for (i = 0; i < s->file_n; ++i) s->files[i]->seen = 0;
    if (s->root) scan_dir(s, s->root, 0, &budget);
    for (i = 0; i < s->file_n; ++i) if (s->files[i]->external) touch_file(s, s->files[i]->unit.path, 1);
    /* Forget deleted files. */
    for (i = j = 0; i < s->file_n; ++i) {
        if (s->files[i]->seen) { s->files[j++] = s->files[i]; continue; }
        unit_free(&s->files[i]->unit);
        free(s->files[i]);
    }
    s->file_n = j;
}

void index_add_external(LsServer *s, const char *path) {
    LsIndexFile *f = find_file(s, path);
    if (f && f->mtime == file_mtime(path)) return;
    touch_file(s, path, f ? f->external : 1);
}

size_t server_units(LsServer *s, LsUnit ***out) {
    size_t i, n = 0;
    LsUnit **v = (LsUnit **)xmalloc((s->doc_n + s->file_n + 1) * sizeof(LsUnit *));
    for (i = 0; i < s->doc_n; ++i) v[n++] = &s->docs[i]->unit;
    for (i = 0; i < s->file_n; ++i)
        if (s->files[i]->unit.has_ir && !server_doc_by_path(s, s->files[i]->unit.path)) v[n++] = &s->files[i]->unit;
    *out = v;
    return n;
}
