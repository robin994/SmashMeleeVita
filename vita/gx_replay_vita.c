#include "gx_replay_vita.h"
#include "gx_texture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MV_GX_VA_PNMTXIDX 0u
#define MV_GX_VA_POS 9u
#define MV_GX_VA_CLR0 11u
#define MV_GX_VA_TEX0 13u
#define MV_GX_QUADS 0x80u
#define MV_GX_TRIANGLES 0x90u
#define MV_GX_TRIANGLESTRIP 0x98u
#define MV_GX_TRIANGLEFAN 0xa0u
#define MV_GX_REPLAY_TARGET_WIDTH 960u
#define MV_GX_REPLAY_TARGET_HEIGHT 544u
#define MV_GX_CULL_NONE 0u
#define MV_GX_CULL_FRONT 1u
#define MV_GX_CULL_BACK 2u
#define MV_GX_CULL_ALL 3u

static int pe_supported(const MvGxMaterialState *m)
{
    if (!m) return 0;
    /* MenMainBack uses exactly two blend states.  Both write RGB only, test
     * LEQUAL, never update Z, and use an always-pass alpha compare. */
    if (!m->pe_color_update || m->pe_alpha_update || m->pe_dst_alpha_enable ||
        m->pe_blend_type != 1u || m->pe_src_factor != 4u ||
        (m->pe_dst_factor != 5u && m->pe_dst_factor != 1u) ||
        !m->pe_z_enable || m->pe_z_func != 3u || m->pe_z_update ||
        m->pe_alpha_comp0 != 7u || m->pe_alpha_comp1 != 7u || m->pe_dither)
        return 0;
    return 1;
}

/* libvita2d ships these GXP programs and intentionally shares its patched
 * texture program pointers across source units. Reusing the same binaries
 * with two additional blend states avoids introducing a shader compiler or a
 * new runtime dependency for the one two-stage MenMainBack material. */
extern const SceGxmProgram texture_v_gxp_start;
extern const SceGxmProgram texture_tint_f_gxp_start;
extern SceGxmFragmentProgram *_vita2d_textureTintFragmentProgram;

/* First replay milestone: exact GX geometry + rigid model matrices, with only
 * the single-texture material cases that can be represented by vita2d's
 * textured helper. TEV, multitexture, PE/depth/cull state remain explicit
 * frontiers rather than being approximated as successful GX support. */

typedef struct {
    float x, y, depth, u, v, u1, v1;
    uint32_t rgba;
} MvReplayCameraVertex;

typedef struct {
    float right[3], up[3], forward[3];
} MvReplayCameraBasis;

static int command_supported(const MvGxCaptureCommand *command, int relaxed)
{
    if (!command) return 0;
    if (!pe_supported(&command->material)) return 0;
    const uint32_t replay_bakeable = MV_GX_MATERIAL_UNSUPPORTED_COLORMAP |
                                     MV_GX_MATERIAL_UNSUPPORTED_ALPHAMAP;
    uint32_t unsupported = command->material.unsupported;
    if (relaxed) {
        /* TtlMoji compatibility bridge: preserve the first HSD texture layer
         * until all title TEV/MatAnim stages have native Vita equivalents. */
        unsupported &= ~MV_GX_MATERIAL_UNSUPPORTED_MULTITEX;
        unsupported &= ~MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV;
    } else {
        if (unsupported & MV_GX_MATERIAL_UNSUPPORTED_MULTITEX) {
            if (!mv_gx_material_multitex_offscreen_bakeable(&command->material)) return 0;
            unsupported &= ~MV_GX_MATERIAL_UNSUPPORTED_MULTITEX;
        }
        if (unsupported & MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV) {
            if (!mv_gx_material_custom_tev_cpu_bakeable(&command->material)) return 0;
            unsupported &= ~MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV;
        }
    }
    if (unsupported & ~replay_bakeable) return 0;
    if (!(command->attr_mask & (1u << MV_GX_VA_POS))) return 0;
    if (command->material.texture_count) {
        if (!command->material.image || !command->material.width || !command->material.height ||
            !(command->attr_mask & (1u << MV_GX_VA_TEX0))) return 0;
        if (!relaxed && (command->attr_mask & (1u << MV_GX_VA_CLR0))) return 0;
        if (!relaxed && command->material.texture_count > 2) return 0;
    }
    return 1;
}

static int normalize3(float v[3])
{
    float length2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!isfinite(length2) || length2 < 1.0e-12f) return -1;
    float inv = 1.0f / sqrtf(length2);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
    return 0;
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int camera_basis(const MvCamera *camera, MvReplayCameraBasis *basis)
{
    for (unsigned i = 0; i < 3; ++i)
        basis->forward[i] = camera->interest[i] - camera->eye[i];
    if (normalize3(basis->forward)) return -1;
    cross3(basis->forward, camera->up, basis->right);
    if (normalize3(basis->right)) return -1;
    cross3(basis->right, basis->forward, basis->up);
    return normalize3(basis->up);
}

static void transform_point(const float m[3][4], const float in[3], float out[3])
{
    out[0] = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2] + m[0][3];
    out[1] = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2] + m[1][3];
    out[2] = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2] + m[2][3];
}

static MvReplayCameraVertex camera_vertex(const MvCamera *camera,
                                          const MvReplayCameraBasis *basis,
                                          const MvGxCaptureCommand *command,
                                          const MvGxCaptureVertex *source)
{
    float world[3];
    transform_point(command->pos_mtx, source->position, world);
    float delta[3] = {world[0] - camera->eye[0], world[1] - camera->eye[1],
                      world[2] - camera->eye[2]};
    MvReplayCameraVertex out;
    out.x = delta[0] * basis->right[0] + delta[1] * basis->right[1] + delta[2] * basis->right[2];
    out.y = delta[0] * basis->up[0] + delta[1] * basis->up[1] + delta[2] * basis->up[2];
    out.depth = delta[0] * basis->forward[0] + delta[1] * basis->forward[1] + delta[2] * basis->forward[2];
    out.u = source->tex0[0]; out.v = source->tex0[1];
    out.u1 = out.u; out.v1 = out.v;
    if (command->material.uv_mtx_valid) {
        float u = out.u, v = out.v;
        out.u = command->material.uv_mtx[0][0] * u + command->material.uv_mtx[0][1] * v +
                command->material.uv_mtx[0][2];
        out.v = command->material.uv_mtx[1][0] * u + command->material.uv_mtx[1][1] * v +
                command->material.uv_mtx[1][2];
    }
    if (command->material.uv_mtx1_valid) {
        float u = out.u1, v = out.v1;
        out.u1 = command->material.uv_mtx1[0][0] * u +
                 command->material.uv_mtx1[0][1] * v +
                 command->material.uv_mtx1[0][2];
        out.v1 = command->material.uv_mtx1[1][0] * u +
                 command->material.uv_mtx1[1][1] * v +
                 command->material.uv_mtx1[1][2];
    }
    out.rgba = (source->present & (1u << MV_GX_VA_CLR0)) ? source->color0 : command->material.material_rgba;
    return out;
}

