#pragma once

#include "item_state_native.h"

#include <stddef.h>
#include <stdint.h>

#define MV_STAGE_ITEM_JOINT_ROOT_CAP 128

typedef struct MvStageItemRawResult {
    uint32_t joint_roots[MV_STAGE_ITEM_JOINT_ROOT_CAP];
    size_t joint_count;
    MvItemStateRawResult states;
    uint32_t item_count;
    uint32_t article_count;
    uint32_t attr_count;
    uint32_t hurt_count;
    uint32_t model_count;
    uint32_t dynamics_count;
    uint32_t special_count;
} MvStageItemRawResult;

/* Convert the non-HSD metadata owned by a stage's serialized itemdata table
 * and return every HSD model/animation root.  The caller deliberately walks
 * those roots with its own shared HSD visited set, because stage-item models
 * are allowed to alias ordinary map graphs inside the same DAT. */
int mv_stage_itemdata_prepare_raw(void* bytes, size_t size,
                                  uint32_t itemdata_root,
                                  const char* filename,
                                  MvStageItemRawResult* out);
