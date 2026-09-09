#pragma once
#include <stddef.h>
#include <stdint.h>
/* Base mip level, GX tiled source -> linear RGBA bytes. No TEV/material shading. */
size_t mv_gx_texture_size(unsigned width, unsigned height, unsigned format);
int mv_gx_decode(uint8_t *rgba, size_t capacity, size_t stride,
                 const uint8_t *source, size_t source_size,
                 unsigned width, unsigned height, unsigned format,
                 const uint8_t *palette, size_t palette_size, unsigned palette_format);