static uint32_t lerp_rgba(uint32_t a, uint32_t b, float t)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 24; shift += 8) {
        float av = (float)((a >> shift) & 0xff), bv = (float)((b >> shift) & 0xff);
        unsigned value = (unsigned)(av + (bv - av) * t + 0.5f);
        if (value > 255) value = 255;
        result |= value << shift;
    }
    return result;
}

static MvReplayCameraVertex lerp_vertex(const MvReplayCameraVertex *a,
                                        const MvReplayCameraVertex *b, float t)
{
    MvReplayCameraVertex out;
    out.x = a->x + (b->x - a->x) * t;
    out.y = a->y + (b->y - a->y) * t;
    out.depth = a->depth + (b->depth - a->depth) * t;
    out.u = a->u + (b->u - a->u) * t;
    out.v = a->v + (b->v - a->v) * t;
    out.u1 = a->u1 + (b->u1 - a->u1) * t;
    out.v1 = a->v1 + (b->v1 - a->v1) * t;
    out.rgba = lerp_rgba(a->rgba, b->rgba, t);
    return out;
}

static size_t clip_depth(const MvReplayCameraVertex *input, size_t count,
                         MvReplayCameraVertex *output, float plane, int keep_greater)
{
    if (!count) return 0;
    size_t out_count = 0;
    MvReplayCameraVertex previous = input[count - 1];
    int previous_inside = keep_greater ? previous.depth >= plane : previous.depth <= plane;
    for (size_t i = 0; i < count; ++i) {
        MvReplayCameraVertex current = input[i];
        int current_inside = keep_greater ? current.depth >= plane : current.depth <= plane;
        if (current_inside != previous_inside) {
            float denominator = current.depth - previous.depth;
            if (fabsf(denominator) > 1.0e-12f) {
                float t = (plane - previous.depth) / denominator;
                output[out_count] = lerp_vertex(&previous, &current, t);
                output[out_count++].depth = plane;
            }
        }
        if (current_inside) output[out_count++] = current;
        previous = current;
        previous_inside = current_inside;
    }
    return out_count;
}

static void viewport(const MvCamera *camera, float *x, float *y, float *w, float *h)
{
    float aspect = camera && camera->aspect > 0.0f ? camera->aspect : (4.0f / 3.0f);
    *h = (float)MV_GX_REPLAY_TARGET_HEIGHT;
    *w = *h * aspect;
    if (*w > (float)MV_GX_REPLAY_TARGET_WIDTH) {
        *w = (float)MV_GX_REPLAY_TARGET_WIDTH;
        *h = *w / aspect;
    }
    *x = ((float)MV_GX_REPLAY_TARGET_WIDTH - *w) * 0.5f;
    *y = ((float)MV_GX_REPLAY_TARGET_HEIGHT - *h) * 0.5f;
}

static int project(const MvCamera *camera, const MvReplayCameraVertex *source,
                   float view_x, float view_y, float view_w, float view_h,
                   float *x, float *y, float *z)
{
    if (source->depth < camera->near_z || source->depth > camera->far_z) return -1;
    float nx, ny;
    if (camera->projection_type == 1) {
        float tangent = tanf(camera->fov_y * (3.14159265358979323846f / 360.0f));
        if (!isfinite(tangent) || tangent <= 0.0f) return -1;
        nx = source->x / (source->depth * tangent * camera->aspect);
        ny = source->y / (source->depth * tangent);
    } else if (camera->projection_type == 2) {
        float px = camera->near_z * source->x / source->depth;
        float py = camera->near_z * source->y / source->depth;
        nx = (2.0f * px - camera->right - camera->left) / (camera->right - camera->left);
        ny = (2.0f * py - camera->top - camera->bottom) / (camera->top - camera->bottom);
    } else {
        nx = (2.0f * source->x - camera->right - camera->left) / (camera->right - camera->left);
        ny = (2.0f * source->y - camera->top - camera->bottom) / (camera->top - camera->bottom);
    }
    if (!isfinite(nx) || !isfinite(ny)) return -1;
    *x = view_x + (nx * 0.5f + 0.5f) * view_w;
    *y = view_y + (0.5f - ny * 0.5f) * view_h;
    /* MenMainBack's captured PE state is LEQUAL with Z writes disabled for
     * every command.  The isolated viewer starts from a clear depth surface,
     * so all in-range fragments are visible regardless of mutual depth order.
     * Use the nearest representable depth while the general depth-clear/EFB
     * path is still pending; the real compare/write state is programmed below. */
    *z = 0.0f;
    return 0;
}

static int triangle_indices(const MvGxCaptureCommand *command, uint32_t triangle,
                            uint32_t out[3])
{
    switch (command->primitive) {
    case MV_GX_TRIANGLES:
        out[0] = triangle * 3; out[1] = out[0] + 1; out[2] = out[0] + 2;
        break;
    case MV_GX_QUADS: {
        uint32_t base = (triangle / 2) * 4;
        if (triangle & 1) { out[0] = base; out[1] = base + 2; out[2] = base + 3; }
        else { out[0] = base; out[1] = base + 1; out[2] = base + 2; }
        break;
    }
    case MV_GX_TRIANGLESTRIP:
        out[0] = triangle; out[1] = triangle + 1; out[2] = triangle + 2;
        if (triangle & 1) { uint32_t swap = out[0]; out[0] = out[1]; out[1] = swap; }
        break;
    case MV_GX_TRIANGLEFAN:
        out[0] = 0; out[1] = triangle + 1; out[2] = triangle + 2;
        break;
    default:
        return -1;
    }
    if (out[0] >= command->vertex_count || out[1] >= command->vertex_count ||
        out[2] >= command->vertex_count) return -1;
    return 0;
}

static int cull_projected_triangle(uint32_t cull_mode,
                                   float x0, float y0, float x1, float y1,
                                   float x2, float y2)
{
    if (cull_mode == MV_GX_CULL_NONE) return 0;
    if (cull_mode == MV_GX_CULL_ALL) return 1;
    const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (!isfinite(area) || fabsf(area) < 1.0e-7f) return 1;
    /* GX defines front-facing polygons as clockwise in window coordinates.
     * Our projected window has +Y downward, therefore positive signed area is
     * clockwise.  Cull the semantic face requested by the original PObj. */
    const int front = area > 0.0f;
    if (cull_mode == MV_GX_CULL_BACK) return !front;
    if (cull_mode == MV_GX_CULL_FRONT) return front;
    return 1;
}

