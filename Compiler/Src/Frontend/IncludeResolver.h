/*
 * Frontend/IncludeResolver.h
 *
 * Include resolution and discovery of decorators declared in included headers.
 */

#ifndef CXXE_FRONTEND_INCLUDE_RESOLVER_H
#define CXXE_FRONTEND_INCLUDE_RESOLVER_H

#include "Core/Common.h"
#include "Ir/Ir.h"

typedef struct {
    char **visited;
    size_t visited_n, visited_cap;
    char **include_dirs;
    size_t include_dir_n, include_dir_cap;
} IncludeContext;

void discover_included_decorators(IR *ir, const char *main_path, const char *const *include_dirs, size_t include_dir_n);

#endif /* CXXE_FRONTEND_INCLUDE_RESOLVER_H */
