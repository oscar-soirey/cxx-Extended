/*
 * Core/Common.h
 *
 * Common system includes and project-wide constants.
 */

#ifndef CXXE_CORE_COMMON_H
#define CXXE_CORE_COMMON_H



#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_GROW_MIN 64u

#endif /* CXXE_CORE_COMMON_H */
