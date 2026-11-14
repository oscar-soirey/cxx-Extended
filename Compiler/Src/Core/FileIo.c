/*
 * Core/FileIo.c
 *
 * Whole-file binary read/write helpers.
 */

#include "Core/Common.h"
#include "Core/FileIo.h"
#include "Core/Memory.h"

uint8_t *read_bytes(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    long end;
    size_t n, got;
    uint8_t *p;
    if (!f) die("cannot open '%s': %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek '%s'", path);
    end = ftell(f);
    if (end < 0) die("cannot size '%s'", path);
    if (fseek(f, 0, SEEK_SET) != 0) die("cannot rewind '%s'", path);
    n = (size_t)end;
    p = (uint8_t *)xmalloc(n + 1);
    got = fread(p, 1, n, f);
    fclose(f);
    if (got != n) die("cannot read '%s'", path);
    p[n] = 0;
    if (size_out) *size_out = n;
    return p;
}

void write_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot create '%s': %s", path, strerror(errno));
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        die("cannot write '%s'", path);
    }
    if (fclose(f) != 0) die("cannot close '%s'", path);
}
