#include "gx_capture_vita.h"

#include <dolphin/gx.h>
#include <dolphin/gx/GXCommandList.h>
#include <math.h>
#include <string.h>

#define MV_CAPTURE_MAX_COMMANDS 2048u
#define MV_CAPTURE_MAX_VERTICES 32768u
#define MV_CAPTURE_MATRIX_SLOTS 10u

typedef struct {
    const uint8_t *base;
    uint8_t stride;
} MvArrayState;

typedef struct {
    GXCompCnt count;
    GXCompType type;
    uint8_t frac;
    uint8_t valid;
} MvAttrFormat;

static GXAttrType attr_types[GX_VA_MAX_ATTR];
static MvArrayState arrays[GX_VA_MAX_ATTR];
static MvAttrFormat formats[GX_MAX_VTXFMT][GX_VA_MAX_ATTR];
static MvGxCaptureCommand commands[MV_CAPTURE_MAX_COMMANDS];
static MvGxCaptureVertex vertices[MV_CAPTURE_MAX_VERTICES];
static float pos_mtx[MV_CAPTURE_MATRIX_SLOTS][3][4];
static float nrm_mtx[MV_CAPTURE_MATRIX_SLOTS][3][4];
static float tex_mtx[MV_CAPTURE_MATRIX_SLOTS][3][4];
static uint32_t current_mtx, cull_mode;
static MvGxCaptureStats stats;
static MvGxMaterialState material_state;
static float capture_projection[4][4];
static float capture_viewport[6];
static uint32_t capture_projection_type;
static uint8_t capture_projection_valid;
static uint8_t capture_viewport_valid;

typedef struct {
    uint8_t active;
    uint8_t primitive;
    uint8_t vtxfmt;
    uint8_t reserved;
    uint16_t expected_vertices;
    uint16_t emitted_vertices;
    uint16_t vertex_bytes;
    uint16_t buffered_bytes;
    uint8_t buffer[256];
} MvImmediateState;

static MvImmediateState immediate;

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static float read_be_float(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                    (uint32_t)p[2] << 8 | p[3];
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int component_size(GXCompType type)
{
    switch (type) {
    case GX_U8:
    case GX_S8:
        return 1;
    case GX_U16:
    case GX_S16:
        return 2;
    case GX_F32:
        return 4;
    default:
        return 0;
    }
}

static size_t color_size(GXCompType type)
{
    static const uint8_t sizes[] = {2, 3, 4, 2, 3, 4};
    unsigned index = (unsigned)type;
    return index < sizeof(sizes) ? sizes[index] : 0;
}

static size_t element_size(GXAttr attr, const MvAttrFormat *fmt)
{
    if (attr <= GX_VA_TEX7MTXIDX) return 1;
    if (!fmt->valid) return 0;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) return color_size(fmt->type);
    int bytes = component_size(fmt->type);
    if (!bytes) return 0;
    unsigned components;
    if (attr == GX_VA_POS) components = fmt->count == GX_POS_XYZ ? 3 : 2;
    else if (attr == GX_VA_NRM || attr == GX_VA_NBT)
        components = fmt->count == GX_NRM_XYZ ? 3 : 9;
    else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7)
        components = fmt->count == GX_TEX_ST ? 2 : 1;
    else
        return 0;
    return (size_t)bytes * components;
}

static int read_scalar(const uint8_t *p, GXCompType type, uint8_t frac, float *out)
{
    float scale = frac < 31 ? 1.0f / (float)(1u << frac) : 0.0f;
    switch (type) {
    case GX_U8: *out = (float)p[0] * scale; break;
    case GX_S8: *out = (float)(int8_t)p[0] * scale; break;
    case GX_U16: *out = (float)read_be16(p) * scale; break;
    case GX_S16: *out = (float)(int16_t)read_be16(p) * scale; break;
    case GX_F32: *out = read_be_float(p); break;
    default: return -1;
    }
    return isfinite(*out) ? 0 : -1;
}

static uint8_t expand4(unsigned value) { return (uint8_t)((value << 4) | value); }
static uint8_t expand5(unsigned value) { return (uint8_t)((value << 3) | (value >> 2)); }
static uint8_t expand6(unsigned value) { return (uint8_t)((value << 2) | (value >> 4)); }
static uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a)
{
    return (r << 24) | (g << 16) | (b << 8) | a;
}

static int read_color(const uint8_t *p, GXCompType type, uint32_t *out)
{
    switch (type) {
    case GX_RGB565: {
        uint16_t value = read_be16(p);
        *out = rgba(expand5(value >> 11), expand6((value >> 5) & 63),
                    expand5(value & 31), 255);
        return 0;
    }
    case GX_RGB8:
    case GX_RGBX8:
        *out = rgba(p[0], p[1], p[2], 255);
        return 0;
    case GX_RGBA4: {
        uint16_t value = read_be16(p);
        *out = rgba(expand4(value >> 12), expand4((value >> 8) & 15),
                    expand4((value >> 4) & 15), expand4(value & 15));
        return 0;
    }
    case GX_RGBA6: {
        uint32_t value = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        *out = rgba(expand6(value >> 18), expand6((value >> 12) & 63),
                    expand6((value >> 6) & 63), expand6(value & 63));
        return 0;
    }
    case GX_RGBA8:
        *out = rgba(p[0], p[1], p[2], p[3]);
        return 0;
    default:
        return -1;
    }
}

