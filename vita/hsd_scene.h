#pragma once
#include "hsd_data.h"

typedef struct {
    uint32_t image_desc, tlut_desc, pixels, palette, palette_entries, palette_format;
    uint32_t width, height, format;
} MvImage;

/* CPU-side, typed representation of the static subset of an HSD scene.
   Coordinates are transformed through the original Joint SRT hierarchy; the
   triangle list is already expanded from the GX display list so renderers do
   not need to interpret GameCube command streams. */
typedef struct {
    float x, y, z, u, v;
    uint32_t rgba; /* 0xRRGGBBAA, independent from any host GPU API. */
} MvSceneVertex;

typedef struct {
    uint32_t joint_desc, dobj_desc, mobj_desc, pobj_desc;
    uint32_t tobj_desc, image_desc, tlut_desc;
    uint32_t rendermode, tobj_flags;
    uint32_t material_rgba; /* 0xRRGGBBAA */
    float texture_blending;
    size_t first_vertex, vertex_count;
    uint8_t texture_count;
} MvSceneMesh;

typedef struct {
    MvSceneVertex *vertices;
    MvSceneMesh *meshes;
    size_t vertex_count, vertex_capacity, mesh_count, mesh_capacity;
    size_t joint_count, dobj_count, pobj_count, primitive_count;
    size_t textured_mesh_count, skipped_pobj_count, skipped_primitive_count;
    size_t unsupported_joint_count;
    size_t anim_joint_count, anim_aobj_count, anim_fobj_count, anim_channel_count;
    size_t unsupported_anim_count;
    float anim_frame;
    int animated;
    float min_x, min_y, min_z, max_x, max_y, max_z;
    int bounds_valid;
} MvScene;

typedef struct {
    uint16_t flags, projection_type;
    int16_t viewport_xmin, viewport_xmax, viewport_ymin, viewport_ymax;
    uint16_t scissor_left, scissor_right, scissor_top, scissor_bottom;
    float eye[3], interest[3], up[3];
    float near_z, far_z;
    float fov_y, aspect;
    float top, bottom, left, right;
} MvCamera;

typedef struct {
    size_t front, partial, behind, far_count;
} MvCameraVisibility;

enum {
    MV_SCENE_STAT_MESHES,
    MV_SCENE_STAT_VERTICES,
    MV_SCENE_STAT_TRIANGLES,
    MV_SCENE_STAT_JOINTS,
    MV_SCENE_STAT_DOBJS,
    MV_SCENE_STAT_POBJS,
    MV_SCENE_STAT_TEXTURED_MESHES,
    MV_SCENE_STAT_SKIPPED_POBJS,
    MV_SCENE_STAT_SKIPPED_PRIMITIVES,
    MV_SCENE_STAT_UNSUPPORTED_JOINTS,
    MV_SCENE_STAT_COUNT
};

/* Collect static material textures from named *_joint roots. Animation tables
   and arbitrary game-specific data table roots require additional typed readers. */
int mv_menu_textures(const MvDat *v, MvImage *images, size_t capacity, size_t *count);
/* Resolve an ImageDesc selected at runtime (for example by TexAnim TIMG) even
   when no static TObj in the archive references it. tlut_desc may be UINT32_MAX. */
int mv_image_from_desc(const MvDat *v, uint32_t image_desc, uint32_t tlut_desc,
                       MvImage *image);
int mv_image_decode(const MvDat *v, const MvImage *image, uint8_t *rgba,
                    size_t capacity, size_t stride);
/* Build the static Euler/rigid subset of a named HSD Joint tree. Unsupported
   runtime-only features are counted and skipped instead of being treated as
   successful stubs. The source archive is never modified. */
int mv_scene_build(const MvDat *v, const char *root_name, MvScene *scene);
/* Apply the same HSD_AnimJoint first-play sample used by Melee immediately
   after HSD_JObjReqAnimAll(frame) + HSD_JObjAnimAll(). This currently covers
   the rigid R/T/S subset; unsupported animation features fail closed. */
int mv_scene_build_animated(const MvDat *v, const char *root_name,
                            const char *anim_root_name, float frame, MvScene *scene);
int mv_scene_build_frame0(const MvDat *v, const char *root_name,
                          const char *anim_root_name, MvScene *scene);
void mv_scene_free(MvScene *scene);
/* Read the static HSD camera descriptor used by a named *_camera root. WObj
   animation/RObj indirections are deliberately rejected until their adapters
   exist; the menu camera in MnMaAll.usd is fully static. */
int mv_camera_read(const MvDat *v, const char *root_name, MvCamera *camera);
int mv_camera_visibility(const MvCamera *camera, const MvScene *scene,
                         MvCameraVisibility *visibility);
/* Stable fixed-width summary used by host and ARM parity tests. */
int mv_scene_stats(const MvDat *v, const char *root_name,
                   uint32_t stats[MV_SCENE_STAT_COUNT]);

enum {
    MV_ANIM_STAT_MESHES,
    MV_ANIM_STAT_TRIANGLES,
    MV_ANIM_STAT_JOINTS,
    MV_ANIM_STAT_ANIM_JOINTS,
    MV_ANIM_STAT_AOBJS,
    MV_ANIM_STAT_FOBJS,
    MV_ANIM_STAT_CHANNELS,
    MV_ANIM_STAT_UNSUPPORTED,
    MV_ANIM_STAT_COUNT
};
int mv_scene_animated_stats(const MvDat *v, const char *root_name,
                            const char *anim_root_name, float frame,
                            uint32_t stats[MV_ANIM_STAT_COUNT]);
int mv_scene_frame0_stats(const MvDat *v, const char *root_name,
                          const char *anim_root_name,
                          uint32_t stats[MV_ANIM_STAT_COUNT]);
