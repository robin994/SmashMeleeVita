#pragma once
#include "hsd_matanim.h"
#include "hsd_native.h"
#include "hsd_runtime_probe.h"
#include "hsd_scene.h"
#include "gx_replay_vita.h"
#include <stdio.h>
#include <vita2d.h>
#define MV_PAGE_SIZE 12
#define MV_SCENE_TEXTURE_LIMIT 128

typedef struct {
    uint32_t image_desc, tlut_desc;
    vita2d_texture *texture;
} MvGpuImage;

typedef struct {
    unsigned char *bytes;
    MvDat dat;
    MvImage *images;
    size_t count, page;
    vita2d_texture *textures[MV_PAGE_SIZE];
    MvScene scene;
    MvMatAnimStats matanim_stats;
    MvCamera camera;
    MvNativeHsd native;
    MvHsdRuntimeStats runtime_stats;
    MvGxCaptureStats gx_capture_stats;
    MvGxReplayClassStats gx_replay_stats;
    MvGxReplay gx_replay;
    MvGpuImage scene_textures[MV_SCENE_TEXTURE_LIMIT];
    size_t scene_texture_count, scene_texture_bytes;
    int scene_ready, camera_ready, scene_mode, native_ready, runtime_ready, gx_capture_ready;
    int gx_replay_class_ready, gx_replay_mode;
    FILE *log;
    const char *error;
} MvViewer;
int mv_viewer_init(MvViewer *viewer, FILE *log);
void mv_viewer_page(MvViewer *viewer, int direction);
void mv_viewer_toggle_scene(MvViewer *viewer);
void mv_viewer_draw(MvViewer *viewer, vita2d_pgf *font);
void mv_viewer_close(MvViewer *viewer);