static int texture_matches(const MvGxReplayTexture *texture, const MvGxMaterialState *material)
{
    return texture->image == material->image && texture->palette == material->palette &&
           texture->material_rgba == material->material_rgba &&
           texture->tobj_flags == material->tobj_flags &&
           texture->blending == material->blending &&
           texture->width == material->width && texture->height == material->height &&
           texture->format == material->format && texture->palette_entries == material->palette_entries &&
           texture->palette_format == material->palette_format &&
           texture->wrap_s == material->wrap_s && texture->wrap_t == material->wrap_t &&
           texture->tev_valid == material->tev_valid &&
           texture->tev_active == material->tev_active &&
           (!material->tev_valid ||
            (!memcmp(texture->tev_op, material->tev_op, sizeof(texture->tev_op)) &&
             !memcmp(texture->tev_konst, material->tev_konst, sizeof(texture->tev_konst)) &&
             !memcmp(texture->tev0, material->tev0, sizeof(texture->tev0)) &&
             !memcmp(texture->tev1, material->tev1, sizeof(texture->tev1))));
}

static uint8_t clamp_u8_int(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static uint8_t tev_lerp_u8(uint8_t a, uint8_t b, uint8_t c)
{
    return (uint8_t)(((unsigned)a * (255u - c) + (unsigned)b * c + 127u) / 255u);
}

static uint8_t tev_scale_u8(uint8_t value, float factor)
{
    if (!isfinite(factor)) return 0;
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    return (uint8_t)(value * factor + 0.5f);
}

static int material_needs_bake(const MvGxMaterialState *material)
{
    unsigned colormap = (material->tobj_flags >> 16) & 0xfu;
    unsigned alphamap = (material->tobj_flags >> 20) & 0xfu;
    return (material->tev_valid && material->tev_active != 0 &&
            mv_gx_material_custom_tev_cpu_bakeable(material)) ||
           !((colormap == 4u || colormap == 5u) &&
             (alphamap == 3u || alphamap == 4u));
}

static void bake_custom_tev_pixel(const MvGxMaterialState *material, uint8_t rgba[4])
{
    if (!material->tev_valid || material->tev_active == 0 ||
        !mv_gx_material_custom_tev_cpu_bakeable(material)) return;

    /* The narrow matcher in mv_gx_material_custom_tev_cpu_bakeable guarantees
     * the exact MenMainBack stage: ADD, no bias, scale 1, clamp, with
     * A=TEV0.rgb, B=KONST.rgb, C=texture.rgb and D=zero. */
    uint8_t tex_rgb[3] = {rgba[0], rgba[1], rgba[2]};
    for (unsigned c = 0; c < 3; ++c)
        rgba[c] = tev_lerp_u8(material->tev0[c], material->tev_konst[c], tex_rgb[c]);
}

static void bake_material_pixel(const MvGxMaterialState *material, uint8_t rgba[4])
{
    bake_custom_tev_pixel(material, rgba);
    uint8_t src[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    uint8_t mat[4] = {
        (uint8_t)(material->material_rgba >> 24),
        (uint8_t)(material->material_rgba >> 16),
        (uint8_t)(material->material_rgba >> 8),
        (uint8_t)material->material_rgba,
    };
    unsigned colormap = (material->tobj_flags >> 16) & 0xfu;
    unsigned alphamap = (material->tobj_flags >> 20) & 0xfu;
    uint8_t blend = tev_scale_u8(255, material->blending);

    for (unsigned c = 0; c < 3; ++c) {
        switch (colormap) {
        case 0: /* NONE */
        case 6: /* PASS */
            rgba[c] = mat[c];
            break;
        case 1: /* ALPHA_MASK */
            rgba[c] = tev_lerp_u8(mat[c], src[c], src[3]);
            break;
        case 2: /* RGB_MASK */
            rgba[c] = tev_lerp_u8(mat[c], src[c], src[c]);
            break;
        case 3: /* BLEND */
            rgba[c] = tev_lerp_u8(mat[c], src[c], blend);
            break;
        case 4: /* MODULATE */
            rgba[c] = tev_lerp_u8(0, mat[c], src[c]);
            break;
        case 5: /* REPLACE */
            rgba[c] = src[c];
            break;
        case 7: /* ADD */
            rgba[c] = clamp_u8_int((int)mat[c] + src[c]);
            break;
        case 8: /* SUB */
            rgba[c] = clamp_u8_int((int)mat[c] - src[c]);
            break;
        default:
            rgba[c] = 0;
            break;
        }
    }

    switch (alphamap) {
    case 0: /* NONE */
    case 5: /* PASS */
        rgba[3] = mat[3];
        break;
    case 1: /* ALPHA_MASK */
        rgba[3] = tev_lerp_u8(mat[3], src[3], src[3]);
        break;
    case 2: /* BLEND */
        rgba[3] = tev_lerp_u8(mat[3], src[3], blend);
        break;
    case 3: /* MODULATE */
        rgba[3] = tev_lerp_u8(0, mat[3], src[3]);
        break;
    case 4: /* REPLACE */
        rgba[3] = src[3];
        break;
    case 6: /* ADD */
        rgba[3] = clamp_u8_int((int)mat[3] + src[3]);
        break;
    case 7: /* SUB */
        rgba[3] = clamp_u8_int((int)mat[3] - src[3]);
        break;
    default:
        rgba[3] = 0;
        break;
    }
}

static MvGxReplayTexture *find_texture(MvGxReplay *replay, const MvGxMaterialState *material)
{
    for (uint32_t i = 0; i < replay->texture_count; ++i)
        if (texture_matches(&replay->textures[i], material)) return &replay->textures[i];
    return NULL;
}

static int gx_wrap_to_gxm(uint8_t wrap, SceGxmTextureAddrMode *out)
{
    if (!out) return -1;
    switch (wrap) {
    case 0: *out = SCE_GXM_TEXTURE_ADDR_CLAMP; return 0;  /* GX_CLAMP */
    case 1: *out = SCE_GXM_TEXTURE_ADDR_REPEAT; return 0; /* GX_REPEAT */
    /* vita2d's linear-strided textures reject GXM MIRROR on physical Vita.
     * MIRROR is expanded into an original|reflected texture below and sampled
     * with REPEAT, which has the same two-unit period once UV is halved. */
    case 2: *out = SCE_GXM_TEXTURE_ADDR_REPEAT; return 0; /* baked GX_MIRROR */
    default: return -1;
    }
}

static vita2d_texture *create_texture(const MvGxMaterialState *material,
                                      uint16_t *storage_width, uint16_t *storage_height,
                                      float *uv_scale_s, float *uv_scale_t)
{
    size_t source_size = mv_gx_texture_size(material->width, material->height, material->format);
    if (!source_size || !material->image) return NULL;
    const unsigned mirror_s = material->wrap_s == 2;
    const unsigned mirror_t = material->wrap_t == 2;
    const unsigned out_width = (unsigned)material->width * (mirror_s ? 2u : 1u);
    const unsigned out_height = (unsigned)material->height * (mirror_t ? 2u : 1u);
    if (!out_width || !out_height || out_width > UINT16_MAX || out_height > UINT16_MAX) return NULL;
    vita2d_texture *texture = vita2d_create_empty_texture_format(out_width, out_height,
                                                                 SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!texture) return NULL;
    size_t stride = vita2d_texture_get_stride(texture);
    uint8_t *pixels = vita2d_texture_get_datap(texture);
    size_t palette_size = (size_t)material->palette_entries * 2u;
    if (!pixels || mv_gx_decode(pixels, stride * material->height, stride,
                                material->image, source_size, material->width, material->height,
                                material->format, material->palette, palette_size,
                                material->palette_format)) {
        vita2d_free_texture(texture);
        return NULL;
    }
    if (material_needs_bake(material)) {
        for (unsigned y = 0; y < material->height; ++y) {
            uint8_t *row = pixels + (size_t)y * stride;
            for (unsigned x = 0; x < material->width; ++x)
                bake_material_pixel(material, row + x * 4u);
        }
    }
    if (mirror_s) {
        for (unsigned y = 0; y < material->height; ++y) {
            uint8_t *row = pixels + (size_t)y * stride;
            for (unsigned x = 0; x < material->width; ++x)
                memcpy(row + (size_t)(material->width + x) * 4u,
                       row + (size_t)(material->width - 1u - x) * 4u, 4u);
        }
    }
    if (mirror_t) {
        const size_t row_bytes = (size_t)out_width * 4u;
        for (unsigned y = 0; y < material->height; ++y)
            memcpy(pixels + (size_t)(material->height + y) * stride,
                   pixels + (size_t)(material->height - 1u - y) * stride,
                   row_bytes);
    }
    vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
                               SCE_GXM_TEXTURE_FILTER_LINEAR);
    SceGxmTextureAddrMode u_mode, v_mode;
    if (gx_wrap_to_gxm(material->wrap_s, &u_mode) ||
        gx_wrap_to_gxm(material->wrap_t, &v_mode) ||
        sceGxmTextureSetUAddrMode(&texture->gxm_tex, u_mode) < 0 ||
        sceGxmTextureSetVAddrMode(&texture->gxm_tex, v_mode) < 0) {
        vita2d_free_texture(texture);
        return NULL;
    }
    if (storage_width) *storage_width = (uint16_t)out_width;
    if (storage_height) *storage_height = (uint16_t)out_height;
    if (uv_scale_s) *uv_scale_s = mirror_s ? 0.5f : 1.0f;
    if (uv_scale_t) *uv_scale_t = mirror_t ? 0.5f : 1.0f;
    return texture;
}

static MvGxReplayTexture *prepare_texture(MvGxReplay *replay,
                                          const MvGxMaterialState *material,
                                          uint32_t command_index)
{
    MvGxReplayTexture *existing = find_texture(replay, material);
    if (existing) return existing;
    if (replay->texture_count >= MV_GX_REPLAY_TEXTURE_LIMIT) return NULL;

    MvGxReplayTexture *entry = &replay->textures[replay->texture_count];
    memset(entry, 0, sizeof(*entry));
    entry->image = material->image;
    entry->palette = material->palette;
    entry->material_rgba = material->material_rgba;
    entry->tobj_flags = material->tobj_flags;
    entry->blending = material->blending;
    entry->width = material->width;
    entry->height = material->height;
    entry->format = material->format;
    entry->palette_entries = material->palette_entries;
    entry->palette_format = material->palette_format;
    entry->wrap_s = material->wrap_s;
    entry->wrap_t = material->wrap_t;
    entry->tev_valid = material->tev_valid;
    memcpy(entry->tev_op, material->tev_op, sizeof(entry->tev_op));
    memcpy(entry->tev_konst, material->tev_konst, sizeof(entry->tev_konst));
    memcpy(entry->tev0, material->tev0, sizeof(entry->tev0));
    memcpy(entry->tev1, material->tev1, sizeof(entry->tev1));
    entry->tev_active = material->tev_active;
    entry->texture = create_texture(material, &entry->storage_width, &entry->storage_height,
                                    &entry->uv_scale_s, &entry->uv_scale_t);
    if (!entry->texture) {
        ++replay->texture_failures;
        if (replay->log)
            fprintf(replay->log, "GX_REPLAY_TEXTURE_FAIL command=%u size=%ux%u fmt=%u\n",
                    command_index, entry->width, entry->height, entry->format);
        memset(entry, 0, sizeof(*entry));
        return NULL;
    }
    replay->texture_bytes += (uint32_t)entry->storage_width * entry->storage_height * 4u;
    if (entry->wrap_s == 2u || entry->wrap_t == 2u)
        ++replay->mirror_baked_textures;
    if (entry->tev_valid) {
        if (entry->tev_active) ++replay->custom_tev_baked_commands;
        else ++replay->custom_tev_noop_commands;
    }
    ++replay->texture_count;
    return entry;
}

static void second_layer_material(const MvGxMaterialState *source, MvGxMaterialState *out)
{
    memset(out, 0, sizeof(*out));
    out->image = source->image1;
    out->palette = source->palette1;
    out->material_rgba = 0xffffffffu;
    out->tobj_flags = source->tobj1_flags;
    out->blending = source->blending1;
    out->width = source->width1;
    out->height = source->height1;
    out->palette_entries = source->palette_entries1;
    out->format = source->format1;
    out->palette_format = source->palette_format1;
    out->wrap_s = source->wrap_s1;
    out->wrap_t = source->wrap_t1;
    out->mag_filter = source->mag_filter1;
    out->texture_count = 1;
    out->uv_mtx_valid = source->uv_mtx1_valid;
    memcpy(out->uv_mtx, source->uv_mtx1, sizeof(out->uv_mtx));
    out->tev_valid = source->tev1_valid;
    memcpy(out->tev_op, source->tev1_op, sizeof(out->tev_op));
    memcpy(out->tev_konst, source->tev1_konst, sizeof(out->tev_konst));
    memcpy(out->tev0, source->tev1_reg0, sizeof(out->tev0));
    memcpy(out->tev1, source->tev1_reg1, sizeof(out->tev1));
    out->tev_active = source->tev1_active;
}

static int uv_matrix_nonidentity(const MvGxMaterialState *material)
{
    if (!material->uv_mtx_valid) return 0;
    const float expected[2][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    for (unsigned r = 0; r < 2; ++r)
        for (unsigned c = 0; c < 3; ++c)
            if (fabsf(material->uv_mtx[r][c] - expected[r][c]) > 1.0e-6f) return 1;
    return 0;
}

static unsigned tint_color(const MvGxMaterialState *material)
{
    if (material_needs_bake(material)) return RGBA8(255, 255, 255, 255);
    uint32_t rgba = material->material_rgba;
    unsigned colormap = (material->tobj_flags >> 16) & 0xfu;
    unsigned alphamap = (material->tobj_flags >> 20) & 0xfu;
    if (colormap == 5u) rgba = (rgba & 0xffu) | 0xffffff00u; /* REPLACE RGB */
    if (alphamap == 4u) rgba = (rgba & 0xffffff00u) | 0xffu; /* REPLACE alpha */
    return RGBA8(rgba >> 24, rgba >> 16, rgba >> 8, rgba);
}

static void release_multitex_programs(MvGxReplay *replay)
{
    SceGxmShaderPatcher *patcher = vita2d_get_shader_patcher();
    if (patcher && replay->multitex_replace_program)
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, replay->multitex_replace_program);
    if (patcher && replay->multitex_multiply_program)
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, replay->multitex_multiply_program);
    if (patcher && replay->pe_alpha_program)
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, replay->pe_alpha_program);
    if (patcher && replay->pe_additive_program)
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, replay->pe_additive_program);
    replay->multitex_replace_program = NULL;
    replay->multitex_multiply_program = NULL;
    replay->pe_alpha_program = NULL;
    replay->pe_additive_program = NULL;
    replay->pe_ready = 0;
    if (patcher && replay->multitex_program_registered)
        sceGxmShaderPatcherUnregisterProgram(patcher, replay->multitex_program_id);
    replay->multitex_program_registered = 0;
}

