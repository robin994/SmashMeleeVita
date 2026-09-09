#include "hsd_data.h"
#include <stdlib.h>
#include <string.h>

uint16_t mv_be16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)((unsigned)b[0] << 8 | b[1]);
}
uint32_t mv_be32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
           (uint32_t)b[2] << 8 | b[3];
}
const uint8_t *mv_dat_span(const MvDat *v, uint32_t offset, size_t size)
{
    if (offset > v->data_size || size > v->data_size - offset) return NULL;
    return v->data + offset;
}
void mv_dat_close(MvDat *v)
{
    free(v->pointer_bits);
    memset(v, 0, sizeof(*v));
}
static int valid_symbol(const MvDat *v, const uint8_t *entry)
{
    uint32_t name = mv_be32(entry + 4);
    return mv_be32(entry) <= v->data_size && name < v->strings_size &&
           memchr(v->strings + name, 0, v->strings_size - name) != NULL;
}
int mv_dat_open(MvDat *v, const void *bytes, size_t size)
{
    memset(v, 0, sizeof(*v));
    if (!bytes || size < 32 || size > 64u * 1024u * 1024u) return -1;
    const uint8_t *b = bytes;
    if (mv_be32(b) != size) return -1;
    uint32_t ds = mv_be32(b + 4), nr = mv_be32(b + 8);
    uint32_t np = mv_be32(b + 12), ne = mv_be32(b + 16);
    uint64_t end = 32ull + ds + (uint64_t)nr * 4 + ((uint64_t)np + ne) * 8;
    if (end > size) return -1;
    v->file = b; v->file_size = size; v->data = b + 32; v->data_size = ds;
    v->relocation_count = nr; v->public_count = np; v->extern_count = ne;
    v->relocations = b + 32 + ds;
    v->publics = v->relocations + (size_t)nr * 4;
    v->externs = v->publics + (size_t)np * 8;
    v->strings = b + (size_t)end; v->strings_size = size - (size_t)end;
    /* Some original archives contain unaligned pointer fields: one bit per byte. */
    v->pointer_bits = calloc(((size_t)ds + 7) / 8 + 1, 1);
    if (!v->pointer_bits) { mv_dat_close(v); return -1; }
    for (uint32_t i = 0; i < nr; ++i) {
        uint32_t field = mv_be32(v->relocations + i * 4);
        const uint8_t *p = mv_dat_span(v, field, 4);
        if (!p || mv_be32(p) > ds) goto invalid;
        v->pointer_bits[field / 8] |= (uint8_t)(1u << (field % 8));
    }
    for (uint32_t i = 0; i < np; ++i)
        if (!valid_symbol(v, v->publics + i * 8)) goto invalid;
    for (uint32_t i = 0; i < ne; ++i)
        if (!valid_symbol(v, v->externs + i * 8)) goto invalid;
    return 0;
invalid:
    mv_dat_close(v);
    return -1;
}
int mv_dat_pointer(const MvDat *v, uint32_t field, uint32_t *target)
{
    const uint8_t *p = mv_dat_span(v, field, 4);
    if (!p || !v->pointer_bits || !target) return -1;
    uint32_t value = mv_be32(p);
    if (!(v->pointer_bits[field / 8] & (1u << (field % 8))))
        return value == 0 ? 0 : -1;
    if (value > v->data_size) return -1;
    *target = value;
    return 1;
}
int mv_dat_public(const MvDat *v, uint32_t index, const char **name, uint32_t *target)
{
    if (index >= v->public_count || !name || !target) return -1;
    const uint8_t *entry = v->publics + index * 8;
    *target = mv_be32(entry);
    *name = (const char *)v->strings + mv_be32(entry + 4);
    return 0;
}
