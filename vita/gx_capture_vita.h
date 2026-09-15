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
    /* GX texture-coordinate routing.  Gameplay materials can bind TEX0 and
     * TEX1 simultaneously; preserving only the texture images is insufficient
     * because each texture unit may consume a different vertex attribute. */
    uint8_t texgen_src0;
    uint8_t texgen_src1;
    uint8_t texgen_valid_mask;
    uint8_t tev_stage_count;
    /* First four generated TEV stages are enough for the exact common HSD
     * two-layer MODULATE graph used by GrCs.  Capture the actual GX program
     * rather than inferring gameplay TEV state from menu-specific TObj flags. */
    uint8_t tev_order_coord[4];
    uint8_t tev_order_map[4];
    uint8_t tev_order_color[4];
    uint8_t tev_color_in[4][4];
    uint8_t tev_alpha_in[4][4];
    uint8_t tev_color_op[4][5];
    uint8_t tev_alpha_op[4][5];
    /* Direct GX TEV subset used by SIS and a few UI primitives. */
    uint8_t simple_tev_color_c0;
    uint8_t simple_tev_alpha_texa_a0;
    uint8_t reserved0;
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
    /* Direct GX konst state used by generated TEV stages. HSD frequently
     * emits RASC/TEXC/KONST blends; preserving only the color inputs without
     * the K selector loses the interpolation coefficient. */
    uint8_t tev_kcolor_sel[4];
    uint8_t tev_kalpha_sel[4];
    uint32_t tev_kcolor_regs[4];
} MvGxMaterialState;

typedef struct {
    float position[3];
    float normal[9];
    float tex0[2];
    float tex1[2];
    /* Raw GX vertex colors followed by the post-XF raster colors consumed by
     * TEV as RASC/RASA. HSD uses COLOR1/A1 for the specular channel. */
    uint32_t color0;
    uint32_t color1;
    uint32_t raster0;
    uint32_t raster1;
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
    uint8_t pos_mtx_valid;
    uint8_t reserved;
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
    uint32_t channel_eval_vertices;
    uint32_t channel_lit_vertices;
    uint32_t channel_normal_vertices;
    uint32_t errors;
    /* First capture error in the current frame. The line number and compact
     * arguments turn opaque capture_result=-2 failures into actionable Vita
     * telemetry without spamming every rejected GX operation. */
    uint32_t first_error_line;
    uint32_t first_error_arg0;
    uint32_t first_error_arg1;
    uint32_t first_error_arg2;
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
/* Clear the captured FIFO while preserving emulated GX register state. */
void mv_gx_capture_begin_frame(void);
/* Reset only the emulated material/PE/TEV state. Fully-programmed immediate
 * renderers such as SIS use this to avoid inheriting unsupported flags from
 * the previously drawn HSD material while preserving camera/matrix state. */
void mv_gx_capture_reset_material_state(void);
void mv_gx_capture_set_projection(const float matrix[4][4], uint32_t type);
void mv_gx_capture_set_viewport(float x, float y, float w, float h,
                                float near_z, float far_z);
int mv_gx_capture_stats(MvGxCaptureStats *out);
void mv_gx_capture_set_material(const MvGxMaterialState *material);
/* Internal Vita GX compatibility layer: mutate the state snapshotted by the next draw. */
MvGxMaterialState *mv_gx_capture_material_state(void);
const MvGxCaptureCommand *mv_gx_capture_commands(uint32_t *count);
const MvGxCaptureVertex *mv_gx_capture_vertices(uint32_t *count);
/* Per-frame diagnostic fingerprint of captured commands and vertices. It is
 * intended for comparisons within one process, so changing animation state,
 * matrices, UVs, colors or texture references changes the value. */
uint32_t mv_gx_capture_frame_signature(void);
int mv_gx_capture_replay_classify(MvGxReplayClassStats *out);
int mv_gx_capture_pe_stats(MvGxPeStats *out);
int mv_gx_material_custom_tev_cpu_bakeable(const MvGxMaterialState *material);
/* GX alpha-test forms implemented exactly by the fixed-function vitaGL path. */
int mv_gx_alpha_compare_vitagl_supported(uint8_t comp0, uint8_t ref0,
                                         uint8_t op, uint8_t comp1,
                                         uint8_t ref1);
/* Exact frame-0 MenMainBack two-TObj graph accepted by the offscreen replay:
 * stage0 MODULATE RGB/A, stage1 MODULATE RGB with alpha preserved. */
int mv_gx_material_multitex_offscreen_bakeable(const MvGxMaterialState *material);
/* Two-layer HSD MODULATE subset that vitaGL can sample directly with
 * independent UV matrices.  This is broader than the legacy offscreen baker. */
int mv_gx_material_multitex_vitagl_supported(const MvGxMaterialState *material);
/* Exact generated GX graph used by common HSD dual-texture materials:
 * primary raster color * TEX0 * TEX1, with primary alpha preserved. */
int mv_gx_material_multitex_hsd_modulate(const MvGxMaterialState *material);
/* Exact two-stage Icicle Mountain graph. Return 1 when final alpha is RASA,
 * 2 when TEX0 alpha also modulates RASA, and 0 for any other graph. */
int mv_gx_material_multitex_hsd_alpha_blend(const MvGxMaterialState *material);
/* Exact Corneria diffuse+specular graph: stage 1 is RASC0*TEX0, stage 2 adds
 * RASC1*TEX1, and alpha preserves the COLOR0/A0 material/raster product. */
int mv_gx_material_multitex_hsd_specular_add(const MvGxMaterialState *material);
/* True only when the captured TEV program actually consumes COLOR0/A0 as
 * RASC/RASA. GXSetNumChans(1) alone does not mean every draw should be
 * modulated by the generated raster channel. */
int mv_gx_material_uses_raster0(const MvGxMaterialState *material);
int mv_gx_material_uses_raster1(const MvGxMaterialState *material);
/* Exact one-texture HSD graph used heavily by Yoshi's Island. Return 1 for
 * RGB=RASC*TEX0, A=RASA*TEXA; 2 for the same RGB with A=RASA. */
int mv_gx_material_single_tev_rasc_tex(const MvGxMaterialState *material);
/* KONST interpolation mode: 1 preserves RASA, 2 interpolates RASA/TEXA
 * with KAlpha just like RGB interpolates RASC/TEXC with KColor. */
int mv_gx_material_single_tev_rasc_tex_konst(const MvGxMaterialState *material);
uint32_t mv_gx_material_kcolor_rgba(const MvGxMaterialState *material, unsigned stage);
uint8_t mv_gx_material_kalpha_u8(const MvGxMaterialState *material, unsigned stage);
/* Resolve a texture/post-texture matrix previously loaded through
 * GXLoadTexMtxImm into the 2D affine form used by the replay. */
int mv_gx_capture_get_tex_mtx(uint32_t id, float out[2][3]);
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