static uint32_t primitive_triangles(uint8_t primitive, uint16_t count)
{
    switch (primitive) {
    case GX_QUADS: return count % 4 == 0 ? (uint32_t)(count / 4) * 2u : UINT32_MAX;
    case GX_TRIANGLES: return count % 3 == 0 ? count / 3u : UINT32_MAX;
    case GX_TRIANGLESTRIP:
    case GX_TRIANGLEFAN: return count >= 3 ? count - 2u : 0;
    case GX_LINES:
    case GX_LINESTRIP:
    case GX_POINTS: return 0;
    default: return UINT32_MAX;
    }
}

static int decode_vertex(const uint8_t **cursor, const uint8_t *end, GXVtxFmt vtxfmt,
                         MvGxCaptureVertex *out, uint32_t *attr_mask)
{
    memset(out, 0, sizeof(*out));
    out->color0 = 0xffffffffu;
    out->pos_mtx_idx = (uint8_t)current_mtx;
    for (unsigned attr_index = GX_VA_PNMTXIDX; attr_index <= GX_VA_TEX7; ++attr_index) {
        GXAttr attr = (GXAttr)attr_index;
        GXAttrType attr_type = attr_types[attr_index];
        if (attr_type == GX_NONE) continue;
        const MvAttrFormat *fmt = &formats[vtxfmt][attr_index];
        size_t bytes = element_size(attr, fmt);
        if (!bytes) return -1;
        const uint8_t *source = NULL;
        if (attr_type == GX_DIRECT) {
            if ((size_t)(end - *cursor) < bytes) return -1;
            source = *cursor;
            *cursor += bytes;
        } else if (attr_type == GX_INDEX8 || attr_type == GX_INDEX16) {
            size_t index_bytes = attr_type == GX_INDEX8 ? 1u : 2u;
            unsigned index_count = (attr == GX_VA_NRM && fmt->count == GX_NRM_NBT3) ? 3u : 1u;
            if ((size_t)(end - *cursor) < index_bytes * index_count) return -1;
            /* NBT3 carries three independent array indices. Preserve the
             * fail-closed boundary until the command vertex stores and
             * replays those independently instead of treating them as one. */
            if (index_count == 3u) return -1;
            uint32_t index = index_bytes == 1 ? (*cursor)[0] : read_be16(*cursor);
            *cursor += index_bytes * index_count;
            if (!arrays[attr_index].base || !arrays[attr_index].stride) return -1;
            source = arrays[attr_index].base + (size_t)index * arrays[attr_index].stride;
        } else {
            return -1;
        }
        if (attr_index < 32) *attr_mask |= 1u << attr_index;
        if (attr <= GX_VA_TEX7MTXIDX) {
            if (attr == GX_VA_PNMTXIDX) out->pos_mtx_idx = source[0];
            continue;
        }
        if (attr == GX_VA_POS) {
            int scalar = component_size(fmt->type);
            if (!scalar || read_scalar(source, fmt->type, fmt->frac, &out->position[0]) ||
                read_scalar(source + scalar, fmt->type, fmt->frac, &out->position[1])) return -1;
            if (fmt->count == GX_POS_XYZ &&
                read_scalar(source + scalar * 2, fmt->type, fmt->frac, &out->position[2])) return -1;
            out->present |= 1u;
        } else if (attr == GX_VA_NRM || attr == GX_VA_NBT) {
            int scalar = component_size(fmt->type);
            unsigned components = fmt->count == GX_NRM_XYZ ? 3u : 9u;
            if (!scalar) return -1;
            for (unsigned component = 0; component < components; ++component)
                if (read_scalar(source + scalar * component, fmt->type, fmt->frac,
                                &out->normal[component])) return -1;
            out->present |= 2u;
        } else if (attr == GX_VA_CLR0) {
            if (read_color(source, fmt->type, &out->color0)) return -1;
            out->present |= 4u;
        } else if (attr == GX_VA_TEX0) {
            int scalar = component_size(fmt->type);
            if (!scalar || read_scalar(source, fmt->type, fmt->frac, &out->tex0[0])) return -1;
            if (fmt->count == GX_TEX_ST &&
                read_scalar(source + scalar, fmt->type, fmt->frac, &out->tex0[1])) return -1;
            out->present |= 8u;
        }
    }
    return (out->present & 1u) ? 0 : -1;
}

void mv_gx_capture_set_projection(const float matrix[4][4], uint32_t type)
{
    if (!matrix) { capture_projection_valid = 0; return; }
    memcpy(capture_projection, matrix, sizeof(capture_projection));
    capture_projection_type = type;
    capture_projection_valid = 1;
}

