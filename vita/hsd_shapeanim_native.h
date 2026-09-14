#pragma once

#include "hsd_data.h"

#include <stddef.h>
#include <sysdolphin/baselib/pobj.h>

typedef struct {
    HSD_ShapeAnimJoint *root;
    void *storage;
    size_t joint_count;
    size_t dobj_count;
    size_t shapeanim_count;
    size_t aobj_count;
    size_t fobj_count;
} MvNativeShapeAnim;

int mv_native_shapeanim_build_at(const MvDat *dat, uint32_t root_offset,
                                 MvNativeShapeAnim *out);
void mv_native_shapeanim_free(MvNativeShapeAnim *anim);
