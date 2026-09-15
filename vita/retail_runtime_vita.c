#include "retail_runtime_vita.h"

#include "gx_capture_vita.h"
#include "gx_replay_vita.h"
#include "render_vita.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/video.h>

typedef struct {
    MvGxReplay replay;
    FILE *log;
    uint32_t heap_generation;
    uint32_t clip_probe_generation;
    unsigned frame;
    int active;
    int capture_started;
} MvRetailRuntime;

static MvRetailRuntime retail;

static void mv_retail_log_tail_commands(unsigned frame)
{
    if (!retail.log || frame > 2u) return;

    uint32_t command_count = 0;
    const MvGxCaptureCommand *commands = mv_gx_capture_commands(&command_count);
    uint32_t first = command_count > 8u ? command_count - 8u : 0u;
    for (uint32_t i = first; i < command_count; ++i) {
        const MvGxCaptureCommand *c = &commands[i];
        const MvGxMaterialState *m = &c->material;
        fprintf(retail.log,
                "VITA_RETAIL_TAIL frame=%u command=%u primitive=%u triangles=%u attr_mask=%08x cull=%u textures=%u unsupported=%08x material=%08x blend=%u:%u,%u z=%u:%u:%u color_alpha=%u,%u alpha_cmp=%u,%u,%u,%u,%u viewport=%u:%g,%g,%g,%g,%g,%g\n",
                frame, i, c->primitive, c->triangle_count, c->attr_mask,
                c->cull_mode, m->texture_count, m->unsupported, m->material_rgba,
                m->pe_blend_type, m->pe_src_factor, m->pe_dst_factor,
                m->pe_z_enable, m->pe_z_func, m->pe_z_update,
                m->pe_color_update, m->pe_alpha_update,
                m->pe_alpha_comp0, m->pe_alpha_ref0, m->pe_alpha_op,
                m->pe_alpha_comp1, m->pe_alpha_ref1,
                c->viewport_valid, c->viewport[0], c->viewport[1],
                c->viewport[2], c->viewport[3], c->viewport[4], c->viewport[5]);
    }
    fflush(retail.log);
}

