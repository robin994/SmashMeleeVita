#pragma once

#include "hsd_data.h"
#include <stddef.h>
#include <sysdolphin/baselib/mobj.h>

typedef struct {
    HSD_MatAnimJoint *root;
    void *storage;
    size_t joint_count;
    size_t matanim_count;
    size_t texanim_count;
    size_t aobj_count;
    size_t fobj_count;
    size_t image_count;
    size_t tlut_count;
} MvNativeMatAnim;

int mv_native_matanim_build_at(const MvDat *dat, uint32_t root_offset,
                               MvNativeMatAnim *out);
int mv_native_matanim_build(const MvDat *dat, const char *name,
                            MvNativeMatAnim *out);
void mv_native_matanim_free(MvNativeMatAnim *anim);
