/*
 * Core/Path.h
 *
 * Path manipulation and executable directory discovery.
 */

#ifndef CXXE_CORE_PATH_H
#define CXXE_CORE_PATH_H

#include "Core/Common.h"

extern char *g_cxxe_executable_dir;
char *cxxe_executable_dir0(const char *argv0);
char *path_dirname0(const char *path);
char *path_join0(const char *dir, const char *name);
char *normalize_path0(const char *path);
int file_exists0(const char *path);

#endif /* CXXE_CORE_PATH_H */
