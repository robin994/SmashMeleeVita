#pragma once

#include "hsd_data.h"

#include <stddef.h>
#include <stdint.h>

#define MV_ITEM_STATE_ROOT_CAP 512

typedef struct MvItemStateRawResult {
    uint32_t anim_roots[MV_ITEM_STATE_ROOT_CAP];
    size_t anim_count;
    uint32_t matanim_roots[MV_ITEM_STATE_ROOT_CAP];
    size_t matanim_count;
    uint32_t shape_roots[MV_ITEM_STATE_ROOT_CAP];
    size_t shape_count;
    uint32_t table_count;
    uint32_t desc_count;
    uint32_t script_root_count;
    uint32_t command_count;
    uint32_t histogram[26];
    uint32_t special_histogram[256];
} MvItemStateRawResult;

/* Nativeize the PPC-bitfield command streams referenced by one or more
 * serialized Article::xC_itemStates tables. Pointer words remain untouched for
 * HSD_ArchiveParse; local Anim/MatAnim/ShapeAnim roots are returned so the
 * owning archive can nativeize those HSD graphs at the same raw boundary. */
int mv_item_state_tables_prepare_raw(void* bytes, size_t size,
                                     const uint32_t* state_tables,
                                     size_t state_table_count,
                                     MvItemStateRawResult* out,
                                     const char* label);
