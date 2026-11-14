/*
 * Core/FileIo.h
 *
 * Whole-file binary read/write helpers.
 */

#ifndef CXXE_CORE_FILE_IO_H
#define CXXE_CORE_FILE_IO_H

#include "Core/Common.h"

uint8_t *read_bytes(const char *path, size_t *size_out);
void write_bytes(const char *path, const void *data, size_t n);

#endif /* CXXE_CORE_FILE_IO_H */
