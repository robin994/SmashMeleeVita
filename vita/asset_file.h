#pragma once
#include <stddef.h>
/* Allocated buffer belongs to caller; limit prevents unbounded on-device reads. */
int mv_read_file(const char *path, unsigned char **bytes, size_t *size);
