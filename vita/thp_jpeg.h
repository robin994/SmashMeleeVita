#pragma once
#include <stddef.h>
#include <stdint.h>
/* Convert one baseline THP JPEG payload (without MTH's next-size word).
 * Input/output must not overlap. Output capacity >= 2*input_size is sufficient.
 * Returns zero on success; rejects malformed headers/missing EOI/capacity errors. */
int mv_thp_jpeg_unpack(const uint8_t *input, size_t size, uint8_t *output,
                       size_t capacity, size_t *written);