void mv_gx_capture_set_viewport(float x, float y, float w, float h,
                                float near_z, float far_z)
{
    capture_viewport[0] = x; capture_viewport[1] = y;
    capture_viewport[2] = w; capture_viewport[3] = h;
    capture_viewport[4] = near_z; capture_viewport[5] = far_z;
    capture_viewport_valid = w > 0.0f && h > 0.0f;
}

void mv_gx_capture_reset(void)
{
    memset(&stats, 0, sizeof(stats));
    memset(commands, 0, sizeof(commands));
    memset(vertices, 0, sizeof(vertices));
    memset(attr_types, 0, sizeof(attr_types));
    memset(arrays, 0, sizeof(arrays));
    memset(formats, 0, sizeof(formats));
    memset(pos_mtx, 0, sizeof(pos_mtx));
    memset(nrm_mtx, 0, sizeof(nrm_mtx));
    memset(tex_mtx, 0, sizeof(tex_mtx));
    memset(&material_state, 0, sizeof(material_state));
    memset(capture_projection, 0, sizeof(capture_projection));
    memset(capture_viewport, 0, sizeof(capture_viewport));
    capture_projection_type = 0;
    capture_projection_valid = 0;
    capture_viewport_valid = 0;
    memset(&immediate, 0, sizeof(immediate));
    material_state.material_rgba = 0xffffffffu;
    current_mtx = GX_PNMTX0;
    cull_mode = GX_CULL_NONE;
}

int mv_gx_capture_stats(MvGxCaptureStats *out)
{
    if (!out) return -1;
    *out = stats;
    return stats.errors ? -2 : 0;
}

int mv_gx_capture_world_bounds(float out[6])
{
    if (!out || !stats.commands || !stats.vertices || stats.errors) return -1;
    int have = 0;
    for (uint32_t ci = 0; ci < stats.commands; ++ci) {
        const MvGxCaptureCommand *command = &commands[ci];
        if (command->first_vertex > stats.vertices ||
            command->vertex_count > stats.vertices - command->first_vertex) return -1;
        for (uint32_t vi = 0; vi < command->vertex_count; ++vi) {
            const MvGxCaptureVertex *v = &vertices[command->first_vertex + vi];
            float x = command->pos_mtx[0][0] * v->position[0] +
                      command->pos_mtx[0][1] * v->position[1] +
                      command->pos_mtx[0][2] * v->position[2] + command->pos_mtx[0][3];
            float y = command->pos_mtx[1][0] * v->position[0] +
                      command->pos_mtx[1][1] * v->position[1] +
                      command->pos_mtx[1][2] * v->position[2] + command->pos_mtx[1][3];
            float z = command->pos_mtx[2][0] * v->position[0] +
                      command->pos_mtx[2][1] * v->position[1] +
                      command->pos_mtx[2][2] * v->position[2] + command->pos_mtx[2][3];
            if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return -1;
            if (!have) {
                out[0] = out[3] = x; out[1] = out[4] = y; out[2] = out[5] = z;
                have = 1;
            } else {
                if (x < out[0]) out[0] = x; if (x > out[3]) out[3] = x;
                if (y < out[1]) out[1] = y; if (y > out[4]) out[4] = y;
                if (z < out[2]) out[2] = z; if (z > out[5]) out[5] = z;
            }
        }
    }
    return have ? 0 : -1;
}

void mv_gx_capture_set_material(const MvGxMaterialState *material)
{
    if (material) material_state = *material;
    else {
        memset(&material_state, 0, sizeof(material_state));
        material_state.material_rgba = 0xffffffffu;
    }
}

MvGxMaterialState *mv_gx_capture_material_state(void)
{
    return &material_state;
}

const MvGxCaptureCommand *mv_gx_capture_commands(uint32_t *count)
{
    if (count) *count = stats.commands;
    return commands;
}

const MvGxCaptureVertex *mv_gx_capture_vertices(uint32_t *count)
{
    if (count) *count = stats.vertices;
    return vertices;
}

int mv_gx_material_custom_tev_cpu_bakeable(const MvGxMaterialState *material)
{
    if (!material || !material->tev_valid) return 1;
    const uint32_t active = material->tev_active;
    const int color_active = (active & 0x40000000u) != 0;
    const int alpha_active = (active & 0x80000000u) != 0;
    if (!color_active && !alpha_active) return 1;

    const uint8_t *op = material->tev_op;
    /* All custom single-texture stages in MnSlChr use HSD's generated
     * TEV0/KONST interpolation graph.  Validate every field we evaluate so
     * unrelated gameplay TEV graphs remain fail-closed. */
    if (color_active) {
        if (op[0] != GX_TEV_ADD || op[2] != GX_TB_ZERO ||
            op[4] != GX_CS_SCALE_1 || op[6] != GX_ENABLE ||
            op[8] != 0x85 || op[9] != 0x80 ||
            op[10] != GX_CC_TEXC || op[11] != GX_CC_ZERO)
            return 0;
    }
    if (alpha_active) {
        if (op[1] != GX_TEV_ADD || op[3] != GX_TB_ZERO ||
            op[5] > GX_CS_DIVIDE_2 || op[7] != GX_ENABLE ||
            op[12] != 0x44 || op[13] != 0x43 ||
            op[14] != GX_CA_TEXA || op[15] != GX_CA_ZERO)
            return 0;
    }
    return 1;
}

