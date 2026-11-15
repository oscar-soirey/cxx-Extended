/*
 * Text.h
 *
 * Positions and paths. LSP positions are 0-based lines and UTF-16 code-unit
 * columns; the compiler reports 1-based lines and byte columns.
 */

#ifndef CXXE_LS_TEXT_H
#define CXXE_LS_TEXT_H

#include "Core/Common.h"

typedef struct {
    const char *text;
    size_t len;
    size_t *line_start;
    size_t line_count;
} LineMap;

void linemap_build(LineMap *m, const char *text, size_t len);
void linemap_free(LineMap *m);
/* LSP (line, UTF-16 character) -> byte offset, clamped to the line. */
size_t linemap_offset(const LineMap *m, long line, long character);
/* Byte offset -> LSP (line, UTF-16 character). */
void linemap_position(const LineMap *m, size_t offset, long *line, long *character);
/* Compiler 1-based (line, byte column) -> byte offset. */
size_t linemap_offset_from_compiler(const LineMap *m, unsigned line, unsigned column);

char *uri_to_path(const char *uri);
char *path_to_uri(const char *path);
/* Path comparison as the platform does (case-insensitive, / == \ on Windows). */
int path_equal(const char *a, const char *b);
char *path_parent(const char *path);
const char *path_basename(const char *path);
int path_has_extension(const char *path, const char *ext);

#endif
