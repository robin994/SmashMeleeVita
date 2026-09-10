#pragma once
#include "hsd_data.h"
#include <sysdolphin/baselib/aobj.h>
typedef struct {
    HSD_AnimJoint *root;
    void *entries;
    unsigned count;
} MvNativeAnim;
int mv_native_anim_build_at(const MvDat *dat, uint32_t root_offset, MvNativeAnim *out);
int mv_native_anim_build(const MvDat *dat, const char *name, MvNativeAnim *out);
void mv_native_anim_free(MvNativeAnim *anim);