static int uv_mtx_identity(const float m[2][3], uint8_t valid)
{
    static const float expected[2][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
    };
    /* HSD only materializes a texture matrix when it differs from the default.
     * No captured matrix therefore means the identity transform. */
    if (!valid) return 1;
    for (unsigned r = 0; r < 2; ++r)
        for (unsigned c = 0; c < 3; ++c)
            if (fabsf(m[r][c] - expected[r][c]) > 1.0e-6f) return 0;
    return 1;
}

static int uv_mtx_finite(const float m[2][3], uint8_t valid)
{
    if (!valid) return 1;
    for (unsigned r = 0; r < 2; ++r)
        for (unsigned c = 0; c < 3; ++c)
            if (!isfinite(m[r][c])) return 0;
    return 1;
}

int mv_gx_material_multitex_vitagl_supported(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 2 || !m->image || !m->image1 ||
        !m->width || !m->height || !m->width1 || !m->height1)
        return 0;
    if ((m->unsupported & ~MV_GX_MATERIAL_UNSUPPORTED_MULTITEX) != 0)
        return 0;
    if ((m->tobj_flags & 0x0fu) != 0 || (m->tobj1_flags & 0x0fu) != 0)
        return 0;
    if (((m->tobj_flags >> 16) & 0x0fu) != 4u ||
        ((m->tobj_flags >> 20) & 0x0fu) != 3u ||
        ((m->tobj1_flags >> 16) & 0x0fu) != 4u ||
        (((m->tobj1_flags >> 20) & 0x0fu) != 0u &&
         ((m->tobj1_flags >> 20) & 0x0fu) != 3u))
        return 0;
    if ((m->tobj_flags & (1u << 24)) || (m->tobj1_flags & (1u << 24)))
        return 0;
    if (fabsf(m->blending - 1.0f) > 1.0e-6f ||
        fabsf(m->blending1 - 1.0f) > 1.0e-6f)
        return 0;
    if (!uv_mtx_finite(m->uv_mtx, m->uv_mtx_valid) ||
        !uv_mtx_finite(m->uv_mtx1, m->uv_mtx1_valid))
        return 0;
    if (m->wrap_s > GX_MIRROR || m->wrap_t > GX_MIRROR ||
        m->wrap_s1 > GX_MIRROR || m->wrap_t1 > GX_MIRROR ||
        m->mag_filter != GX_LINEAR || m->mag_filter1 != GX_LINEAR)
        return 0;
    if ((m->tev_valid && m->tev_active) || (m->tev1_valid && m->tev1_active))
        return 0;
    return 1;
}

int mv_gx_material_multitex_offscreen_bakeable(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 2 || !m->image || !m->image1 ||
        !m->width || !m->height || !m->width1 || !m->height1)
        return 0;

    /* HSD's common two-layer MODULATE graph is used by MenMainBack and by
     * almost the entire GmTtAll title background.  Independent bilinear
     * samples stay separate; the replay combines both texture units at draw
     * time rather than baking them into one image. */
    if ((m->unsupported & ~MV_GX_MATERIAL_UNSUPPORTED_MULTITEX) != 0) return 0;
    if ((m->tobj_flags & 0x0fu) != 0 || (m->tobj1_flags & 0x0fu) != 0) return 0;
    if (((m->tobj_flags >> 16) & 0x0fu) != 4u ||
        ((m->tobj_flags >> 20) & 0x0fu) != 3u ||
        ((m->tobj1_flags >> 16) & 0x0fu) != 4u ||
        (((m->tobj1_flags >> 20) & 0x0fu) != 0u &&
         ((m->tobj1_flags >> 20) & 0x0fu) != 3u))
        return 0;
    if ((m->tobj_flags & (1u << 24)) || (m->tobj1_flags & (1u << 24))) return 0;
    if (fabsf(m->blending - 1.0f) > 1.0e-6f ||
        fabsf(m->blending1 - 1.0f) > 1.0e-6f)
        return 0;
    if (!uv_mtx_identity(m->uv_mtx, m->uv_mtx_valid) ||
        !uv_mtx_identity(m->uv_mtx1, m->uv_mtx1_valid))
        return 0;
    if (m->wrap_s > GX_MIRROR || m->wrap_t > GX_MIRROR ||
        m->wrap_s1 > GX_MIRROR || m->wrap_t1 > GX_MIRROR)
        return 0;
    if (m->mag_filter != GX_LINEAR || m->mag_filter1 != GX_LINEAR) return 0;
    if ((m->tev_valid && m->tev_active) || (m->tev1_valid && m->tev1_active)) return 0;
    return 1;
}

