#pragma once

#include "hsd_data.h"
#include <stddef.h>
#include <stdint.h>

/* One sampled HSD_AnimJoint node. Channels use the upstream JObj animation
   numbers (1..10 = rotate XYZ, path, translate XYZ, scale XYZ). */
typedef struct {
    uint32_t child, next, flags;
    uint16_t channel_mask;
    uint8_t has_child, has_next;
    float channels[10];
    size_t aobj_count, fobj_count, applied_channel_count;
} MvAnimJointSample;

/* Generic AObj sample used by material/texture animation adapters. Channel
   numbers are the original upstream object update types. */
typedef struct {
    uint32_t flags, obj_id, channel_mask;
    float end_frame;
    float channels[32];
    size_t fobj_count, applied_channel_count;
} MvAObjSample;

/* Sample the exact first HSD_AObjInterpretAnim pass that follows
   HSD_JObjReqAnimAll(frame). The source archive remains immutable.
   Returns 0 on success, 1 for an explicitly unsupported animation feature,
   and -1 for malformed data or bounds failure. */
int mv_anim_joint_sample(const MvDat *dat, uint32_t anim_joint, float frame,
                         MvAnimJointSample *sample);
int mv_anim_joint_sample_frame0(const MvDat *dat, uint32_t anim_joint,
                                MvAnimJointSample *sample);
int mv_aobj_sample(const MvDat *dat, uint32_t aobj_desc, float frame,
                   MvAObjSample *sample);
