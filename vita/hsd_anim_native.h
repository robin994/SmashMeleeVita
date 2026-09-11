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
/* Validation-only variant used before in-place archive relocation.  Unlike the
   native proxy builder it accepts AObj.obj_id references because the original
   archive parser will relocate those references after scalar conversion. */
int mv_native_anim_validate_at(const MvDat *dat, uint32_t root_offset);
void mv_native_anim_free(MvNativeAnim *anim);