int mv_gx_capture_replay_classify(MvGxReplayClassStats *out)
{
    if (!out || stats.errors) return -1;
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < stats.commands; ++i) {
        const MvGxCaptureCommand *command = &commands[i];
        const uint32_t unsupported = command->material.unsupported;
        if (unsupported & MV_GX_MATERIAL_UNSUPPORTED_MULTITEX) {
            if (!mv_gx_material_multitex_offscreen_bakeable(&command->material)) {
                ++out->skipped_multitex;
                continue;
            }
        }
        if ((unsupported & MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV) &&
            !mv_gx_material_custom_tev_cpu_bakeable(&command->material)) {
            ++out->skipped_custom_tev;
            continue;
        }
        if (unsupported & (MV_GX_MATERIAL_UNSUPPORTED_TEXCOORD |
                           MV_GX_MATERIAL_UNSUPPORTED_BUMP)) {
            ++out->skipped_texcoord;
            continue;
        }
        /* Single-texture colormap/alphamap modes are resolved by the replay
         * texture baker. Keep the bits in the captured state as provenance,
         * but they no longer make the command unsupported by themselves. */
        /* vita2d's textured helper has one uniform tint rather than a color
         * attribute. Keep textured GX vertex colors out of the first replay
         * bridge instead of silently flattening them. GX_VA_CLR0=11. */
        if (command->material.texture_count && (command->attr_mask & (1u << 11))) {
            ++out->skipped_vertex_color;
            continue;
        }
        ++out->supported_commands;
        out->supported_triangles += command->triangle_count;
        if (command->material.texture_count) ++out->textured_commands;
        else ++out->untextured_commands;
    }
    return 0;
}

int mv_gx_capture_pe_stats(MvGxPeStats *out)
{
    if (!out || stats.errors) return -1;
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < stats.commands; ++i) {
        const MvGxCaptureCommand *command = &commands[i];
        const MvGxMaterialState *m = &command->material;
        ++out->commands;
        if (m->pe_blend_type == GX_BM_BLEND &&
            m->pe_src_factor == GX_BL_SRCALPHA &&
            m->pe_dst_factor == GX_BL_INVSRCALPHA)
            ++out->blend_alpha;
        else if (m->pe_blend_type == GX_BM_BLEND &&
                 m->pe_src_factor == GX_BL_SRCALPHA &&
                 m->pe_dst_factor == GX_BL_ONE)
            ++out->blend_additive;
        else
            ++out->blend_other;
        if (m->pe_z_enable && m->pe_z_func == GX_LEQUAL) ++out->z_lequal;
        if (m->pe_z_update) ++out->z_write;
        if (m->pe_alpha_comp0 == GX_ALWAYS && m->pe_alpha_comp1 == GX_ALWAYS)
            ++out->alpha_compare_always;
        if (m->pe_custom) ++out->custom_pe;
        switch (command->cull_mode) {
        case GX_CULL_NONE: ++out->cull_none; break;
        case GX_CULL_FRONT: ++out->cull_front; break;
        case GX_CULL_BACK: ++out->cull_back; break;
        case GX_CULL_ALL: ++out->cull_all; break;
        default: return -1;
        }
    }
    return 0;
}

uint32_t mv_gx_capture_custom_tev_records(uint32_t *out, uint32_t max_records)
{
    uint32_t count = 0;
    if (!out || !max_records) return 0;
    for (uint32_t i = 0; i < stats.commands && count < max_records; ++i) {
        const MvGxCaptureCommand *command = &commands[i];
        if (!command->material.tev_valid) continue;
        uint32_t *record = out + count * 9u;
        record[0] = i;
        memcpy(&record[1], command->material.tev_op, 16);
        memcpy(&record[5], command->material.tev_konst, 4);
        memcpy(&record[6], command->material.tev0, 4);
        memcpy(&record[7], command->material.tev1, 4);
        record[8] = command->material.tev_active;
        ++count;
    }
    return count;
}

