#include "asset_file.h"
#include <stdio.h>
#include <stdlib.h>

int mv_read_file(const char *path, unsigned char **bytes, size_t *size)
{
    *bytes = NULL; *size = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return -1; }
    long length = ftell(file);
    if (length < 32 || length > 64 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        fclose(file); return -1;
    }
    unsigned char *buffer = malloc((size_t)length);
    if (!buffer) { fclose(file); return -1; }
    size_t got = fread(buffer, 1, (size_t)length, file);
    int failed = ferror(file);
    fclose(file);
    if (got != (size_t)length || failed) { free(buffer); return -1; }
    *bytes = buffer; *size = (size_t)length;
    return 0;
}
