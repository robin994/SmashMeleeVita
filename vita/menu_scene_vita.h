#pragma once

#include "hsd_data.h"
#include "hsd_scene.h"
#include <stdio.h>
#include <stdint.h>
#include <vita2d.h>

#define MV_MENU_OVERLAY_TEXTURES 64u

typedef struct {
    uint32_t image_desc;
    uint32_t tlut_desc;
    vita2d_texture *texture;
} MvMenuOverlayTexture;

typedef struct {
    MvScene scene;
    MvMenuOverlayTexture textures[MV_MENU_OVERLAY_TEXTURES];
    size_t texture_count;
    int ready;
} MvMenuSceneOverlay;

int mv_menu_scene_overlay_init(MvMenuSceneOverlay *overlay, const MvDat *dat,
                               const char *joint_root, const char *anim_root,
                               float frame, FILE *log);
void mv_menu_scene_overlay_draw(const MvMenuSceneOverlay *overlay,
                                const MvCamera *camera);
void mv_menu_scene_overlay_close(MvMenuSceneOverlay *overlay);