static int init_multitex_programs(MvGxReplay *replay)
{
    SceGxmShaderPatcher *patcher = vita2d_get_shader_patcher();
    if (!patcher) return -1;
    int result = sceGxmShaderPatcherRegisterProgram(
        patcher, &texture_tint_f_gxp_start, &replay->multitex_program_id);
    if (result < 0) {
        if (replay->log)
            fprintf(replay->log, "GX_MULTITEX_PROGRAM_FAIL stage=register result=%08x\n",
                    (unsigned)result);
        return -1;
    }
    replay->multitex_program_registered = 1;

    const SceGxmBlendInfo replace = {
        .colorFunc = SCE_GXM_BLEND_FUNC_ADD,
        .alphaFunc = SCE_GXM_BLEND_FUNC_ADD,
        .colorSrc = SCE_GXM_BLEND_FACTOR_ONE,
        .colorDst = SCE_GXM_BLEND_FACTOR_ZERO,
        .alphaSrc = SCE_GXM_BLEND_FACTOR_ONE,
        .alphaDst = SCE_GXM_BLEND_FACTOR_ZERO,
        .colorMask = SCE_GXM_COLOR_MASK_ALL,
    };
    const SceGxmBlendInfo multiply = {
        .colorFunc = SCE_GXM_BLEND_FUNC_ADD,
        .alphaFunc = SCE_GXM_BLEND_FUNC_ADD,
        .colorSrc = SCE_GXM_BLEND_FACTOR_DST_COLOR,
        .colorDst = SCE_GXM_BLEND_FACTOR_ZERO,
        .alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO,
        .alphaDst = SCE_GXM_BLEND_FACTOR_ONE,
        .colorMask = SCE_GXM_COLOR_MASK_ALL,
    };
    const SceGxmColorMask rgb_mask = (SceGxmColorMask)(
        SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G | SCE_GXM_COLOR_MASK_B);
    const SceGxmBlendInfo pe_alpha = {
        .colorFunc = SCE_GXM_BLEND_FUNC_ADD,
        .alphaFunc = SCE_GXM_BLEND_FUNC_ADD,
        .colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA,
        .colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO,
        .alphaDst = SCE_GXM_BLEND_FACTOR_ONE,
        .colorMask = rgb_mask,
    };
    const SceGxmBlendInfo pe_additive = {
        .colorFunc = SCE_GXM_BLEND_FUNC_ADD,
        .alphaFunc = SCE_GXM_BLEND_FUNC_ADD,
        .colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA,
        .colorDst = SCE_GXM_BLEND_FACTOR_ONE,
        .alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO,
        .alphaDst = SCE_GXM_BLEND_FACTOR_ONE,
        .colorMask = rgb_mask,
    };
    result = sceGxmShaderPatcherCreateFragmentProgram(
        patcher, replay->multitex_program_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE, &replace, &texture_v_gxp_start,
        &replay->multitex_replace_program);
    if (result < 0) {
        if (replay->log)
            fprintf(replay->log, "GX_MULTITEX_PROGRAM_FAIL stage=replace result=%08x\n",
                    (unsigned)result);
        release_multitex_programs(replay);
        return -1;
    }
    result = sceGxmShaderPatcherCreateFragmentProgram(
        patcher, replay->multitex_program_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE, &multiply, &texture_v_gxp_start,
        &replay->multitex_multiply_program);
    if (result < 0) {
        if (replay->log)
            fprintf(replay->log, "GX_MULTITEX_PROGRAM_FAIL stage=multiply result=%08x\n",
                    (unsigned)result);
        release_multitex_programs(replay);
        return -1;
    }
    result = sceGxmShaderPatcherCreateFragmentProgram(
        patcher, replay->multitex_program_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE, &pe_alpha, &texture_v_gxp_start,
        &replay->pe_alpha_program);
    if (result < 0) {
        if (replay->log)
            fprintf(replay->log, "GX_PE_PROGRAM_FAIL stage=alpha result=%08x\n",
                    (unsigned)result);
        release_multitex_programs(replay);
        return -1;
    }
    result = sceGxmShaderPatcherCreateFragmentProgram(
        patcher, replay->multitex_program_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
        SCE_GXM_MULTISAMPLE_NONE, &pe_additive, &texture_v_gxp_start,
        &replay->pe_additive_program);
    if (result < 0) {
        if (replay->log)
            fprintf(replay->log, "GX_PE_PROGRAM_FAIL stage=additive result=%08x\n",
                    (unsigned)result);
        release_multitex_programs(replay);
        return -1;
    }
    replay->pe_ready = 1;
    return 0;
}

