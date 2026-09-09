#pragma once

#include "gx_capture_vita.h"
#include "hsd_scene.h"

#include <stdio.h>
#include <stdint.h>
#include <vita2d.h>

#define MV_GX_REPLAY_TEXTURE_LIMIT 128u

typedef struct {
    const uint8_t *image;
    const uint8_t *palette;
    uint32_t material_rgba;
    uint32_t tobj_flags;
    float blending;
    uint16_t width, height, palette_entries;
    uint16_t storage_width, storage_height;
    uint8_t format, palette_format;
    uint8_t wrap_s, wrap_t;
    uint8_t tev_valid;
    uint8_t reserved0[3];
    uint8_t tev_op[16];
    uint8_t tev_konst[4];
    uint8_t tev0[4];
    uint8_t tev1[4];
    uint32_t tev_active;
    float uv_scale_s, uv_scale_t;
    vita2d_texture *texture;
} MvGxReplayTexture;

typedef struct {
    MvGxReplayTexture textures[MV_GX_REPLAY_TEXTURE_LIMIT];
    uint32_t texture_count;
    uint32_t texture_bytes;
    uint32_t texture_failures;
    uint32_t submitted_commands;
    uint32_t input_triangles;
    uint32_t output_triangles;
    uint32_t skipped_matrix_index;
    uint32_t uv_matrix_commands;
    uint32_t wrap_repeat_commands;
    uint32_t wrap_mirror_commands;
    uint32_t wrap_clamp_commands;
    uint32_t mirror_baked_textures;
    uint32_t custom_tev_baked_commands;
    uint32_t custom_tev_noop_commands;
    uint32_t pe_alpha_commands;
    uint32_t pe_additive_commands;
    uint32_t pe_culled_triangles;
    uint32_t pe_cull_none_commands;
    uint32_t pe_cull_back_commands;
    uint32_t multitex_command;
    uint32_t multitex_texture0;
    uint32_t multitex_texture1;
    uint32_t multitex_input_triangles;
    uint32_t multitex_output_triangles;
    uint32_t multitex_target_bytes;
    SceGxmShaderPatcherId multitex_program_id;
    SceGxmFragmentProgram *multitex_replace_program;
    SceGxmFragmentProgram *multitex_multiply_program;
    SceGxmFragmentProgram *pe_alpha_program;
    SceGxmFragmentProgram *pe_additive_program;
    vita2d_texture *multitex_target;
    int multitex_program_registered;
    int multitex_ready;
    int pe_ready;
    int ready;
    int submit_logged;
    FILE *log;
} MvGxReplay;

int mv_gx_replay_init(MvGxReplay *replay, const MvCamera *camera, FILE *log);
void mv_gx_replay_draw(MvGxReplay *replay, const MvCamera *camera);
void mv_gx_replay_close(MvGxReplay *replay);
