#include "thp_jpeg.h"
#include <string.h>

int mv_thp_jpeg_unpack(const uint8_t *src, size_t size, uint8_t *dst,
                       size_t capacity, size_t *written)
{
    if (written) *written = 0;
    if (!src || !dst || !written || size < 6 || src[0] != 0xff || src[1] != 0xd8)
        return -1;
    size_t scan = 2;
    int baseline = 0;
    for (;;) {
        if (scan > size - 4 || src[scan] != 0xff) return -2;
        unsigned marker = src[scan+1];
        if (marker != 0xdb && marker != 0xc4 && marker != 0xc0 && marker != 0xda &&
            marker != 0xfe && !(marker >= 0xe0 && marker <= 0xef)) return -2;
        size_t length = ((size_t)src[scan+2] << 8) | src[scan+3];
        if (length < 2 || length > size - scan - 2) return -2;
        if (marker == 0xc0) baseline = 1;
        scan += length + 2;
        if (marker == 0xda) break;
    }
    if (!baseline) return -2;
    /* MTH frames are padded to 32 bytes. The true EOI is the last marker,
     * not arbitrary FF D9 bytes in Nintendo's unstuffed entropy stream. */
    size_t end = size;
    while (end >= scan + 2 && !(src[end-2] == 0xff && src[end-1] == 0xd9)) {
        if (size - end >= 32 || (src[end-1] != 0 && src[end-1] != 0xff)) return -3;
        --end;
    }
    if (end < scan + 2) return -3;
    size_t needed = end;
    for (size_t i=scan; i<end-2; ++i) {
        if (src[i] == 0xff) {
            if (needed == (size_t)-1) return -4;
            ++needed;
        }
    }
    if (needed > capacity) return -4;
    memcpy(dst,src,scan);
    size_t out = scan;
    for (size_t i=scan; i<end-2; ++i) {
        dst[out++] = src[i];
        if (src[i] == 0xff) dst[out++] = 0;
    }
    dst[out++] = 0xff; dst[out++] = 0xd9;
    *written = out;
    return 0;
}