static void mv_retail_log_clip_probe(void)
{
    if (!retail.log) return;
    uint32_t command_count = 0, vertex_count = 0;
    const MvGxCaptureCommand *commands = mv_gx_capture_commands(&command_count);
    const MvGxCaptureVertex *vertices = mv_gx_capture_vertices(&vertex_count);
    uint32_t projected_commands = 0, tested = 0, finite = 0, positive_w = 0;
    uint32_t inside_xy = 0, inside_xyz = 0;
    uint32_t first_finite_cmd = UINT32_MAX, first_inside_cmd = UINT32_MAX;
    uint32_t first_bad_cmd = UINT32_MAX, first_bad_vertex = UINT32_MAX;
    float first_bad_p[4] = {0}, first_bad_clip[4] = {0};
    float min_x = 0, max_x = 0, min_y = 0, max_y = 0, min_z = 0, max_z = 0;
    int have_ndc = 0;
    for (uint32_t ci = 0; ci < command_count; ++ci) {
        const MvGxCaptureCommand *c = &commands[ci];
        if (!c->projection_valid || !c->triangle_count || c->first_vertex > vertex_count || c->vertex_count > vertex_count - c->first_vertex) continue;
        ++projected_commands;
        for (uint32_t vi = 0; vi < c->vertex_count; ++vi) {
            const MvGxCaptureVertex *v = &vertices[c->first_vertex + vi];
            float p[4];
            p[0] = c->pos_mtx[0][0] * v->position[0] + c->pos_mtx[0][1] * v->position[1] + c->pos_mtx[0][2] * v->position[2] + c->pos_mtx[0][3];
            p[1] = c->pos_mtx[1][0] * v->position[0] + c->pos_mtx[1][1] * v->position[1] + c->pos_mtx[1][2] * v->position[2] + c->pos_mtx[1][3];
            p[2] = c->pos_mtx[2][0] * v->position[0] + c->pos_mtx[2][1] * v->position[1] + c->pos_mtx[2][2] * v->position[2] + c->pos_mtx[2][3];
            p[3] = 1.0f;
            float clip_x = 0, clip_y = 0, clip_z_gx = 0, clip_w = 0;
            for (int k = 0; k < 4; ++k) { clip_x += c->projection[0][k] * p[k]; clip_y += c->projection[1][k] * p[k]; clip_z_gx += c->projection[2][k] * p[k]; clip_w += c->projection[3][k] * p[k]; }
            ++tested;
            float clip_z = 2.0f * clip_z_gx + clip_w;
            if (!isfinite(clip_x) || !isfinite(clip_y) || !isfinite(clip_z) || !isfinite(clip_w) || fabsf(clip_w) < 1.0e-8f) {
                if (first_bad_cmd == UINT32_MAX) {
                    first_bad_cmd = ci; first_bad_vertex = vi;
                    memcpy(first_bad_p, p, sizeof(first_bad_p));
                    first_bad_clip[0] = clip_x; first_bad_clip[1] = clip_y;
                    first_bad_clip[2] = clip_z; first_bad_clip[3] = clip_w;
                }
                continue;
            }
            ++finite;
            if (first_finite_cmd == UINT32_MAX) first_finite_cmd = ci;
            float nx = clip_x / clip_w, ny = clip_y / clip_w, nz = clip_z / clip_w;
            if (!have_ndc) { min_x = max_x = nx; min_y = max_y = ny; min_z = max_z = nz; have_ndc = 1; }
            else { if (nx < min_x) min_x = nx; if (nx > max_x) max_x = nx; if (ny < min_y) min_y = ny; if (ny > max_y) max_y = ny; if (nz < min_z) min_z = nz; if (nz > max_z) max_z = nz; }
            if (clip_w <= 0.0f) continue;
            ++positive_w;
            if (fabsf(clip_x) <= clip_w && fabsf(clip_y) <= clip_w) { ++inside_xy; if (first_inside_cmd == UINT32_MAX) first_inside_cmd = ci; if (fabsf(clip_z) <= clip_w) ++inside_xyz; }
        }
    }
    fprintf(retail.log, "VITA_RETAIL_CLIP_PROBE commands=%u projected=%u tested=%u finite=%u positive_w=%u inside_xy=%u inside_xyz=%u first_finite_cmd=%u first_inside_cmd=%u ndc_x=%g,%g ndc_y=%g,%g ndc_z=%g,%g\n", command_count, projected_commands, tested, finite, positive_w, inside_xy, inside_xyz, first_finite_cmd, first_inside_cmd, min_x, max_x, min_y, max_y, min_z, max_z);
    if (first_bad_cmd != UINT32_MAX) {
        const MvGxCaptureCommand *c = &commands[first_bad_cmd];
        const MvGxCaptureVertex *v = &vertices[c->first_vertex + first_bad_vertex];
        int pos_finite = 1, pos_zero = 1, proj_finite = 1, proj_zero = 1;
        for (int r = 0; r < 3; ++r) for (int k = 0; k < 4; ++k) { float value = c->pos_mtx[r][k]; if (!isfinite(value)) pos_finite = 0; if (value != 0.0f) pos_zero = 0; }
        for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) { float value = c->projection[r][k]; if (!isfinite(value)) proj_finite = 0; if (value != 0.0f) proj_zero = 0; }
        fprintf(retail.log, "VITA_RETAIL_FIRST_BAD_CLIP command=%u vertex=%u primitive=%u attr_mask=%08x current_mtx=%u pos_loaded=%u pos_finite=%d pos_zero=%d proj_finite=%d proj_zero=%d vertex_xyz=%g,%g,%g view_xyz1=%g,%g,%g,%g clip_xyzw=%g,%g,%g,%g\n", first_bad_cmd, first_bad_vertex, c->primitive, c->attr_mask, c->current_mtx, c->pos_mtx_valid, pos_finite, pos_zero, proj_finite, proj_zero, v->position[0], v->position[1], v->position[2], first_bad_p[0], first_bad_p[1], first_bad_p[2], first_bad_p[3], first_bad_clip[0], first_bad_clip[1], first_bad_clip[2], first_bad_clip[3]);
        fprintf(retail.log, "VITA_RETAIL_FIRST_BAD_POSMTX command=%u m=%g,%g,%g,%g;%g,%g,%g,%g;%g,%g,%g,%g\n", first_bad_cmd, c->pos_mtx[0][0], c->pos_mtx[0][1], c->pos_mtx[0][2], c->pos_mtx[0][3], c->pos_mtx[1][0], c->pos_mtx[1][1], c->pos_mtx[1][2], c->pos_mtx[1][3], c->pos_mtx[2][0], c->pos_mtx[2][1], c->pos_mtx[2][2], c->pos_mtx[2][3]);
        fprintf(retail.log, "VITA_RETAIL_FIRST_BAD_PROJ command=%u p=%g,%g,%g,%g;%g,%g,%g,%g;%g,%g,%g,%g;%g,%g,%g,%g\n", first_bad_cmd, c->projection[0][0], c->projection[0][1], c->projection[0][2], c->projection[0][3], c->projection[1][0], c->projection[1][1], c->projection[1][2], c->projection[1][3], c->projection[2][0], c->projection[2][1], c->projection[2][2], c->projection[2][3], c->projection[3][0], c->projection[3][1], c->projection[3][2], c->projection[3][3]);
    }
    if (first_inside_cmd != UINT32_MAX) {
        const MvGxCaptureCommand *c = &commands[first_inside_cmd];
        const MvGxMaterialState *m = &c->material;
        fprintf(retail.log, "VITA_RETAIL_DRAW_STATE_PROBE command=%u primitive=%u triangles=%u attr_mask=%08x current_mtx=%u cull=%u projection_type=%u viewport=%u:%g,%g,%g,%g,%g,%g material=%08x textures=%u unsupported=%08x blend=%u:%u,%u z=%u:%u:%u color_alpha=%u,%u alpha_cmp=%u,%u,%u,%u,%u\n", first_inside_cmd, c->primitive, c->triangle_count, c->attr_mask, c->current_mtx, c->cull_mode, c->projection_type, c->viewport_valid, c->viewport[0], c->viewport[1], c->viewport[2], c->viewport[3], c->viewport[4], c->viewport[5], m->material_rgba, m->texture_count, m->unsupported, m->pe_blend_type, m->pe_src_factor, m->pe_dst_factor, m->pe_z_enable, m->pe_z_func, m->pe_z_update, m->pe_color_update, m->pe_alpha_update, m->pe_alpha_comp0, m->pe_alpha_ref0, m->pe_alpha_op, m->pe_alpha_comp1, m->pe_alpha_ref1);
    }
    fflush(retail.log);
}

