#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    MV_GX_MATERIAL_UNSUPPORTED_MULTITEX = 1u << 0,
    MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV = 1u << 1,
    MV_GX_MATERIAL_UNSUPPORTED_TEXCOORD = 1u << 2,
    MV_GX_MATERIAL_UNSUPPORTED_BUMP = 1u << 3,
    MV_GX_MATERIAL_UNSUPPORTED_COLORMAP = 1u << 4,
    MV_GX_MATERIAL_UNSUPPORTED_ALPHAMAP = 1u << 5,
};

typedef struct {
    const uint8_t *image;
    const uint8_t *palette;
    uint32_t material_rgba;
    uint32_t rendermode;
    /* Effective pixel-engine state after HSD_SetupPEMode.  This is captured
     * from the runtime MObj rather than inferred later in the Vita renderer,
     * so custom HSD_PEDesc and default rendermode semantics share one path. */
    uint8_t pe_color_update;
    uint8_t pe_alpha_update;
    uint8_t pe_dst_alpha_enable;
    uint8_t pe_dst_alpha;
    uint8_t pe_blend_type;
    uint8_t pe_src_factor;
    uint8_t pe_dst_factor;
    uint8_t pe_logic_op;
    uint8_t pe_z_enable;
    uint8_t pe_z_func;
    uint8_t pe_z_update;
    uint8_t pe_z_comp_loc;
    uint8_t pe_alpha_comp0;
    uint8_t pe_alpha_ref0;
    uint8_t pe_alpha_op;
    uint8_t pe_alpha_comp1;
    uint8_t pe_alpha_ref1;
    uint8_t pe_dither;
    uint8_t pe_custom;
    uint8_t pe_reserved;
    uint32_t tobj_flags;
    uint32_t unsupported;
    float blending;
    uint16_t width;
    uint16_t height;
    uint16_t palette_entries;
    uint8_t format;
    uint8_t palette_format;
    uint8_t wrap_s;
    uint8_t wrap_t;
    uint8_t mag_filter;
    uint8_t texture_count;
    uint8_t uv_mtx_valid;
    uint8_t tev_valid;
    uint8_t reserved0[2];
    /* First two rows of the HSD UV texture matrix, collapsed to
     * u'=a*u+b*v+c / v'=d*u+e*v+f for replay. */
    float uv_mtx[2][3];
    /* Raw HSD_TObjTev payload. Keeping the selectors/op state in the capture
     * queue lets the replay decide whether a custom stage is exactly
     * precomputable or needs a real TEV shader. */
    uint8_t tev_op[16];
    uint8_t tev_konst[4];
    uint8_t tev0[4];
    uint8_t tev1[4];
    uint32_t tev_active;
    /* Second TObj snapshot for the single remaining MenMainBack multitexture
     * material. Kept separate from the first-layer ABI so the existing
     * single-texture replay path stays unchanged while we characterize and
     * implement the exact two-stage composition. */
    const uint8_t *image1;
    const uint8_t *palette1;
    uint32_t tobj1_flags;
    float blending1;
    uint16_t width1;
    uint16_t height1;
    uint16_t palette_entries1;
    uint8_t format1;
    uint8_t palette_format1;
    uint8_t wrap_s1;
    uint8_t wrap_t1;
    uint8_t mag_filter1;
    uint8_t uv_mtx1_valid;
    uint8_t tev1_valid;
    uint8_t reserved1;
    float uv_mtx1[2][3];
    uint8_t tev1_op[16];
    uint8_t tev1_konst[4];
    uint8_t tev1_reg0[4];
    uint8_t tev1_reg1[4];
    uint32_t tev1_active;
} MvGxMaterialState;

typedef struct {
    float position[3];
    float normal[9];
    float tex0[2];
    uint32_t color0;
    uint32_t present;
    uint8_t pos_mtx_idx;
    uint8_t reserved[3];
} MvGxCaptureVertex;