uint32_t mv_gx_capture_multitex_records(uint32_t *out, uint32_t max_records)
{
    uint32_t count = 0;
    if (!out || !max_records) return 0;
    for (uint32_t i = 0; i < stats.commands && count < max_records; ++i) {
        const MvGxCaptureCommand *command = &commands[i];
        const MvGxMaterialState *m = &command->material;
        if (m->texture_count < 2) continue;
        uint32_t *r = out + count * 28u;
        memset(r, 0, 28u * sizeof(*r));
        r[0] = i;
        r[1] = m->texture_count;
        r[2] = m->rendermode;
        r[3] = m->tobj_flags;
        r[4] = (uint32_t)(uintptr_t)m->image;
        r[5] = ((uint32_t)m->width << 16) | m->height;
        r[6] = ((uint32_t)m->format << 24) | ((uint32_t)m->wrap_s << 16) |
               ((uint32_t)m->wrap_t << 8) | m->mag_filter;
        memcpy(&r[7], &m->blending, sizeof(float));
        memcpy(&r[8], m->uv_mtx, sizeof(m->uv_mtx));
        r[14] = m->tev_active;
        r[15] = m->tobj1_flags;
        r[16] = (uint32_t)(uintptr_t)m->image1;
        r[17] = ((uint32_t)m->width1 << 16) | m->height1;
        r[18] = ((uint32_t)m->format1 << 24) | ((uint32_t)m->wrap_s1 << 16) |
                ((uint32_t)m->wrap_t1 << 8) | m->mag_filter1;
        memcpy(&r[19], &m->blending1, sizeof(float));
        memcpy(&r[20], m->uv_mtx1, sizeof(m->uv_mtx1));
        r[26] = m->tev1_active;
        r[27] = ((uint32_t)m->uv_mtx_valid << 24) |
                ((uint32_t)m->uv_mtx1_valid << 16) |
                ((uint32_t)m->tev_valid << 8) | m->tev1_valid;
        ++count;
    }
    return count;
}

void GXSetArray(GXAttr attr, const void *base_ptr, u8 stride)
{
    if ((unsigned)attr >= GX_VA_MAX_ATTR || !base_ptr || !stride) {
        ++stats.errors;
        return;
    }
    arrays[attr].base = base_ptr;
    arrays[attr].stride = stride;
    ++stats.array_binds;
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    if ((unsigned)attr >= GX_VA_MAX_ATTR || (unsigned)type > GX_INDEX16) {
        ++stats.errors;
        return;
    }
    attr_types[attr] = type;
}

void GXClearVtxDesc(void)
{
    memset(attr_types, 0, sizeof(attr_types));
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt count,
                     GXCompType type, u8 frac)
{
    if ((unsigned)vtxfmt >= GX_MAX_VTXFMT || (unsigned)attr >= GX_VA_MAX_ATTR) {
        ++stats.errors;
        return;
    }
    MvAttrFormat *fmt = &formats[vtxfmt][attr];
    fmt->count = count;
    fmt->type = type;
    fmt->frac = frac;
    fmt->valid = 1;
    ++stats.attr_formats;
}

void GXSetCurrentMtx(u32 id) { current_mtx = id; }

static size_t immediate_vertex_bytes(GXVtxFmt vtxfmt)
{
    size_t total = 0;
    if ((unsigned)vtxfmt >= GX_MAX_VTXFMT) return 0;
    for (unsigned attr_index = GX_VA_PNMTXIDX; attr_index <= GX_VA_TEX7; ++attr_index) {
        GXAttrType type = attr_types[attr_index];
        if (type == GX_NONE) continue;
        const MvAttrFormat *fmt = &formats[vtxfmt][attr_index];
        if (type == GX_DIRECT) {
            size_t bytes = element_size((GXAttr)attr_index, fmt);
            if (!bytes) return 0;
            total += bytes;
        } else if (type == GX_INDEX8 || type == GX_INDEX16) {
            unsigned index_count = (attr_index == GX_VA_NRM && fmt->count == GX_NRM_NBT3) ? 3u : 1u;
            total += (type == GX_INDEX8 ? 1u : 2u) * index_count;
        } else {
            return 0;
        }
    }
    return total;
}

static void capture_snapshot_camera(MvGxCaptureCommand *command)
{
    if (capture_projection_valid) {
        memcpy(command->projection, capture_projection, sizeof(command->projection));
        command->projection_type = capture_projection_type;
        command->projection_valid = 1;
    }
    if (capture_viewport_valid) {
        memcpy(command->viewport, capture_viewport, sizeof(command->viewport));
        command->viewport_valid = 1;
    }
}

static int matrix_slot(u32 id)
{
    if (id % 3u) return -1;
    unsigned slot = id / 3u;
    return slot < MV_CAPTURE_MATRIX_SLOTS ? (int)slot : -1;
}

static void capture_apply_vertex_matrices(MvGxCaptureCommand *command, uint32_t first, uint16_t count)
{
    if (!(command->attr_mask & (1u << GX_VA_PNMTXIDX))) return;
    for (uint16_t i = 0; i < count; ++i) {
        MvGxCaptureVertex *vertex = &vertices[first + i];
        int vertex_slot = matrix_slot(vertex->pos_mtx_idx);
        if (vertex_slot < 0) { ++stats.errors; return; }
        float x = vertex->position[0], y = vertex->position[1], z = vertex->position[2];
        vertex->position[0] = pos_mtx[vertex_slot][0][0] * x + pos_mtx[vertex_slot][0][1] * y + pos_mtx[vertex_slot][0][2] * z + pos_mtx[vertex_slot][0][3];
        vertex->position[1] = pos_mtx[vertex_slot][1][0] * x + pos_mtx[vertex_slot][1][1] * y + pos_mtx[vertex_slot][1][2] * z + pos_mtx[vertex_slot][1][3];
        vertex->position[2] = pos_mtx[vertex_slot][2][0] * x + pos_mtx[vertex_slot][2][1] * y + pos_mtx[vertex_slot][2][2] * z + pos_mtx[vertex_slot][2][3];
    }
    memset(command->pos_mtx, 0, sizeof(command->pos_mtx));
    command->pos_mtx[0][0] = 1.0f;
    command->pos_mtx[1][1] = 1.0f;
    command->pos_mtx[2][2] = 1.0f;
}