static int render_multitex_frame0(MvGxReplay *replay, const MvCamera *camera,
                                  const MvGxCaptureCommand *command,
                                  const MvGxCaptureVertex *vertices, uint32_t vertex_count)
{
    if (!replay || !camera || !command || !vertices ||
        replay->multitex_texture0 >= replay->texture_count ||
        replay->multitex_texture1 >= replay->texture_count)
        return -1;
    if (command->first_vertex > vertex_count ||
        command->vertex_count > vertex_count - command->first_vertex)
        return -1;

    MvReplayCameraBasis basis;
    if (camera_basis(camera, &basis)) return -1;
    float view_x, view_y, view_w, view_h;
    viewport(camera, &view_x, &view_y, &view_w, &view_h);
    const MvGxCaptureVertex *source = vertices + command->first_vertex;
    size_t capacity = (size_t)command->triangle_count * 9u;
    if (!capacity) return -1;

    vita2d_pool_reset();
    vita2d_texture_vertex *out0 = vita2d_pool_memalign(
        (unsigned)(capacity * sizeof(*out0)), sizeof(*out0));
    vita2d_texture_vertex *out1 = vita2d_pool_memalign(
        (unsigned)(capacity * sizeof(*out1)), sizeof(*out1));
    if (!out0 || !out1) return -1;
    MvGxReplayTexture *tex0 = &replay->textures[replay->multitex_texture0];
    MvGxReplayTexture *tex1 = &replay->textures[replay->multitex_texture1];
    size_t out_count = 0;
    uint32_t out_triangles = 0;
    for (uint32_t ti = 0; ti < command->triangle_count; ++ti) {
        uint32_t index[3];
        if (triangle_indices(command, ti, index)) return -1;
        for (unsigned k = 0; k < 3; ++k) {
            if ((source[index[k]].present & (1u << MV_GX_VA_PNMTXIDX)) &&
                source[index[k]].pos_mtx_idx != command->current_mtx)
                return -1;
        }
        MvReplayCameraVertex tri[3], near_clip[6], far_clip[8];
        for (unsigned k = 0; k < 3; ++k)
            tri[k] = camera_vertex(camera, &basis, command, &source[index[k]]);
        size_t clipped = clip_depth(tri, 3, near_clip, camera->near_z, 1);
        clipped = clip_depth(near_clip, clipped, far_clip, camera->far_z, 0);
        for (size_t k = 1; k + 1 < clipped; ++k) {
            const MvReplayCameraVertex *fan[3] = {
                &far_clip[0], &far_clip[k], &far_clip[k + 1]
            };
            if (out_count + 3 > capacity) return -1;
            float px[3], py[3], pz[3];
            for (unsigned n = 0; n < 3; ++n) {
                if (project(camera, fan[n], view_x, view_y, view_w, view_h,
                            &px[n], &py[n], &pz[n]))
                    return -1;
            }
            if (cull_projected_triangle(command->cull_mode,
                                        px[0], py[0], px[1], py[1], px[2], py[2])) {
                ++replay->pe_culled_triangles;
                continue;
            }
            for (unsigned n = 0; n < 3; ++n) {
                out0[out_count + n].x = out1[out_count + n].x = px[n];
                out0[out_count + n].y = out1[out_count + n].y = py[n];
                out0[out_count + n].z = out1[out_count + n].z = pz[n];
                out0[out_count + n].u = fan[n]->u * tex0->uv_scale_s;
                out0[out_count + n].v = fan[n]->v * tex0->uv_scale_t;
                out1[out_count + n].u = fan[n]->u1 * tex1->uv_scale_s;
                out1[out_count + n].v = fan[n]->v1 * tex1->uv_scale_t;
            }
            out_count += 3;
            ++out_triangles;
        }
    }
    if (!out_count) return -1;

    replay->multitex_target = vita2d_create_empty_texture_rendertarget(
        MV_GX_REPLAY_TARGET_WIDTH, MV_GX_REPLAY_TARGET_HEIGHT,
        SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!replay->multitex_target) return -1;
    vita2d_texture_set_filters(replay->multitex_target,
                               SCE_GXM_TEXTURE_FILTER_POINT,
                               SCE_GXM_TEXTURE_FILTER_POINT);
    replay->multitex_target_bytes =
        MV_GX_REPLAY_TARGET_WIDTH * MV_GX_REPLAY_TARGET_HEIGHT * 4u;

    unsigned old_clear = vita2d_get_clear_color();
    SceGxmFragmentProgram *saved_tint = _vita2d_textureTintFragmentProgram;
    vita2d_set_clear_color(RGBA8(0, 0, 0, 0));
    vita2d_start_drawing_advanced(replay->multitex_target, 0);
    SceGxmContext *context = vita2d_get_context();
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    vita2d_clear_screen();
    _vita2d_textureTintFragmentProgram = replay->multitex_replace_program;
    vita2d_draw_array_textured(tex0->texture, SCE_GXM_PRIMITIVE_TRIANGLES,
                               out0, out_count, tint_color(&command->material));
    _vita2d_textureTintFragmentProgram = replay->multitex_multiply_program;
    vita2d_draw_array_textured(tex1->texture, SCE_GXM_PRIMITIVE_TRIANGLES,
                               out1, out_count, RGBA8(255, 255, 255, 255));
    _vita2d_textureTintFragmentProgram = saved_tint;
    vita2d_end_drawing();
    vita2d_set_clear_color(old_clear);
    vita2d_wait_rendering_done();

    replay->multitex_input_triangles = command->triangle_count;
    replay->multitex_output_triangles = out_triangles;
    replay->multitex_ready = 1;
    return 0;
}

