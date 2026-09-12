#pragma once
#include <stddef.h>
#include <stdint.h>

/* Immutable big-endian view. Never relocate original bytes into host pointers. */
typedef struct {
    const uint8_t *file, *data, *relocations, *publics, *externs, *strings;
    size_t file_size, strings_size;
    uint32_t data_size, relocation_count, public_count, extern_count;
    uint8_t *pointer_bits;
    /* Fields participating in an HSD extern linked list.  Their serialized
       word is the next field offset (or 0xFFFFFFFF), not an in-file target. */
    uint8_t *external_bits;
} MvDat;

uint16_t mv_be16(const void *p);
uint32_t mv_be32(const void *p);
int mv_dat_open(MvDat *view, const void *bytes, size_t size);
void mv_dat_close(MvDat *view);
const uint8_t *mv_dat_span(const MvDat *view, uint32_t offset, size_t size);
/* 1: relocated reference (including offset zero); 0: null; -1: invalid/unresolved. */
int mv_dat_pointer(const MvDat *view, uint32_t field, uint32_t *target);
int mv_dat_external(const MvDat *view, uint32_t field);
int mv_dat_public(const MvDat *view, uint32_t index, const char **name, uint32_t *target);