static void immediate_finish(void)
{
    MvGxCaptureCommand *command = &commands[stats.commands];
    capture_apply_vertex_matrices(command, stats.vertices, immediate.expected_vertices);
    if (stats.errors) { immediate.active = 0; return; }
    ++stats.commands;
    stats.vertices += immediate.expected_vertices;
    stats.triangles += command->triangle_count;
    if (command->primitive == GX_LINES || command->primitive == GX_LINESTRIP || command->primitive == GX_POINTS) ++stats.line_point_commands;
    immediate.active = 0;
}

static void immediate_write(const uint8_t *bytes, size_t count)
{
    if (!immediate.active || stats.errors) return;
    while (count--) {
        if (immediate.buffered_bytes >= immediate.vertex_bytes || immediate.buffered_bytes >= sizeof(immediate.buffer)) { ++stats.errors; immediate.active = 0; return; }
        immediate.buffer[immediate.buffered_bytes++] = *bytes++;
        if (immediate.buffered_bytes == immediate.vertex_bytes) {
            const uint8_t *cursor = immediate.buffer;
            const uint8_t *end = cursor + immediate.vertex_bytes;
            MvGxCaptureCommand *command = &commands[stats.commands];
            if (immediate.emitted_vertices >= immediate.expected_vertices || decode_vertex(&cursor, end, (GXVtxFmt)immediate.vtxfmt, &vertices[stats.vertices + immediate.emitted_vertices], &command->attr_mask) || cursor != end) { ++stats.errors; immediate.active = 0; return; }
            ++immediate.emitted_vertices;
            immediate.buffered_bytes = 0;
            if (immediate.emitted_vertices == immediate.expected_vertices) immediate_finish();
        }
    }
}

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    uint32_t triangles = primitive_triangles((uint8_t)type, nverts);
    size_t bytes = immediate_vertex_bytes(vtxfmt);
    if (immediate.active || !nverts || !bytes || bytes > sizeof(immediate.buffer) || triangles == UINT32_MAX || stats.commands >= MV_CAPTURE_MAX_COMMANDS || stats.vertices + nverts > MV_CAPTURE_MAX_VERTICES) { ++stats.errors; immediate.active = 0; return; }
    MvGxCaptureCommand *command = &commands[stats.commands];
    memset(command, 0, sizeof(*command));
    command->first_vertex = stats.vertices; command->vertex_count = nverts; command->triangle_count = triangles;
    command->primitive = (uint8_t)type; command->vtxfmt = (uint8_t)vtxfmt; command->current_mtx = current_mtx; command->cull_mode = cull_mode; command->material = material_state;
    capture_snapshot_camera(command);
    int slot = matrix_slot(current_mtx);
    if (slot >= 0) memcpy(command->pos_mtx, pos_mtx[slot], sizeof(command->pos_mtx));
    memset(&immediate, 0, sizeof(immediate));
    immediate.active = 1; immediate.primitive = (uint8_t)type; immediate.vtxfmt = (uint8_t)vtxfmt; immediate.expected_vertices = nverts; immediate.vertex_bytes = (uint16_t)bytes;
}

static void write_be16_value(uint16_t value) { uint8_t b[2] = {(uint8_t)(value >> 8), (uint8_t)value}; immediate_write(b, sizeof(b)); }
static void write_be32_value(uint32_t value) { uint8_t b[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value}; immediate_write(b, sizeof(b)); }
void GXVitaWrite_u8(u8 x) { immediate_write(&x, 1); }
void GXVitaWrite_s8(s8 x) { uint8_t b = (uint8_t)x; immediate_write(&b, 1); }
void GXVitaWrite_u16(u16 x) { write_be16_value(x); }
void GXVitaWrite_s16(s16 x) { write_be16_value((uint16_t)x); }
void GXVitaWrite_u32(u32 x) { write_be32_value(x); }
void GXVitaWrite_s32(s32 x) { write_be32_value((uint32_t)x); }
void GXVitaWrite_f32(f32 x) { uint32_t bits; memcpy(&bits, &x, sizeof(bits)); write_be32_value(bits); }

void GXLoadPosMtxImm(f32 mtx[3][4], u32 id)
{
    int slot = matrix_slot(id);
    if (!mtx || slot < 0) { ++stats.errors; return; }
    memcpy(pos_mtx[slot], mtx, sizeof(pos_mtx[slot]));
    ++stats.pos_mtx_loads;
}