static int mv_retail_replay_reset(uint32_t generation)
{
    if (retail.replay.textures != NULL || retail.replay.ready)
        mv_gx_replay_close(&retail.replay);
    memset(&retail.replay, 0, sizeof(retail.replay));
    if (mv_gx_replay_init_streaming(&retail.replay, retail.log) != 0)
        return -1;
    retail.heap_generation = generation;
    return 0;
}

int mv_retail_runtime_begin(FILE *log)
{
    if (retail.active)
        mv_retail_runtime_end();
    memset(&retail, 0, sizeof(retail));
    retail.log = log;
    if (mv_render_init() < 0)
        return -1;
    if (mv_retail_replay_reset(HSD_GetHeapGeneration()) != 0)
        return -2;
    mv_gx_capture_reset();
    retail.active = 1;
    if (retail.log) {
        fprintf(retail.log,
                "VITA_RETAIL_RUNTIME_BEGIN heap_generation=%u renderer=vitaGL bridge=HSD_StartRender_to_HSD_VICopyXFBAsync\n",
                retail.heap_generation);
        fflush(retail.log);
    }
    return 0;
}

void mv_retail_runtime_end(void)
{
    if (!retail.active)
        return;
    if (retail.replay.textures != NULL || retail.replay.ready)
        mv_gx_replay_close(&retail.replay);
    mv_render_fini();
    memset(&retail, 0, sizeof(retail));
}

void mv_retail_runtime_start_render(int pass)
{
    if (!retail.active || pass != HSD_RP_SCREEN)
        return;

    uint32_t generation = HSD_GetHeapGeneration();
    if (generation != retail.heap_generation) {
        if (retail.log) {
            fprintf(retail.log,
                    "VITA_RETAIL_HEAP_GENERATION old=%u new=%u action=flush_texture_cache\n",
                    retail.heap_generation, generation);
            fflush(retail.log);
        }
        if (mv_retail_replay_reset(generation) != 0) {
            if (retail.log) {
                fprintf(retail.log, "VITA_RETAIL_REPLAY_RESET_FAIL generation=%u\n",
                        generation);
                fflush(retail.log);
            }
            retail.active = 0;
            return;
        }
    }

    mv_gx_capture_begin_frame();
    retail.capture_started = 1;
}

void mv_retail_runtime_present_frame(int pass)
{
    if (!retail.active || pass != HSD_RP_SCREEN || !retail.capture_started)
        return;

    MvGxCaptureStats stats;
    memset(&stats, 0, sizeof(stats));
    int capture_result = mv_gx_capture_stats(&stats);

    if (retail.frame == 0u ||
        retail.clip_probe_generation != retail.heap_generation) {
        mv_retail_log_clip_probe();
        retail.clip_probe_generation = retail.heap_generation;
    }
    mv_retail_log_tail_commands(retail.frame + 1u);

#ifdef MELEE_VITA_GXM_DEBUG
    glPushGroupMarker(0, "MeleeGameplayFrame");
#endif
    mv_render_begin();
    mv_gx_replay_draw_captured(&retail.replay);
#ifdef MELEE_VITA_GXM_DEBUG
    glPopGroupMarker();
#endif
    mv_render_present();

    ++retail.frame;
    if (retail.log &&
        (retail.frame <= 8u || (retail.frame % 120u) == 0u || capture_result != 0))
    {
        fprintf(retail.log,
                "VITA_RETAIL_PRESENT frame=%u commands=%u vertices=%u triangles=%u capture_errors=%u capture_result=%d first_error_line=%u error_args=%u,%u,%u pos_mtx_loads=%u nrm_mtx_loads=%u channel_eval=%u channel_lit=%u channel_normals=%u textures=%u heap_generation=%u\n",
                retail.frame, stats.commands, stats.vertices, stats.triangles,
                stats.errors, capture_result, stats.first_error_line,
                stats.first_error_arg0, stats.first_error_arg1,
                stats.first_error_arg2, stats.pos_mtx_loads, stats.nrm_mtx_loads,
                stats.channel_eval_vertices, stats.channel_lit_vertices,
                stats.channel_normal_vertices,
                retail.replay.texture_count, retail.heap_generation);
        fflush(retail.log);
    }
    retail.capture_started = 0;
}
