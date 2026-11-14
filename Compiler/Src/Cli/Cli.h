/*
 * Cli/Cli.h
 *
 * Command line help and error reporting.
 */

#ifndef CXXE_CLI_CLI_H
#define CXXE_CLI_CLI_H

#include "Core/Common.h"

void print_version(void);
void usage(FILE *stream);
int invalid_option(const char *arg);
int command_usage(const char *command);

#endif /* CXXE_CLI_CLI_H */
