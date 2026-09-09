#pragma once

#include "hsd_scene.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t joint_count;
    size_t matanim_count;
    size_t texanim_count;
    size_t aobj_count;
    size_t fobj_count;
    size_t channel_count;
    size_t image_channel_count;
    size_t uv_channel_count;
    size_t blend_channel_count;
    size_t transformed_mesh_count;
    size_t multitexture_mesh_count;
    size_t unsupported_count;
} MvMatAnimStats;

/* Apply the material/texture state produced by the first animation pass after
   HSD_JObjReqAnimAll(frame). The Joint and MatAnimJoint trees are paired with
   the same traversal semantics as HSD_JObjAddAnimAll. The current renderer
   supports the first UV TObj of each MObj; unsupported TexGen/TEV animation
   fails closed instead of being reported as applied. */
int mv_scene_apply_matanim(const MvDat *dat, const char *joint_root_name,
                           const char *matanim_root_name, float frame,
                           MvScene *scene, MvMatAnimStats *stats);

int mv_scene_apply_matanim_frame0(const MvDat *dat, const char *joint_root_name,
                                  const char *matanim_root_name, MvScene *scene,
                                  MvMatAnimStats *stats);

enum {
    MV_MATANIM_STAT_JOINTS,
    MV_MATANIM_STAT_MATANIMS,
    MV_MATANIM_STAT_TEXANIMS,
    MV_MATANIM_STAT_AOBJS,
    MV_MATANIM_STAT_FOBJS,
    MV_MATANIM_STAT_CHANNELS,
    MV_MATANIM_STAT_IMAGES,
    MV_MATANIM_STAT_UV_CHANNELS,
    MV_MATANIM_STAT_BLEND_CHANNELS,
    MV_MATANIM_STAT_TRANSFORMED_MESHES,
    MV_MATANIM_STAT_MULTITEXTURE_MESHES,
    MV_MATANIM_STAT_UNSUPPORTED,
    MV_MATANIM_STAT_COUNT
};

int mv_matanim_frame0_stats(const MvDat *dat, const char *joint_root_name,
                            const char *anim_root_name,
                            uint32_t out[MV_MATANIM_STAT_COUNT]);