void GXLoadNrmMtxImm(f32 mtx[3][4], u32 id)
{
    int slot = matrix_slot(id);
    if (!mtx || slot < 0) { ++stats.errors; return; }
    memcpy(nrm_mtx[slot], mtx, sizeof(nrm_mtx[slot]));
    ++stats.nrm_mtx_loads;
}

void GXLoadTexMtxImm(f32 mtx[][4], u32 id, GXTexMtxType type)
{
    /* Texture-matrix IDs are not laid out like PNMTX IDs. Capturing the exact
     * matrix payload is enough for the next TEV/texgen stage; slot selection is
     * intentionally bounded instead of pretending to implement GX XF memory. */
    unsigned slot = ((unsigned)id / 3u) % MV_CAPTURE_MATRIX_SLOTS;
    if (!mtx) { ++stats.errors; return; }
    memset(tex_mtx[slot], 0, sizeof(tex_mtx[slot]));
    if (type == GX_MTX2x4)
        memcpy(tex_mtx[slot], mtx, sizeof(float) * 2u * 4u);
    else if (type == GX_MTX3x4)
        memcpy(tex_mtx[slot], mtx, sizeof(tex_mtx[slot]));
    else {
        ++stats.errors;
        return;
    }
    ++stats.tex_mtx_loads;
}

void GXSetCullMode(GXCullMode mode)
{
    cull_mode = (uint32_t)mode;
    ++stats.cull_changes;
}

void GXCallDisplayList(void *list, u32 nbytes)
{
    if (!list || !nbytes) { ++stats.errors; return; }
    ++stats.display_lists;
    const uint8_t *cursor = list;
    const uint8_t *end = cursor + nbytes;
    while (cursor < end) {
        uint8_t opcode = *cursor++;
        if (opcode == GX_NOP) continue;
        uint8_t primitive = opcode & GX_OPCODE_MASK;
        GXVtxFmt vtxfmt = (GXVtxFmt)(opcode & GX_VAT_MASK);
        if ((unsigned)vtxfmt >= GX_MAX_VTXFMT || (size_t)(end - cursor) < 2) {
            ++stats.errors;
            return;
        }
        uint16_t count = read_be16(cursor);
        cursor += 2;
        uint32_t triangles = primitive_triangles(primitive, count);
        if (triangles == UINT32_MAX || stats.commands >= MV_CAPTURE_MAX_COMMANDS ||
            stats.vertices + count > MV_CAPTURE_MAX_VERTICES) {
            ++stats.errors;
            return;
        }
        MvGxCaptureCommand *command = &commands[stats.commands];
        memset(command, 0, sizeof(*command));
        command->first_vertex = stats.vertices;
        command->vertex_count = count;
        command->triangle_count = triangles;
        command->primitive = primitive;
        command->vtxfmt = (uint8_t)vtxfmt;
        command->current_mtx = current_mtx;
        command->cull_mode = cull_mode;
        command->material = material_state;
        capture_snapshot_camera(command);
        int slot = matrix_slot(current_mtx);
        if (slot >= 0) memcpy(command->pos_mtx, pos_mtx[slot], sizeof(command->pos_mtx));
        for (uint16_t i = 0; i < count; ++i) {
            if (decode_vertex(&cursor, end, vtxfmt, &vertices[stats.vertices + i],
                              &command->attr_mask)) {
                ++stats.errors;
                return;
            }
        }
        if (command->attr_mask & (1u << GX_VA_PNMTXIDX)) {
            for (uint16_t i = 0; i < count; ++i) {
                MvGxCaptureVertex *vertex = &vertices[stats.vertices + i];
                int vertex_slot = matrix_slot(vertex->pos_mtx_idx);
                if (vertex_slot < 0) {
                    ++stats.errors;
                    return;
                }
                float x = vertex->position[0];
                float y = vertex->position[1];
                float z = vertex->position[2];
                vertex->position[0] = pos_mtx[vertex_slot][0][0] * x +
                                      pos_mtx[vertex_slot][0][1] * y +
                                      pos_mtx[vertex_slot][0][2] * z +
                                      pos_mtx[vertex_slot][0][3];
                vertex->position[1] = pos_mtx[vertex_slot][1][0] * x +
                                      pos_mtx[vertex_slot][1][1] * y +
                                      pos_mtx[vertex_slot][1][2] * z +
                                      pos_mtx[vertex_slot][1][3];
                vertex->position[2] = pos_mtx[vertex_slot][2][0] * x +
                                      pos_mtx[vertex_slot][2][1] * y +
                                      pos_mtx[vertex_slot][2][2] * z +
                                      pos_mtx[vertex_slot][2][3];
            }
            memset(command->pos_mtx, 0, sizeof(command->pos_mtx));
            command->pos_mtx[0][0] = 1.0f;
            command->pos_mtx[1][1] = 1.0f;
            command->pos_mtx[2][2] = 1.0f;
        }
        ++stats.commands;
        stats.vertices += count;
        stats.triangles += triangles;
        if (primitive == GX_LINES || primitive == GX_LINESTRIP || primitive == GX_POINTS)
            ++stats.line_point_commands;
    }
}