static int mv_gx_replay_init_internal(MvGxReplay *replay, const MvCamera *camera, FILE *log,
                                      uint32_t relaxed_from_command)
{
    if (!replay || !camera) return -1;
    memset(replay, 0, sizeof(*replay));
    replay->log = log;
    replay->relaxed_from_command = relaxed_from_command;
    replay->multitex_command = UINT32_MAX;
    replay->multitex_texture0 = UINT32_MAX;
    replay->multitex_texture1 = UINT32_MAX;
    uint32_t command_count, vertex_count;
    const MvGxCaptureCommand *commands = mv_gx_capture_commands(&command_count);
    const MvGxCaptureVertex *vertices = mv_gx_capture_vertices(&vertex_count);
    if (!commands || !vertices || !command_count || !vertex_count) return -1;

    for (uint32_t i = 0; i < command_count; ++i) {
        const MvGxCaptureCommand *command = &commands[i];
        if (!command_supported(command, i >= replay->relaxed_from_command) ||
            !command->material.texture_count) continue;
        if (command->material.pe_dst_factor == 5u) ++replay->pe_alpha_commands;
        else if (command->material.pe_dst_factor == 1u) ++replay->pe_additive_commands;
        if (command->cull_mode == MV_GX_CULL_NONE) ++replay->pe_cull_none_commands;
        else if (command->cull_mode == MV_GX_CULL_BACK) ++replay->pe_cull_back_commands;
        if (uv_matrix_nonidentity(&command->material)) ++replay->uv_matrix_commands;
        if (command->material.wrap_s == 1 || command->material.wrap_t == 1)
            ++replay->wrap_repeat_commands;
        if (command->material.wrap_s == 2 || command->material.wrap_t == 2)
            ++replay->wrap_mirror_commands;
        if (command->material.wrap_s == 0 && command->material.wrap_t == 0)
            ++replay->wrap_clamp_commands;
        MvGxReplayTexture *entry = prepare_texture(replay, &command->material, i);
        if (!entry) continue;
        if (command->material.texture_count == 2 &&
            mv_gx_material_multitex_offscreen_bakeable(&command->material)) {
            if (replay->multitex_command != UINT32_MAX) {
                if (log) fprintf(log, "GX_MULTITEX_INIT_FAIL reason=multiple_commands\n");
                ++replay->texture_failures;
                continue;
            }
            replay->multitex_command = i;
            replay->multitex_texture0 = (uint32_t)(entry - replay->textures);
            MvGxMaterialState second;
            second_layer_material(&command->material, &second);
            MvGxReplayTexture *entry1 = prepare_texture(replay, &second, i);
            if (!entry1) continue;
            replay->multitex_texture1 = (uint32_t)(entry1 - replay->textures);
        }
    }
    /* PE blend programs are required by every textured replay command, not
     * only by the optional two-texture path. The old viewer always happened
     * to contain one multitexture command, masking this dependency; GmTtAll
     * does not, so pe_ready stayed false and rejected a valid title capture. */
    if (replay->texture_count != 0 && replay->texture_failures == 0) {
        if (init_multitex_programs(replay)) {
            if (log) fprintf(log, "GX_PE_PROGRAM_FAIL stage=init backend=GXM\n");
        }
    }
    if (replay->multitex_command != UINT32_MAX && replay->texture_failures == 0 &&
        replay->pe_ready) {
        const MvGxCaptureCommand *command = &commands[replay->multitex_command];
        if (render_multitex_frame0(replay, camera, command, vertices, vertex_count)) {
            if (log) fprintf(log, "GX_MULTITEX_INIT_FAIL command=%u backend=GXMOffscreen\n",
                             replay->multitex_command);
            replay->multitex_ready = 0;
        }
    }
    replay->ready = replay->texture_failures == 0 && replay->texture_count != 0 &&
                    replay->pe_ready &&
                    (replay->multitex_command == UINT32_MAX || replay->multitex_ready);
    if (log) {
        fprintf(log, "HSD_GX_REPLAY_READY_%s textures=%u bytes=%u texture_failures=%u "
                     "uv_matrix_commands=%u uv=HSD_TObj_MakeTextureMtx "
                     "wrap_repeat=%u wrap_mirror=%u wrap_clamp=%u mirror_baked_textures=%u "
                     "sampler=GX_to_GXM_or_mirror_bake "
                     "mapmode=HSD_TObj_TExp_CPU_baked backend=Vita2DCommandReplay "
                     "custom_tev_baked=%u custom_tev_noop=%u "
                     "custom_tev=MenMainBack_single_stage_CPU_baked "
                     "multitex=%s multitex_command=%u multitex_triangles=%u/%u "
                     "multitex_target_bytes=%u multitex_backend=GXM_offscreen_replace_multiply "
                     "pe=PASS blend_alpha=%u blend_additive=%u z=LEQUAL z_write=0 "
                     "cull_none=%u cull_back=%u cull=GX_clockwise_CPU depth=GXM_LEQUAL_neutral_z0\n",
                replay->ready ? "PASS" : "FAIL", replay->texture_count,
                replay->texture_bytes, replay->texture_failures, replay->uv_matrix_commands,
                replay->wrap_repeat_commands, replay->wrap_mirror_commands,
                replay->wrap_clamp_commands, replay->mirror_baked_textures,
                replay->custom_tev_baked_commands, replay->custom_tev_noop_commands,
                replay->multitex_ready ? "PASS" :
                    (replay->multitex_command == UINT32_MAX ? "none" : "FAIL"),
                replay->multitex_command,
                replay->multitex_input_triangles, replay->multitex_output_triangles,
                replay->multitex_target_bytes,
                replay->pe_alpha_commands, replay->pe_additive_commands,
                replay->pe_cull_none_commands, replay->pe_cull_back_commands);
        fflush(log);
    }
    return replay->ready ? 0 : -1;
}

