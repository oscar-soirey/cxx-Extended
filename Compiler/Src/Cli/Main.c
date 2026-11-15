/*
 * Cli/Main.c
 *
 * CXXE stand-alone C++ extended source -> IR -> C++ converter.
 *
 * IMPORTANT:
 *   - This file DOES NOT compile C++.
 *   - It DOES NOT call clang, llvm, libclang, a compiler driver, or a linker.
 *   - It lexes/parses the source itself and stores a lossless token stream.
 *   - The IR contains both generic syntax and semantic-oriented records.
 *
 * The design goal is: every input token survives the IR round-trip, while
 * the parser recognizes as many C++ constructs as possible and keeps unknown
 * constructs as generic nodes instead of discarding them. This makes the IR
 * suitable as the stable input to a future C+++ frontend/backend.
 *
 * Build:
 *   see Compiler/README.md (CMake or Build.bat)
 *
 * Usage:
 *   cxxe parse input.cpp output.cpir
 *   cxxe emit output.cpir output.cpp
 *   cxxe roundtrip input.cpp output.cpp
 *   cxxe dump output.cpir
 *
 * CPIR format: a small binary container intended for this program. It stores
 * the original bytes, the lossless tokens (including leading trivia), and the
 * parsed nodes. Node ranges refer to token indices; generators can therefore
 * either reproduce exact source or regenerate selected nodes later.
 */

#include "Core/Common.h"
#include "Cli/Cli.h"
#include "CodeGen/CodeGen.h"
#include "Core/Memory.h"
#include "Core/Path.h"
#include "Driver/Pipeline.h"
#include "Ir/Ir.h"
#include "Ir/IrDump.h"
#include "Ir/IrSerialize.h"
#include "Runtime/Runtime.h"

int main(int argc, char **argv) {
    IR ir;
    const char **include_dirs = NULL;
    size_t include_dir_n = 0, include_dir_cap = 0;
    int argi = 1;
    const char *command;

    g_cxxe_executable_dir = cxxe_executable_dir0(argv[0]);

    if (argc < 2) { usage(stderr); free(g_cxxe_executable_dir); g_cxxe_executable_dir = NULL; return 2; }

    while (argi < argc) {
        if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            if (argi == 1) { usage(stdout); free(include_dirs); return 0; }
            break;
        }
        if (strcmp(argv[argi], "--version") == 0 || strcmp(argv[argi], "-v") == 0) {
            if (argi == 1) { print_version(); free(include_dirs); return 0; }
            break;
        }
        if (strcmp(argv[argi], "-I") == 0) {
            if (argi + 1 >= argc) { free(include_dirs); return invalid_option("-I"); }
            if (include_dir_n == include_dir_cap) {
                include_dir_cap = include_dir_cap ? include_dir_cap * 2 : 8;
                include_dirs = (const char **)xrealloc((void *)include_dirs, include_dir_cap * sizeof(*include_dirs));
            }
            include_dirs[include_dir_n++] = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (strncmp(argv[argi], "-I", 2) == 0 && argv[argi][2] != 0) {
            if (include_dir_n == include_dir_cap) {
                include_dir_cap = include_dir_cap ? include_dir_cap * 2 : 8;
                include_dirs = (const char **)xrealloc((void *)include_dirs, include_dir_cap * sizeof(*include_dirs));
            }
            include_dirs[include_dir_n++] = argv[argi] + 2;
            argi++;
            continue;
        }
        break;
    }

    if (argi >= argc) { free(include_dirs); usage(stderr); return 2; }
    command = argv[argi++];

    if (strcmp(command, "runtime") == 0) {
        int r = cxxe_runtime_command(argc - argi + 1, argv + argi - 1);
        free(include_dirs);
        return r;
    }

    if (strcmp(command, "parse") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 2) { free(include_dirs); return command_usage("parse"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        if (argv[argi + 1][0] == '-') { int r = invalid_option(argv[argi + 1]); free(include_dirs); return r; }
        build_ir_from_cpp(&ir, argv[argi], include_dirs, include_dir_n);
        save_ir(&ir, argv[argi + 1]);
        printf("cxxe: parsed %s -> %s (%zu nodes, %zu tokens)\n", argv[argi], argv[argi + 1], ir.node_count, ir.tokens.n);
        ir_free(&ir); free(include_dirs); return 0;
    }

    if (strcmp(command, "emit") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 2) { free(include_dirs); return command_usage("emit"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        if (argv[argi + 1][0] == '-') { int r = invalid_option(argv[argi + 1]); free(include_dirs); return r; }
        ir_init(&ir); load_ir(&ir, argv[argi]); emit_exact(&ir, argv[argi + 1]);
        printf("cxxe: emitted %s -> %s\n", argv[argi], argv[argi + 1]);
        ir_free(&ir); free(include_dirs); return 0;
    }

    if (strcmp(command, "roundtrip") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 2) { free(include_dirs); return command_usage("roundtrip"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        if (argv[argi + 1][0] == '-') { int r = invalid_option(argv[argi + 1]); free(include_dirs); return r; }
        build_ir_from_cpp(&ir, argv[argi], include_dirs, include_dir_n);
        save_ir(&ir, ".cxxe.tmp.cpir");
        {
            IR reloaded; ir_init(&reloaded); load_ir(&reloaded, ".cxxe.tmp.cpir");
            reloaded.path = xstrdup0(argv[argi]); /* enables #line mapping to the source */
            emit_exact(&reloaded, argv[argi + 1]); ir_free(&reloaded);
        }
        remove(".cxxe.tmp.cpir"); ir_free(&ir);
        printf("cxxe: round-trip %s -> %s\n", argv[argi], argv[argi + 1]);
        free(include_dirs); return 0;
    }

    if (strcmp(command, "dump") == 0) {
        if (argi < argc && (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0)) { usage(stdout); free(include_dirs); return 0; }
        if (argc - argi != 1) { free(include_dirs); return command_usage("dump"); }
        if (argv[argi][0] == '-') { int r = invalid_option(argv[argi]); free(include_dirs); return r; }
        ir_init(&ir); load_ir(&ir, argv[argi]); dump_ir(&ir); ir_free(&ir); free(include_dirs); return 0;
    }

    free(include_dirs);
    fprintf(stderr, "cxxe: error: unknown command '%s'\n", command);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\n");
    return 2;
}