typedef struct {
    uint32_t first_vertex;
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t attr_mask;
    uint32_t current_mtx;
    uint32_t cull_mode;
    uint8_t primitive;
    uint8_t vtxfmt;
    uint16_t reserved;
    float pos_mtx[3][4];
    /* GX camera state active when this draw was emitted.  Retail Melee can
     * switch between world/HUD cameras inside one HSD traversal, so replay
     * must keep projection/viewport per command rather than once per frame. */
    float projection[4][4];
    float viewport[6];
    uint32_t projection_type;
    uint8_t projection_valid;
    uint8_t viewport_valid;
    uint8_t camera_reserved[2];
    MvGxMaterialState material;
} MvGxCaptureCommand;

typedef struct {
    uint32_t display_lists;
    uint32_t commands;
    uint32_t vertices;
    uint32_t triangles;
    uint32_t line_point_commands;
    uint32_t array_binds;
    uint32_t attr_formats;
    uint32_t pos_mtx_loads;
    uint32_t nrm_mtx_loads;
    uint32_t tex_mtx_loads;
    uint32_t cull_changes;
    uint32_t errors;
} MvGxCaptureStats;

typedef struct {
    uint32_t supported_commands;
    uint32_t supported_triangles;
    uint32_t textured_commands;
    uint32_t untextured_commands;
    uint32_t skipped_multitex;
    uint32_t skipped_custom_tev;
    uint32_t skipped_texcoord;
    uint32_t skipped_mapmode;
    uint32_t skipped_vertex_color;
} MvGxReplayClassStats;

typedef struct {
    uint32_t commands;
    uint32_t blend_alpha;
    uint32_t blend_additive;
    uint32_t blend_other;
    uint32_t z_lequal;
    uint32_t z_write;
    uint32_t alpha_compare_always;
    uint32_t custom_pe;
    uint32_t cull_none;
    uint32_t cull_front;
    uint32_t cull_back;
    uint32_t cull_all;
} MvGxPeStats;

/* First-stage GX translator inspired by the command-buffer architecture used
 * by ACGC-Vita-Port.  It records the authentic HSD GX display stream into a
 * bounded native queue but does not submit it to VitaGL/GXM yet. */
void mv_gx_capture_reset(void);
void mv_gx_capture_set_projection(const float matrix[4][4], uint32_t type);
void mv_gx_capture_set_viewport(float x, float y, float w, float h,
                                float near_z, float far_z);
int mv_gx_capture_stats(MvGxCaptureStats *out);
void mv_gx_capture_set_material(const MvGxMaterialState *material);
/* Internal Vita GX compatibility layer: mutate the state snapshotted by the next draw. */
MvGxMaterialState *mv_gx_capture_material_state(void);
const MvGxCaptureCommand *mv_gx_capture_commands(uint32_t *count);
const MvGxCaptureVertex *mv_gx_capture_vertices(uint32_t *count);
int mv_gx_capture_replay_classify(MvGxReplayClassStats *out);
int mv_gx_capture_pe_stats(MvGxPeStats *out);
int mv_gx_material_custom_tev_cpu_bakeable(const MvGxMaterialState *material);
/* Exact frame-0 MenMainBack two-TObj graph accepted by the offscreen replay:
 * stage0 MODULATE RGB/A, stage1 MODULATE RGB with alpha preserved. */
int mv_gx_material_multitex_offscreen_bakeable(const MvGxMaterialState *material);
/* Two-layer HSD MODULATE subset that vitaGL can sample directly with
 * independent UV matrices.  This is broader than the legacy offscreen baker. */
int mv_gx_material_multitex_vitagl_supported(const MvGxMaterialState *material);
/* Writes up to max_records custom-TEV records. Each record is 9 u32 words:
 * command index followed by the exact 32-byte HSD_TObjTev payload. */
uint32_t mv_gx_capture_custom_tev_records(uint32_t *out, uint32_t max_records);
/* Writes up to max_records two-texture material records. Each record is 28
 * u32 words: command index/count/rendermode followed by first/second TObj
 * flags, image metadata, blending, six UV matrix floats and TEV active bits. */
uint32_t mv_gx_capture_multitex_records(uint32_t *out, uint32_t max_records);
/* Bounds of every captured GX vertex after the per-command rigid HSD model
 * matrix has been applied. out = minx,miny,minz,maxx,maxy,maxz. */
int mv_gx_capture_world_bounds(float out[6]);

