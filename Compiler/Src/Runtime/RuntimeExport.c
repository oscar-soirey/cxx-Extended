/*
 * Runtime/RuntimeExport.c
 *
 * `cxxe runtime ...` command: export the embedded runtime to disk.
 */

#include "Core/Common.h"
#include "Runtime/Runtime.h"
#include "Cli/Cli.h"
#include "Core/Memory.h"

#ifdef _WIN32
#include <direct.h>
#define CXXE_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define CXXE_MKDIR(path) mkdir((path), 0777)
#endif

static int cxxe_write_text_file(const char *path, const char *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cxxe: error: cannot create '%s': %s\\n", path, strerror(errno));
        return 0;
    }
    if (size && fwrite(data, 1, size, f) != size) {
        fclose(f);
        fprintf(stderr, "cxxe: error: cannot write '%s'\\n", path);
        return 0;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "cxxe: error: cannot close '%s'\\n", path);
        return 0;
    }
    return 1;
}

static int cxxe_mkdir_one(const char *path) {
    if (!path || !*path) return 1;
    if (CXXE_MKDIR(path) == 0) return 1;
    if (errno == EEXIST) return 1;
    return 0;
}

static int cxxe_make_directory_recursive(const char *path) {
    char *tmp;
    size_t n, i;
    if (!path || !*path) return 0;
    tmp = xstrdup0(path);
    n = strlen(tmp);
    for (i = 0; i < n; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            if (i == 0) continue;
            if (i == 2 && tmp[1] == ':') continue; /* Windows drive prefix. */
            tmp[i] = 0;
            if (*tmp && !cxxe_mkdir_one(tmp)) {
                free(tmp);
                return 0;
            }
            tmp[i] = saved;
        }
    }
    if (!cxxe_mkdir_one(tmp)) {
        free(tmp);
        return 0;
    }
    free(tmp);
    return 1;
}

static int cxxe_runtime_emit_one(const char *kind, const char *output) {
    const char *data;
    size_t size;
    if (!kind || !output) return 0;
    if (strcmp(kind, "header") == 0) {
        data = CXXE_RUNTIME_HPP;
        size = CXXE_RUNTIME_HPP_SIZE;
    } else if (strcmp(kind, "source") == 0) {
        data = CXXE_RUNTIME_CPP;
        size = CXXE_RUNTIME_CPP_SIZE;
    } else {
        fprintf(stderr, "cxxe: error: unknown runtime artifact '%s'\\n", kind);
        return 0;
    }
    if (strcmp(output, "-") == 0) {
        if (size && fwrite(data, 1, size, stdout) != size) return 0;
        return 1;
    }
    return cxxe_write_text_file(output, data, size);
}

static int cxxe_runtime_export(const char *directory) {
    char *hpp, *cpp;
    size_t n;
    if (!directory || !*directory) return 0;
    if (!cxxe_make_directory_recursive(directory)) {
        fprintf(stderr, "cxxe: error: cannot create runtime directory '%s': %s\\n", directory, strerror(errno));
        return 0;
    }
    n = strlen(directory);
    hpp = (char *)xmalloc(n + strlen("/cxxe_runtime.hpp") + 1);
    cpp = (char *)xmalloc(n + strlen("/cxxe_runtime.cpp") + 1);
    sprintf(hpp, "%s/cxxe_runtime.hpp", directory);
    sprintf(cpp, "%s/cxxe_runtime.cpp", directory);
    if (!cxxe_runtime_emit_one("header", hpp) || !cxxe_runtime_emit_one("source", cpp)) {
        free(hpp); free(cpp);
        return 0;
    }
    free(hpp);
    free(cpp);
    return 1;
}

int cxxe_runtime_command(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "cxxe: error: runtime expects 'export', 'header', or 'source'\\n");
        return 2;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf(
            "Usage: cxxe runtime <command> <output>\n"
            "\n"
            "Commands:\n"
            "  export <directory>    Write cxxe_runtime.hpp and cxxe_runtime.cpp\n"
            "  header <output.hpp|->  Write only cxxe_runtime.hpp\n"
            "  source <output.cpp|->  Write only cxxe_runtime.cpp\n"
            "\n"
            "Use '-' as the output path to write the requested artifact to stdout.\n");
        return 0;
    }
    if (strcmp(argv[1], "export") == 0) {
        if (argc != 3 || !*argv[2]) return command_usage("runtime export");
        return cxxe_runtime_export(argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "header") == 0) {
        if (argc != 3 || !*argv[2]) return command_usage("runtime header");
        return cxxe_runtime_emit_one("header", argv[2]) ? 0 : 1;
    }
    if (strcmp(argv[1], "source") == 0) {
        if (argc != 3 || !*argv[2]) return command_usage("runtime source");
        return cxxe_runtime_emit_one("source", argv[2]) ? 0 : 1;
    }
    fprintf(stderr, "cxxe: error: unknown runtime command '%s'\\n", argv[1]);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\\n");
    return 2;
}
