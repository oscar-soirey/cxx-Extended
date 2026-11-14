/*
 * Cli/Cli.c
 *
 * Command line help and error reporting.
 */

#include "Core/Common.h"
#include "Cli/Cli.h"

void print_version(void) {
    printf("CXXE 0.1\n");
}

void usage(FILE *stream) {
    fprintf(stream,
        "CXXE 0.1 - C++ Extended Compiler - C++ source <-> CPIR converter\n"
        "\n"
        "Usage:\n"
        "  cxxe [options] <command> [arguments]\n"
        "\n"
        "Commands:\n"
        "  parse <input.cpp> <output.cpir>    Parse C++ into CPIR\n"
        "  emit <input.cpir> <output.cpp>     Generate C++ from CPIR\n"
        "  roundtrip <input.cpp> <output.cpp> Parse then generate C++\n"
        "  dump <input.cpir>                  Display CPIR information\n"
        "  runtime export <directory>          Export cxxe_runtime.hpp/.cpp\n"
        "  runtime header <output.hpp|->       Export only the runtime header\n"
        "  runtime source <output.cpp|->       Export only the runtime source\n"
        "\n"
        "Options:\n"
        "  -h, --help                         Show this help message\n"
        "  -v, --version                      Show version\n"
        "  -I <dir>                           Add an include search directory\n"
        "\n"
        "Examples:\n"
        "  cxxe parse main.cpp main.cpir\n"
        "  cxxe emit main.cpir main.cpp\n"
        "  cxxe roundtrip main.cpp generated.cpp\n"
        "  cxxe dump main.cpir\n");
}

int invalid_option(const char *arg) {
    fprintf(stderr, "CXXE: error: invalid option '%s'\n", arg);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\n");
    return 2;
}

int command_usage(const char *command) {
    fprintf(stderr, "cxxe: error: invalid arguments for command '%s'\n", command);
    fprintf(stderr, "Try 'cxxe.exe --help' for more information.\n");
    return 2;
}