int mv_gx_replay_init(MvGxReplay *replay, const MvCamera *camera, FILE *log)
{
    return mv_gx_replay_init_internal(replay, camera, log, UINT32_MAX);
}

int mv_gx_replay_init_relaxed_from(MvGxReplay *replay, const MvCamera *camera, FILE *log,
                                   uint32_t relaxed_from_command)
{
    return mv_gx_replay_init_internal(replay, camera, log, relaxed_from_command);
}

void mv_gx_replay_draw(MvGxReplay *replay, const MvCamera *camera)
{
    if (!replay || !replay->ready || !camera) return;
    MvReplayCameraBasis basis;
    if (camera_basis(camera, &basis)) return;
    float view_x, view_y, view_w, view_h;
    viewport(camera, &view_x, &view_y, &view_w, &view_h);
    vita2d_draw_rectangle(view_x, view_y, view_w, view_h, RGBA8(4, 7, 11, 255));
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle((int)view_x, (int)view_y,
                              (int)(view_x + view_w), (int)(view_y + view_h));
    SceGxmContext *context = vita2d_get_context();
    SceGxmFragmentProgram *saved_tint = _vita2d_textureTintFragmentProgram;
    /* MenMainBack's effective PE state is LEQUAL with depth writes disabled
     * for every draw.  CPU winding handles the original GX cull semantics;
     * keep GXM hardware culling off so no second convention is applied. */
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_LESS_EQUAL);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_LESS_EQUAL);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);

    uint32_t command_count, vertex_count;
    const MvGxCaptureCommand *commands = mv_gx_capture_commands(&command_count);
    const MvGxCaptureVertex *vertices = mv_gx_capture_vertices(&vertex_count);
    uint32_t submitted_commands = 0, input_triangles = 0, output_triangles = 0;
    uint32_t skipped_matrix = 0;
    uint32_t culled_triangles = replay->pe_culled_triangles;
    for (uint32_t ci = 0; ci < command_count; ++ci) {
        const MvGxCaptureCommand *command = &commands[ci];
        if (!command_supported(command, ci >= replay->relaxed_from_command) ||
            command->first_vertex > vertex_count ||
            command->vertex_count > vertex_count - command->first_vertex) continue;
        const MvGxCaptureVertex *source = vertices + command->first_vertex;
        _vita2d_textureTintFragmentProgram =
            command->material.pe_dst_factor == 1u ? replay->pe_additive_program :
                                                     replay->pe_alpha_program;
        if (command->material.texture_count == 2 &&
            mv_gx_material_multitex_offscreen_bakeable(&command->material)) {
            if (replay->multitex_ready && ci == replay->multitex_command &&
                replay->multitex_target) {
                input_triangles += command->triangle_count;
                output_triangles += replay->multitex_output_triangles;
                vita2d_draw_texture_tint(replay->multitex_target, 0.0f, 0.0f,
                                         RGBA8(255, 255, 255, 255));
                ++submitted_commands;
            }
            continue;
        }
        vita2d_texture *texture = NULL;
        MvGxReplayTexture *texture_entry = NULL;
        if (command->material.texture_count) {
            texture_entry = find_texture(replay, &command->material);
            if (!texture_entry || !texture_entry->texture) continue;
            texture = texture_entry->texture;
        }
        size_t capacity = (size_t)command->triangle_count * 9u;
        if (!capacity) continue;
        input_triangles += command->triangle_count;
        if (texture) {
            vita2d_texture_vertex *out = vita2d_pool_malloc((unsigned)(capacity * sizeof(*out)));
            if (!out) continue;
            size_t out_count = 0;
            for (uint32_t ti = 0; ti < command->triangle_count; ++ti) {
                uint32_t index[3];
                if (triangle_indices(command, ti, index)) continue;
                int matrix_ok = 1;
                for (unsigned k = 0; k < 3; ++k) {
                    if ((source[index[k]].present & (1u << MV_GX_VA_PNMTXIDX)) &&
                        source[index[k]].pos_mtx_idx != command->current_mtx) matrix_ok = 0;
                }
                if (!matrix_ok) { ++skipped_matrix; continue; }
                MvReplayCameraVertex tri[3], near_clip[6], far_clip[8];
                for (unsigned k = 0; k < 3; ++k)
                    tri[k] = camera_vertex(camera, &basis, command, &source[index[k]]);
                size_t clipped = clip_depth(tri, 3, near_clip, camera->near_z, 1);
                clipped = clip_depth(near_clip, clipped, far_clip, camera->far_z, 0);
                for (size_t k = 1; k + 1 < clipped; ++k) {
                    const MvReplayCameraVertex *fan[3] = {&far_clip[0], &far_clip[k], &far_clip[k + 1]};
                    if (out_count + 3 > capacity) break;
                    int valid = 1;
                    for (unsigned n = 0; n < 3; ++n) {
                        vita2d_texture_vertex *dst = &out[out_count + n];
                        if (project(camera, fan[n], view_x, view_y, view_w, view_h,
                                    &dst->x, &dst->y, &dst->z)) { valid = 0; break; }
                        dst->u = fan[n]->u * texture_entry->uv_scale_s;
                        dst->v = fan[n]->v * texture_entry->uv_scale_t;
                    }
                    if (valid) {
                        const vita2d_texture_vertex *a = &out[out_count];
                        const vita2d_texture_vertex *b = &out[out_count + 1];
                        const vita2d_texture_vertex *c = &out[out_count + 2];
                        if (cull_projected_triangle(command->cull_mode,
                                                    a->x, a->y, b->x, b->y, c->x, c->y)) {
                            ++culled_triangles;
                            continue;
                        }
                        out_count += 3;
                        ++output_triangles;
                    }
                }
            }
            if (out_count) {
                vita2d_draw_array_textured(texture, SCE_GXM_PRIMITIVE_TRIANGLES, out,
                                           out_count, tint_color(&command->material));
                ++submitted_commands;
            }
        } else {
            vita2d_color_vertex *out = vita2d_pool_malloc((unsigned)(capacity * sizeof(*out)));
            if (!out) continue;
            size_t out_count = 0;
            for (uint32_t ti = 0; ti < command->triangle_count; ++ti) {
                uint32_t index[3];
                if (triangle_indices(command, ti, index)) continue;
                MvReplayCameraVertex tri[3], near_clip[6], far_clip[8];
                for (unsigned k = 0; k < 3; ++k)
                    tri[k] = camera_vertex(camera, &basis, command, &source[index[k]]);
                size_t clipped = clip_depth(tri, 3, near_clip, camera->near_z, 1);
                clipped = clip_depth(near_clip, clipped, far_clip, camera->far_z, 0);
                for (size_t k = 1; k + 1 < clipped; ++k) {
                    const MvReplayCameraVertex *fan[3] = {&far_clip[0], &far_clip[k], &far_clip[k + 1]};
                    if (out_count + 3 > capacity) break;
                    int valid = 1;
                    for (unsigned n = 0; n < 3; ++n) {
                        vita2d_color_vertex *dst = &out[out_count + n];
                        if (project(camera, fan[n], view_x, view_y, view_w, view_h,
                                    &dst->x, &dst->y, &dst->z)) { valid = 0; break; }
                        uint32_t rgba = fan[n]->rgba;
                        dst->color = RGBA8(rgba >> 24, rgba >> 16, rgba >> 8, rgba);
                    }
                    if (valid) {
                        const vita2d_color_vertex *a = &out[out_count];
                        const vita2d_color_vertex *b = &out[out_count + 1];
                        const vita2d_color_vertex *c = &out[out_count + 2];
                        if (cull_projected_triangle(command->cull_mode,
                                                    a->x, a->y, b->x, b->y, c->x, c->y)) {
                            ++culled_triangles;
                            continue;
                        }
                        out_count += 3;
                        ++output_triangles;
                    }
                }
            }
            if (out_count) {
                vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, out, out_count);
                ++submitted_commands;
            }
        }
    }
    _vita2d_textureTintFragmentProgram = saved_tint;
    sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
    vita2d_disable_clipping();

    replay->submitted_commands = submitted_commands;
    replay->input_triangles = input_triangles;
    replay->output_triangles = output_triangles;
    replay->skipped_matrix_index = skipped_matrix;
    if (!replay->submit_logged && replay->log) {
        fprintf(replay->log,
                "HSD_GX_REPLAY_SUBMIT_PASS commands=%u input_triangles=%u output_triangles=%u "
                "textures=%u skipped_matrix=%u backend=Vita2DCommandReplay "
                "model_mtx=rigid_HSD_SRT uv_mtx=HSD_TObj camera=original_HSD "
                "depth=GXM_LEQUAL_no_write_neutral_z0 cull=GX_clockwise_CPU "
                "culled_triangles=%u pe=HSD_PE blend=alpha_or_SRCALPHA_ONE alpha_write=off\n",
                submitted_commands, input_triangles, output_triangles, replay->texture_count,
                skipped_matrix, culled_triangles);
        fflush(replay->log);
        replay->submit_logged = 1;
    }
}

void mv_gx_replay_close(MvGxReplay *replay)
{
    if (!replay) return;
    vita2d_wait_rendering_done();
    if (replay->multitex_target) {
        vita2d_free_texture(replay->multitex_target);
        replay->multitex_target = NULL;
    }
    release_multitex_programs(replay);
    for (uint32_t i = 0; i < replay->texture_count; ++i) {
        if (replay->textures[i].texture) vita2d_free_texture(replay->textures[i].texture);
        replay->textures[i].texture = NULL;
    }
    replay->texture_count = 0;
    replay->multitex_ready = 0;
    replay->ready = 0;
}
