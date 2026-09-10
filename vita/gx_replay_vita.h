#pragma once

#include "gx_capture_vita.h"
#include "hsd_scene.h"

#include <stdio.h>
#include <stdint.h>
#include <vitaGL.h>

#define MV_GX_REPLAY_TEXTURE_LIMIT 512u

typedef struct {
    MvGxMaterialState key;
    GLuint id;
    unsigned bytes;
    uint32_t last_use;
} MvGlTexture;
typedef struct {
    MvGlTexture *textures;
    unsigned texture_count, texture_capacity, texture_bytes, texture_failures;
    unsigned relaxed_from_command, submitted_commands, skipped_commands;
    uint32_t use_serial;
    int ready, submit_logged;
    FILE *log;
} MvGxReplay;

int mv_gx_replay_init(MvGxReplay *replay, const MvCamera *camera, FILE *log);
int mv_gx_replay_init_relaxed_from(MvGxReplay *replay, const MvCamera *camera, FILE *log,
                                   uint32_t relaxed_from_command);
void mv_gx_replay_draw(MvGxReplay *replay, const MvCamera *camera);
void mv_gx_replay_close(MvGxReplay *replay);
