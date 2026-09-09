#include "gx_texture.h"
#include "hsd_data.h"
#include <string.h>

static int geometry(unsigned format, unsigned *w, unsigned *h, unsigned *bytes)
{
    *bytes = 32;
    switch (format) {
    case 0: case 8: case 14: *w = 8; *h = 8; return 1;
    case 1: case 2: case 9: *w = 8; *h = 4; return 1;
    case 3: case 4: case 5: case 10: *w = 4; *h = 4; return 1;
    case 6: *w = 4; *h = 4; *bytes = 64; return 1;
    default: return 0;
    }
}
size_t mv_gx_texture_size(unsigned width, unsigned height, unsigned format)
{
    unsigned w, h, bytes;
    if (!width || !height || width > 1024 || height > 1024 ||
        !geometry(format, &w, &h, &bytes)) return 0;
    return (size_t)((width + w - 1) / w) * ((height + h - 1) / h) * bytes;
}
static uint8_t expand5(unsigned n) { return (uint8_t)((n << 3) | (n >> 2)); }
static uint8_t expand6(unsigned n) { return (uint8_t)((n << 2) | (n >> 4)); }
static void rgb565(uint16_t v, uint8_t *c)
{
    c[0] = expand5(v >> 11); c[1] = expand6((v >> 5) & 63);
    c[2] = expand5(v & 31); c[3] = 255;
}
static void rgb5a3(uint16_t v, uint8_t *c)
{
    if (v & 0x8000) {
        c[0] = expand5((v >> 10) & 31); c[1] = expand5((v >> 5) & 31);
        c[2] = expand5(v & 31); c[3] = 255;
    } else {
        unsigned alpha = v >> 12;
        c[0] = (uint8_t)(((v >> 8) & 15) * 17);
        c[1] = (uint8_t)(((v >> 4) & 15) * 17); c[2] = (uint8_t)((v & 15) * 17);
        c[3] = (uint8_t)((alpha << 5) | (alpha << 2) | (alpha >> 1));
    }
}
static int indexed(uint8_t *c, unsigned index, const uint8_t *pal, size_t size, unsigned fmt)
{
    if (!pal || (size_t)index * 2 + 2 > size) return -1;
    uint16_t v = mv_be16(pal + index * 2);
    switch (fmt) {
    case 0: c[0] = c[1] = c[2] = (uint8_t)v; c[3] = (uint8_t)(v >> 8); break;
    case 1: rgb565(v, c); break;
    case 2: rgb5a3(v, c); break;
    default: return -1;
    }
    return 0;
}
static void cmpr(const uint8_t *block, unsigned x, unsigned y, uint8_t *c)
{
    /* Four 4x4 subblocks in each 8x8 GX tile. Big-endian, MSB-first indices. */
    const uint8_t *s = block + ((y / 4) * 2 + x / 4) * 8;
    uint16_t a = mv_be16(s), b = mv_be16(s + 2);
    unsigned selector = (s[4 + y % 4] >> (6 - (x % 4) * 2)) & 3;
    uint8_t first[4], second[4];
    rgb565(a, first); rgb565(b, second);
    if (selector < 2) { memcpy(c, selector ? second : first, 4); return; }
    for (unsigned k = 0; k < 3; ++k) {
        if (a > b) {
            /* GX uses 5/8,3/8 interpolation, unlike PC DXT1's thirds. */
            unsigned weight = selector == 2 ? 5 : 3;
            c[k] = (uint8_t)((first[k] * weight + second[k] * (8 - weight)) / 8);
        } else c[k] = (uint8_t)((first[k] + second[k]) / 2);
    }
    c[3] = (a <= b && selector == 3) ? 0 : 255;
}
int mv_gx_decode(uint8_t *rgba, size_t capacity, size_t stride,
                 const uint8_t *source, size_t source_size,
                 unsigned width, unsigned height, unsigned format,
                 const uint8_t *palette, size_t palette_size, unsigned palette_format)
{
    size_t needed = mv_gx_texture_size(width, height, format);
    if (!needed || !rgba || !source || source_size < needed || stride < (size_t)width * 4 ||
        capacity < (size_t)width * 4 || (size_t)(height - 1) > (capacity - (size_t)width * 4) / stride)
        return -1;
    unsigned bw, bh, bytes;
    if (!geometry(format, &bw, &bh, &bytes)) return -1;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            size_t tile = (size_t)(y / bh) * ((width + bw - 1) / bw) + x / bw;
            const uint8_t *block = source + tile * bytes;
            unsigned pixel = (y % bh) * bw + x % bw;
            uint8_t *c = rgba + y * stride + x * 4;
            unsigned v;
            switch (format) {
            case 0: v = (block[pixel / 2] >> ((1 - pixel % 2) * 4)) & 15;
                memset(c, (int)(v * 17), 4); break;
            case 1: memset(c, block[pixel], 4); break;
            case 2: v = block[pixel]; c[0] = c[1] = c[2] = (uint8_t)((v & 15) * 17);
                c[3] = (uint8_t)((v >> 4) * 17); break;
            case 3: c[0] = c[1] = c[2] = block[pixel * 2 + 1]; c[3] = block[pixel * 2]; break;
            case 4: rgb565(mv_be16(block + pixel * 2), c); break;
            case 5: rgb5a3(mv_be16(block + pixel * 2), c); break;
            case 6: c[0] = block[pixel * 2 + 1]; c[1] = block[32 + pixel * 2];
                c[2] = block[33 + pixel * 2]; c[3] = block[pixel * 2]; break;
            case 8: v = (block[pixel / 2] >> ((1 - pixel % 2) * 4)) & 15;
                if (indexed(c, v, palette, palette_size, palette_format)) return -1;
                break;
            case 9: if (indexed(c, block[pixel], palette, palette_size, palette_format)) return -1;
                break;
            case 10: v = mv_be16(block + pixel * 2) & 0x3fff;
                if (indexed(c, v, palette, palette_size, palette_format)) return -1;
                break;
            case 14: cmpr(block, x % 8, y % 8, c); break;
            default: return -1;
            }
        }
    }
    return 0;
}
