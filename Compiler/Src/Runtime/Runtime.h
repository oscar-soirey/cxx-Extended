/*
 * Runtime/Runtime.h
 *
 * Embedded CXXE runtime sources and runtime export command.
 */

#ifndef CXXE_RUNTIME_RUNTIME_H
#define CXXE_RUNTIME_RUNTIME_H

#include "Core/Common.h"

extern const char CXXE_RUNTIME_HPP[];
extern const char CXXE_RUNTIME_CPP[];
int cxxe_runtime_command(int argc, char **argv);
extern const size_t CXXE_RUNTIME_HPP_SIZE;
extern const size_t CXXE_RUNTIME_CPP_SIZE;

#endif /* CXXE_RUNTIME_RUNTIME_H */
