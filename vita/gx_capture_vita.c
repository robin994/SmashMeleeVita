#include "gx_capture_vita.h"

#include <dolphin/gx.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/os.h>
#include <math.h>
#include <string.h>

#include "gx_state_vita.h"

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
static uint8_t pos_mtx_valid[MV_CAPTURE_MATRIX_SLOTS];
static float nrm_mtx[MV_CAPTURE_MATRIX_SLOTS][3][4];
static uint8_t nrm_mtx_valid[MV_CAPTURE_MATRIX_SLOTS];
static float tex_mtx[MV_CAPTURE_MATRIX_SLOTS][3][4];
static uint8_t tex_mtx_valid[MV_CAPTURE_MATRIX_SLOTS];
static uint32_t tex_mtx_id[MV_CAPTURE_MATRIX_SLOTS];
static uint32_t current_mtx, cull_mode;
/* GX_VA_NBT shares the hardware normal VCD/array slot with GX_VA_NRM.
 * Preserve the API-level NBT semantic while decoding the canonical normal slot. */
static uint8_t normal_is_nbt;
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
static uint32_t decode_error_attr;
static uint32_t decode_error_reason;
static uint32_t decode_error_type;
static uint8_t packet_mismatch_logged;

static int decode_fail(unsigned attr, unsigned reason, GXAttrType type)
{
    decode_error_attr = attr;
    decode_error_reason = reason;
    decode_error_type = (uint32_t) type;
    return -1;
}

static uint32_t active_attr_mask(void)
{
    uint32_t mask = 0;
    for (unsigned attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr)
        if (attr_types[attr] != GX_NONE) mask |= 1u << attr;
    return mask;
}

static void active_attr_type_packs(uint32_t *lo, uint32_t *hi)
{
    uint32_t a = 0, b = 0;
    for (unsigned attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr) {
        uint32_t type = (uint32_t) attr_types[attr] & 3u;
        if (attr < 16) a |= type << (attr * 2u);
        else b |= type << ((attr - 16u) * 2u);
    }
    if (lo) *lo = a;
    if (hi) *hi = b;
}

static void capture_error(uint32_t line, uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    if (!stats.first_error_line) {
        stats.first_error_line = line;
        stats.first_error_arg0 = arg0;
        stats.first_error_arg1 = arg1;
        stats.first_error_arg2 = arg2;
    }
    ++stats.errors;
}

#define CAPTURE_ERROR(a0, a1, a2) capture_error((uint32_t)__LINE__, (uint32_t)(a0), (uint32_t)(a1), (uint32_t)(a2))

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
    else if (attr == GX_VA_NRM)
        components = (normal_is_nbt || fmt->count != GX_NRM_XYZ) ? 9 : 3;
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
    decode_error_attr = UINT32_MAX;
    decode_error_reason = 0;
    decode_error_type = 0;
    memset(out, 0, sizeof(*out));
    out->color0 = out->color1 = 0xffffffffu;
    out->raster0 = out->raster1 = 0xffffffffu;
    out->pos_mtx_idx = (uint8_t)current_mtx;
    for (unsigned attr_index = GX_VA_PNMTXIDX; attr_index <= GX_VA_TEX7; ++attr_index) {
        GXAttr attr = (GXAttr)attr_index;
        GXAttrType attr_type = attr_types[attr_index];
        if (attr_type == GX_NONE) continue;
        const MvAttrFormat *fmt = &formats[vtxfmt][attr_index];
        size_t bytes = element_size(attr, fmt);
        if (!bytes) return decode_fail(attr_index, 1u, attr_type);
        const uint8_t *source = NULL;
        if (attr_type == GX_DIRECT) {
            if ((size_t)(end - *cursor) < bytes) return decode_fail(attr_index, 2u, attr_type);
            source = *cursor;
            *cursor += bytes;
        } else if (attr_type == GX_INDEX8 || attr_type == GX_INDEX16) {
            size_t index_bytes = attr_type == GX_INDEX8 ? 1u : 2u;
            unsigned index_count = (attr == GX_VA_NRM && fmt->count == GX_NRM_NBT3) ? 3u : 1u;
            if ((size_t)(end - *cursor) < index_bytes * index_count) return decode_fail(attr_index, 3u, attr_type);
            if (!arrays[attr_index].base || !arrays[attr_index].stride) return decode_fail(attr_index, 4u, attr_type);
            if (index_count == 3u) {
                /* GX_NRM_NBT3 stores three independent indices into the same
                 * normal array (normal/binormal/tangent). Gameplay fighter
                 * display lists use this form heavily. Consume and decode
                 * all three instead of aborting the whole display list. */
                int scalar = component_size(fmt->type);
                if (!scalar) return -1;
                for (unsigned vector = 0; vector < 3u; ++vector) {
                    const uint8_t *idxp = *cursor + index_bytes * vector;
                    uint32_t index = index_bytes == 1 ? idxp[0] : read_be16(idxp);
                    const uint8_t *normal_source = arrays[attr_index].base +
                        (size_t)index * arrays[attr_index].stride;
                    for (unsigned component = 0; component < 3u; ++component) {
                        if (read_scalar(normal_source + scalar * component,
                                        fmt->type, fmt->frac,
                                        &out->normal[vector * 3u + component]))
                            return -1;
                    }
                }
                *cursor += index_bytes * 3u;
                if (attr_index < 32) { *attr_mask |= 1u << attr_index; out->present |= 1u << attr_index; }
                continue;
            }
            uint32_t index = index_bytes == 1 ? (*cursor)[0] : read_be16(*cursor);
            *cursor += index_bytes;
            source = arrays[attr_index].base + (size_t)index * arrays[attr_index].stride;
        } else {
            return decode_fail(attr_index, 5u, attr_type);
        }
        if (attr_index < 32) { *attr_mask |= 1u << attr_index; out->present |= 1u << attr_index; }
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
        } else if (attr == GX_VA_NRM) {
            int scalar = component_size(fmt->type);
            unsigned components =
                (normal_is_nbt || fmt->count != GX_NRM_XYZ) ? 9u : 3u;
            if (!scalar) return -1;
            for (unsigned component = 0; component < components; ++component)
                if (read_scalar(source + scalar * component, fmt->type, fmt->frac,
                                &out->normal[component])) return -1;
        } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
            uint32_t *color = attr == GX_VA_CLR0 ? &out->color0 : &out->color1;
            if (read_color(source, fmt->type, color)) return -1;
        } else if (attr == GX_VA_TEX0 || attr == GX_VA_TEX1) {
            int scalar = component_size(fmt->type);
            float *tex = attr == GX_VA_TEX0 ? out->tex0 : out->tex1;
            if (!scalar || read_scalar(source, fmt->type, fmt->frac, &tex[0])) return -1;
            if (fmt->count == GX_TEX_ST &&
                read_scalar(source + scalar, fmt->type, fmt->frac, &tex[1])) return -1;
        }
    }
    return (out->present & (1u << GX_VA_POS)) ? 0 : -1;
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

void mv_gx_capture_begin_frame(void)
{
    /* A frame boundary clears only the command FIFO. It is not a GX reset:
     * HSD relies on projection, descriptors, matrices and PE/TEV state
     * persisting until the game changes them. */
    memset(&stats, 0, sizeof(stats));
    memset(commands, 0, sizeof(commands));
    memset(vertices, 0, sizeof(vertices));
    memset(&immediate, 0, sizeof(immediate));
}

void mv_gx_capture_reset_material_state(void)
{
    memset(&material_state, 0, sizeof(material_state));
    material_state.material_rgba = 0xffffffffu;
    for (unsigned i = 0; i < 4u; ++i) {
        material_state.tev_kcolor_sel[i] = GX_TEV_KCSEL_1_4;
        material_state.tev_kalpha_sel[i] = GX_TEV_KASEL_1_4;
    }
}

void mv_gx_capture_reset(void)
{
    mv_gx_capture_begin_frame();
    memset(attr_types, 0, sizeof(attr_types));
    memset(arrays, 0, sizeof(arrays));
    memset(formats, 0, sizeof(formats));
    memset(pos_mtx, 0, sizeof(pos_mtx));
    memset(pos_mtx_valid, 0, sizeof(pos_mtx_valid));
    memset(nrm_mtx, 0, sizeof(nrm_mtx));
    memset(nrm_mtx_valid, 0, sizeof(nrm_mtx_valid));
    memset(tex_mtx, 0, sizeof(tex_mtx));
    memset(tex_mtx_valid, 0, sizeof(tex_mtx_valid));
    memset(tex_mtx_id, 0, sizeof(tex_mtx_id));
    normal_is_nbt = 0;
    packet_mismatch_logged = 0;
    mv_gx_capture_reset_material_state();
    memset(capture_projection, 0, sizeof(capture_projection));
    memset(capture_viewport, 0, sizeof(capture_viewport));
    capture_projection_type = 0;
    capture_projection_valid = 0;
    capture_viewport_valid = 0;
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
    else mv_gx_capture_reset_material_state();
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

static uint32_t capture_hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size-- != 0) {
        hash ^= *bytes++;
        hash *= 16777619u;
    }
    return hash;
}

uint32_t mv_gx_capture_frame_signature(void)
{
    uint32_t hash = 2166136261u;
    hash = capture_hash_bytes(hash, &stats.commands, sizeof(stats.commands));
    hash = capture_hash_bytes(hash, &stats.vertices, sizeof(stats.vertices));
    hash = capture_hash_bytes(hash, commands,
                              (size_t) stats.commands * sizeof(commands[0]));
    hash = capture_hash_bytes(hash, vertices,
                              (size_t) stats.vertices * sizeof(vertices[0]));
    return hash;
}

int mv_gx_alpha_compare_vitagl_supported(uint8_t comp0, uint8_t ref0,
                                         uint8_t op, uint8_t comp1,
                                         uint8_t ref1)
{
    if (comp0 == GX_ALWAYS && comp1 == GX_ALWAYS) return 1;
    if (comp0 == GX_GREATER && ref0 == 0 && op == GX_AOP_OR &&
        (comp1 == GX_NEVER || (comp1 == GX_GREATER && ref1 == 0)))
        return 1;

    /* Yoshi's Island uses GX's two-comparator alpha test for foliage and
     * background layers.  These observed AND forms collapse exactly to one
     * fixed-function GEQUAL test (or ALWAYS when the lower bound is zero). */
    if (op == GX_AOP_AND) {
        if (comp0 == GX_GEQUAL && comp1 == GX_GEQUAL) return 1;
        if (comp0 == GX_GEQUAL && comp1 == GX_LEQUAL && ref1 == 255) return 1;
        if (comp0 == GX_LEQUAL && ref0 == 255 && comp1 == GX_GEQUAL) return 1;
    }
    return 0;
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

int mv_gx_material_multitex_hsd_modulate(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 2 || m->tev_stage_count != 2 ||
        (m->texgen_valid_mask & 3u) != 3u ||
        m->texgen_src0 != GX_TG_TEX0 || m->texgen_src1 != GX_TG_TEX1)
        return 0;
    if (m->tev_order_coord[0] != GX_TEXCOORD0 ||
        m->tev_order_coord[1] != GX_TEXCOORD1 ||
        m->tev_order_map[0] != GX_TEXMAP0 ||
        m->tev_order_map[1] != GX_TEXMAP1 ||
        m->tev_order_color[0] != GX_COLOR0A0 ||
        m->tev_order_color[1] != GX_COLOR0A0)
        return 0;
    static const uint8_t color0[4] = {GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO};
    static const uint8_t color1[4] = {GX_CC_ZERO, GX_CC_CPREV, GX_CC_TEXC, GX_CC_ZERO};
    static const uint8_t alpha1[4] = {GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA};
    if (memcmp(m->tev_color_in[0], color0, sizeof(color0)) != 0 ||
        memcmp(m->tev_color_in[1], color1, sizeof(color1)) != 0 ||
        memcmp(m->tev_alpha_in[1], alpha1, sizeof(alpha1)) != 0)
        return 0;
    for (unsigned stage = 0; stage < 2; ++stage) {
        const uint8_t *op = m->tev_color_op[stage];
        if (op[0] != GX_TEV_ADD || op[1] != GX_TB_ZERO ||
            op[2] != GX_CS_SCALE_1 || op[3] != GX_ENABLE ||
            op[4] != GX_TEVPREV) return 0;
    }
    const uint8_t *aop = m->tev_alpha_op[1];
    if (aop[0] != GX_TEV_ADD || aop[1] != GX_TB_ZERO ||
        aop[2] != GX_CS_SCALE_1 || aop[3] != GX_ENABLE ||
        aop[4] != GX_TEVPREV) return 0;
    return 1;
}

int mv_gx_material_multitex_hsd_alpha_blend(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 2 || m->tev_stage_count != 2 ||
        (m->texgen_valid_mask & 3u) != 3u ||
        m->texgen_src0 != GX_TG_TEX0 || m->texgen_src1 != GX_TG_TEX1)
        return 0;
    if (m->tev_order_coord[0] != GX_TEXCOORD0 ||
        m->tev_order_coord[1] != GX_TEXCOORD1 ||
        m->tev_order_map[0] != GX_TEXMAP0 ||
        m->tev_order_map[1] != GX_TEXMAP1 ||
        m->tev_order_color[0] != GX_COLOR0A0)
        return 0;

    static const uint8_t color0[4] = {
        GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO,
    };
    static const uint8_t color1[4] = {
        GX_CC_CPREV, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO,
    };
    if (memcmp(m->tev_color_in[0], color0, sizeof(color0)) != 0 ||
        memcmp(m->tev_color_in[1], color1, sizeof(color1)) != 0)
        return 0;

    for (unsigned stage = 0; stage < 2; ++stage) {
        const uint8_t *op = m->tev_color_op[stage];
        if (op[0] != GX_TEV_ADD || op[1] != GX_TB_ZERO ||
            op[2] != GX_CS_SCALE_1 || op[3] != GX_ENABLE ||
            op[4] != GX_TEVPREV)
            return 0;
    }

    static const uint8_t alpha0_rasa[4] = {
        GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
    };
    static const uint8_t alpha1_rasa[4] = {
        GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA,
    };
    if (m->tev_order_color[1] == GX_COLOR0A0 &&
        memcmp(m->tev_alpha_in[0], alpha0_rasa, sizeof(alpha0_rasa)) == 0 &&
        memcmp(m->tev_alpha_in[1], alpha1_rasa, sizeof(alpha1_rasa)) == 0) {
        const uint8_t *op = m->tev_alpha_op[1];
        if (op[0] == GX_TEV_ADD && op[1] == GX_TB_ZERO &&
            op[2] == GX_CS_SCALE_1 && op[3] == GX_ENABLE &&
            op[4] == GX_TEVPREV)
            return 1;
    }

    static const uint8_t alpha0_tex0[4] = {
        GX_CA_ZERO, GX_CA_RASA, GX_CA_TEXA, GX_CA_ZERO,
    };
    static const uint8_t alpha1_tex0[4] = {
        GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV,
    };
    if (m->tev_order_color[1] == GX_COLOR_NULL &&
        memcmp(m->tev_alpha_in[0], alpha0_tex0, sizeof(alpha0_tex0)) == 0 &&
        memcmp(m->tev_alpha_in[1], alpha1_tex0, sizeof(alpha1_tex0)) == 0) {
        for (unsigned stage = 0; stage < 2; ++stage) {
            const uint8_t *op = m->tev_alpha_op[stage];
            if (op[0] != GX_TEV_ADD || op[1] != GX_TB_ZERO ||
                op[2] != GX_CS_SCALE_1 || op[3] != GX_ENABLE ||
                op[4] != GX_TEVPREV)
                return 0;
        }
        return 2;
    }

    return 0;
}

int mv_gx_material_multitex_hsd_specular_add(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 2 || m->tev_stage_count != 3 ||
        (m->texgen_valid_mask & 3u) != 3u ||
        m->texgen_src0 != GX_TG_TEX0 || m->texgen_src1 != GX_TG_TEX1)
        return 0;
    if (m->tev_order_coord[0] != GX_TEXCOORD0 ||
        m->tev_order_coord[1] != GX_TEXCOORD0 ||
        m->tev_order_coord[2] != GX_TEXCOORD1 ||
        m->tev_order_map[0] != GX_TEXMAP0 ||
        m->tev_order_map[1] != GX_TEXMAP0 ||
        m->tev_order_map[2] != GX_TEXMAP1 ||
        m->tev_order_color[0] != GX_COLOR0A0 ||
        m->tev_order_color[1] != GX_COLOR0A0 ||
        m->tev_order_color[2] != GX_COLOR1A1)
        return 0;

    static const uint8_t color0[4] = {
        GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO,
    };
    static const uint8_t color1[4] = {
        GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO,
    };
    static const uint8_t color2[4] = {
        GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_CPREV,
    };
    static const uint8_t alpha0[4] = {
        GX_CA_ZERO, GX_CA_A0, GX_CA_RASA, GX_CA_ZERO,
    };
    static const uint8_t alpha_prev[4] = {
        GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV,
    };
    if (memcmp(m->tev_color_in[0], color0, sizeof(color0)) != 0 ||
        memcmp(m->tev_color_in[1], color1, sizeof(color1)) != 0 ||
        memcmp(m->tev_color_in[2], color2, sizeof(color2)) != 0 ||
        memcmp(m->tev_alpha_in[0], alpha0, sizeof(alpha0)) != 0 ||
        memcmp(m->tev_alpha_in[1], alpha_prev, sizeof(alpha_prev)) != 0 ||
        memcmp(m->tev_alpha_in[2], alpha_prev, sizeof(alpha_prev)) != 0)
        return 0;

    for (unsigned stage = 1; stage < 3; ++stage) {
        const uint8_t *cop = m->tev_color_op[stage];
        const uint8_t *aop = m->tev_alpha_op[stage];
        if (cop[0] != GX_TEV_ADD || cop[1] != GX_TB_ZERO ||
            cop[2] != GX_CS_SCALE_1 || cop[3] != GX_ENABLE ||
            cop[4] != GX_TEVPREV || aop[0] != GX_TEV_ADD ||
            aop[1] != GX_TB_ZERO || aop[2] != GX_CS_SCALE_1 ||
            aop[4] != GX_TEVPREV)
            return 0;
    }
    const uint8_t *aop0 = m->tev_alpha_op[0];
    return aop0[0] == GX_TEV_ADD && aop0[1] == GX_TB_ZERO &&
           aop0[2] == GX_CS_SCALE_1 && aop0[3] == GX_ENABLE &&
           aop0[4] == GX_TEVPREV;
}

static int material_uses_raster_channel(const MvGxMaterialState *m,
                                        unsigned channel)
{
    if (!m || channel > 1u) return 0;

    unsigned count = m->tev_stage_count;
    if (count > 4u) count = 4u;
    for (unsigned stage = 0; stage < count; ++stage) {
        const uint8_t order = m->tev_order_color[stage];
        if (channel == 0) {
            if (order != GX_COLOR0 && order != GX_ALPHA0 &&
                order != GX_COLOR0A0)
                continue;
        } else {
            if (order != GX_COLOR1 && order != GX_ALPHA1 &&
                order != GX_COLOR1A1)
                continue;
        }

        for (unsigned arg = 0; arg < 4u; ++arg) {
            const uint8_t color = m->tev_color_in[stage][arg];
            if (color == GX_CC_RASC || color == GX_CC_RASA)
                return 1;
            if (m->tev_alpha_in[stage][arg] == GX_CA_RASA)
                return 1;
        }
    }
    return 0;
}

int mv_gx_material_uses_raster0(const MvGxMaterialState *m)
{
    return material_uses_raster_channel(m, 0);
}

int mv_gx_material_uses_raster1(const MvGxMaterialState *m)
{
    return material_uses_raster_channel(m, 1);
}

int mv_gx_material_single_tev_rasc_tex(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 1 || m->tev_stage_count != 1 ||
        !(m->texgen_valid_mask & 1u) || m->texgen_src0 != GX_TG_TEX0)
        return 0;
    if (m->tev_order_coord[0] != GX_TEXCOORD0 ||
        m->tev_order_map[0] != GX_TEXMAP0 ||
        m->tev_order_color[0] != GX_COLOR0A0)
        return 0;

    static const uint8_t color[4] = {
        GX_CC_ZERO, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO,
    };
    if (memcmp(m->tev_color_in[0], color, sizeof(color)) != 0) return 0;

    const uint8_t *cop = m->tev_color_op[0];
    const uint8_t *aop = m->tev_alpha_op[0];
    if (cop[0] != GX_TEV_ADD || cop[1] != GX_TB_ZERO ||
        cop[2] != GX_CS_SCALE_1 || cop[3] != GX_ENABLE ||
        cop[4] != GX_TEVPREV || aop[0] != GX_TEV_ADD ||
        aop[1] != GX_TB_ZERO || aop[2] != GX_CS_SCALE_1 ||
        aop[3] != GX_ENABLE || aop[4] != GX_TEVPREV)
        return 0;

    static const uint8_t alpha_texa[4] = {
        GX_CA_ZERO, GX_CA_RASA, GX_CA_TEXA, GX_CA_ZERO,
    };
    static const uint8_t alpha_rasa[4] = {
        GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA,
    };
    if (memcmp(m->tev_alpha_in[0], alpha_texa, sizeof(alpha_texa)) == 0)
        return 1;
    if (memcmp(m->tev_alpha_in[0], alpha_rasa, sizeof(alpha_rasa)) == 0)
        return 2;
    return 0;
}

int mv_gx_material_single_tev_rasc_tex_konst(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 1 || m->tev_stage_count != 1)
        return 0;
    if (m->tev_order_coord[0] != GX_TEXCOORD0 ||
        m->tev_order_map[0] != GX_TEXMAP0 ||
        m->tev_order_color[0] != GX_COLOR0A0)
        return 0;
    static const uint8_t color[4] = {GX_CC_RASC, GX_CC_TEXC,
                                     GX_CC_KONST, GX_CC_ZERO};
    static const uint8_t alpha[4] = {GX_CA_ZERO, GX_CA_ZERO,
                                     GX_CA_ZERO, GX_CA_RASA};
    if (memcmp(m->tev_color_in[0], color, sizeof(color)) != 0 ||
        memcmp(m->tev_alpha_in[0], alpha, sizeof(alpha)) != 0)
        return 0;
    const uint8_t *cop = m->tev_color_op[0];
    const uint8_t *aop = m->tev_alpha_op[0];
    return cop[0] == GX_TEV_ADD && cop[1] == GX_TB_ZERO &&
           cop[2] == GX_CS_SCALE_1 && cop[3] == GX_ENABLE &&
           cop[4] == GX_TEVPREV &&
           aop[0] == GX_TEV_ADD && aop[1] == GX_TB_ZERO &&
           aop[2] == GX_CS_SCALE_1 && aop[3] == GX_ENABLE &&
           aop[4] == GX_TEVPREV;
}

uint32_t mv_gx_material_kcolor_rgba(const MvGxMaterialState *m, unsigned stage)
{
    if (!m || stage >= 4u) return 0xffffffffu;
    const unsigned sel = m->tev_kcolor_sel[stage];
    static const uint8_t fixed[8] = {255, 223, 191, 159, 127, 95, 63, 31};
    if (sel < 8u) {
        uint8_t v = fixed[sel];
        return (uint32_t)v << 24 | (uint32_t)v << 16 | (uint32_t)v << 8 | 0xffu;
    }
    if (sel >= GX_TEV_KCSEL_K0 && sel <= GX_TEV_KCSEL_K3)
        return m->tev_kcolor_regs[sel - GX_TEV_KCSEL_K0];
    if (sel >= GX_TEV_KCSEL_K0_R && sel <= GX_TEV_KCSEL_K3_A) {
        unsigned reg = sel & 3u;
        unsigned component = (sel - GX_TEV_KCSEL_K0_R) >> 2;
        uint32_t packed = m->tev_kcolor_regs[reg];
        unsigned shift = 24u - component * 8u;
        uint8_t v = (uint8_t)(packed >> shift);
        return (uint32_t)v << 24 | (uint32_t)v << 16 | (uint32_t)v << 8 | 0xffu;
    }
    return 0xffffffffu;
}

int mv_gx_material_multitex_vitagl_supported(const MvGxMaterialState *m)
{
    if (!m || m->texture_count != 2 || !m->image || !m->image1 ||
        !m->width || !m->height || !m->width1 || !m->height1)
        return 0;
    if ((m->unsupported & ~MV_GX_MATERIAL_UNSUPPORTED_MULTITEX) != 0)
        return 0;
    if (mv_gx_material_multitex_hsd_modulate(m) ||
        mv_gx_material_multitex_hsd_alpha_blend(m) ||
        mv_gx_material_multitex_hsd_specular_add(m)) {
        if (!uv_mtx_finite(m->uv_mtx, m->uv_mtx_valid) ||
            !uv_mtx_finite(m->uv_mtx1, m->uv_mtx1_valid)) return 0;
        if (m->wrap_s > GX_MIRROR || m->wrap_t > GX_MIRROR ||
            m->wrap_s1 > GX_MIRROR || m->wrap_t1 > GX_MIRROR ||
            m->mag_filter != GX_LINEAR || m->mag_filter1 != GX_LINEAR) return 0;
        return 1;
    }
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
    /* Retail GX aliases NBT arrays to the normal CP array. */
    if (attr == GX_VA_NBT) attr = GX_VA_NRM;
    if ((unsigned)attr >= GX_VA_MAX_ATTR || !base_ptr || !stride) {
        CAPTURE_ERROR(attr, base_ptr != NULL, stride);
        return;
    }
    arrays[attr].base = base_ptr;
    arrays[attr].stride = stride;
    ++stats.array_binds;
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    if ((unsigned)attr >= GX_VA_MAX_ATTR || (unsigned)type > GX_INDEX16) {
        CAPTURE_ERROR(attr, type, 0);
        return;
    }
    /* GX exposes NRM and NBT as API names for one hardware VCD field. */
    if (attr == GX_VA_NBT) {
        if (type != GX_NONE) {
            attr_types[GX_VA_NRM] = type;
            normal_is_nbt = 1;
        } else if (normal_is_nbt) {
            attr_types[GX_VA_NRM] = GX_NONE;
            normal_is_nbt = 0;
        }
        attr_types[GX_VA_NBT] = GX_NONE;
        return;
    }
    if (attr == GX_VA_NRM) {
        if (type != GX_NONE) {
            attr_types[GX_VA_NRM] = type;
            normal_is_nbt = 0;
        } else if (!normal_is_nbt) {
            attr_types[GX_VA_NRM] = GX_NONE;
        }
        return;
    }
    attr_types[attr] = type;
}

void GXClearVtxDesc(void)
{
    memset(attr_types, 0, sizeof(attr_types));
    normal_is_nbt = 0;
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt count,
                     GXCompType type, u8 frac)
{
    /* GX_VA_NBT shares VAT normal-format state with GX_VA_NRM. */
    if (attr == GX_VA_NBT) attr = GX_VA_NRM;
    if ((unsigned)vtxfmt >= GX_MAX_VTXFMT || (unsigned)attr >= GX_VA_MAX_ATTR) {
        CAPTURE_ERROR(vtxfmt, attr, type);
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

static int transform_normal_triplet(const MvGxCaptureVertex *vertex, int slot,
                                    unsigned base, float out[3])
{
    if (!(vertex->present & (1u << GX_VA_NRM)) || base + 2u >= 9u) return 0;
    const float *src = &vertex->normal[base];
    if (!isfinite(src[0]) || !isfinite(src[1]) || !isfinite(src[2])) return 0;
    if (slot >= 0 && nrm_mtx_valid[slot]) {
        out[0] = nrm_mtx[slot][0][0] * src[0] + nrm_mtx[slot][0][1] * src[1] + nrm_mtx[slot][0][2] * src[2];
        out[1] = nrm_mtx[slot][1][0] * src[0] + nrm_mtx[slot][1][1] * src[1] + nrm_mtx[slot][1][2] * src[2];
        out[2] = nrm_mtx[slot][2][0] * src[0] + nrm_mtx[slot][2][1] * src[1] + nrm_mtx[slot][2][2] * src[2];
    } else if (slot >= 0 && pos_mtx_valid[slot]) {
        /* Rigid transforms use the same 3x3 basis.  This is only a fallback
         * for primitives that do not explicitly load a normal matrix. */
        out[0] = pos_mtx[slot][0][0] * src[0] + pos_mtx[slot][0][1] * src[1] + pos_mtx[slot][0][2] * src[2];
        out[1] = pos_mtx[slot][1][0] * src[0] + pos_mtx[slot][1][1] * src[1] + pos_mtx[slot][1][2] * src[2];
        out[2] = pos_mtx[slot][2][0] * src[0] + pos_mtx[slot][2][1] * src[1] + pos_mtx[slot][2][2] * src[2];
    } else {
        out[0] = src[0]; out[1] = src[1]; out[2] = src[2];
    }
    float len2 = out[0]*out[0] + out[1]*out[1] + out[2]*out[2];
    if (!isfinite(len2) || len2 <= 1.0e-20f) return 0;
    float inv = 1.0f / sqrtf(len2);
    out[0] *= inv; out[1] *= inv; out[2] *= inv;
    return 1;
}

static void finalize_vertex_xf(MvGxCaptureCommand *command,
                               MvGxCaptureVertex *vertex, int slot,
                               int bake_position)
{
    float eye[3] = {vertex->position[0], vertex->position[1], vertex->position[2]};
    if (slot >= 0 && pos_mtx_valid[slot]) {
        float x = vertex->position[0], y = vertex->position[1], z = vertex->position[2];
        eye[0] = pos_mtx[slot][0][0] * x + pos_mtx[slot][0][1] * y + pos_mtx[slot][0][2] * z + pos_mtx[slot][0][3];
        eye[1] = pos_mtx[slot][1][0] * x + pos_mtx[slot][1][1] * y + pos_mtx[slot][1][2] * z + pos_mtx[slot][1][3];
        eye[2] = pos_mtx[slot][2][0] * x + pos_mtx[slot][2][1] * y + pos_mtx[slot][2][2] * z + pos_mtx[slot][2][3];
        if (bake_position) {
            vertex->position[0] = eye[0]; vertex->position[1] = eye[1]; vertex->position[2] = eye[2];
        }
    }

    float normal[3] = {0.0f, 0.0f, 0.0f};
    int have_normal = transform_normal_triplet(vertex, slot, 0u, normal);
    if (have_normal) {
        vertex->normal[0] = normal[0]; vertex->normal[1] = normal[1]; vertex->normal[2] = normal[2];
        if (normal_is_nbt) {
            float extra[3];
            if (transform_normal_triplet(vertex, slot, 3u, extra)) {
                vertex->normal[3]=extra[0]; vertex->normal[4]=extra[1]; vertex->normal[5]=extra[2];
            }
            if (transform_normal_triplet(vertex, slot, 6u, extra)) {
                vertex->normal[6]=extra[0]; vertex->normal[7]=extra[1]; vertex->normal[8]=extra[2];
            }
        }
    }

    uint32_t combined_flags = 0;
    if (mv_gx_material_uses_raster0(&command->material)) {
        uint32_t channel_flags = 0;
        uint32_t source = (vertex->present & (1u << GX_VA_CLR0))
                              ? vertex->color0 : 0xffffffffu;
        uint32_t raster = mv_gx_channel0_eval(source, eye, normal, &channel_flags);
        /* A lit channel without a usable normal is not authoritative. Retain
         * the raw color instead of recreating the old all-black failure. */
        if (!(channel_flags & MV_GX_CHANNEL_EVAL_LIT) ||
            (channel_flags & MV_GX_CHANNEL_EVAL_NORMAL))
            vertex->raster0 = raster;
        else
            vertex->raster0 = source;
        combined_flags |= channel_flags;
    }
    if (mv_gx_material_uses_raster1(&command->material)) {
        uint32_t channel_flags = 0;
        uint32_t source = (vertex->present & (1u << GX_VA_CLR1))
                              ? vertex->color1 : 0xffffffffu;
        uint32_t raster = mv_gx_channel1_eval(source, eye, normal, &channel_flags);
        if (!(channel_flags & MV_GX_CHANNEL_EVAL_LIT) ||
            (channel_flags & MV_GX_CHANNEL_EVAL_NORMAL))
            vertex->raster1 = raster;
        else
            vertex->raster1 = source;
        combined_flags |= channel_flags;
    }
    if (combined_flags & MV_GX_CHANNEL_EVAL_ACTIVE) {
        ++stats.channel_eval_vertices;
        if (combined_flags & MV_GX_CHANNEL_EVAL_LIT)
            ++stats.channel_lit_vertices;
        if (combined_flags & MV_GX_CHANNEL_EVAL_NORMAL)
            ++stats.channel_normal_vertices;
    }
}

static void capture_apply_vertex_matrices(MvGxCaptureCommand *command, uint32_t first, uint16_t count)
{
    const int indexed = (command->attr_mask & (1u << GX_VA_PNMTXIDX)) != 0;
    if (indexed) command->pos_mtx_valid = 1;
    for (uint16_t i = 0; i < count; ++i) {
        MvGxCaptureVertex *vertex = &vertices[first + i];
        int vertex_slot = matrix_slot(indexed ? vertex->pos_mtx_idx : command->current_mtx);
        if (vertex_slot < 0) { CAPTURE_ERROR(vertex->pos_mtx_idx, first + i, count); return; }
        if (!pos_mtx_valid[vertex_slot]) command->pos_mtx_valid = 0;
        finalize_vertex_xf(command, vertex, vertex_slot, indexed);
    }
    if (indexed) {
        memset(command->pos_mtx, 0, sizeof(command->pos_mtx));
        command->pos_mtx[0][0] = 1.0f;
        command->pos_mtx[1][1] = 1.0f;
        command->pos_mtx[2][2] = 1.0f;
    }
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
        if (immediate.buffered_bytes >= immediate.vertex_bytes || immediate.buffered_bytes >= sizeof(immediate.buffer)) { CAPTURE_ERROR(immediate.buffered_bytes, immediate.vertex_bytes, sizeof(immediate.buffer)); immediate.active = 0; return; }
        immediate.buffer[immediate.buffered_bytes++] = *bytes++;
        if (immediate.buffered_bytes == immediate.vertex_bytes) {
            const uint8_t *cursor = immediate.buffer;
            const uint8_t *end = cursor + immediate.vertex_bytes;
            MvGxCaptureCommand *command = &commands[stats.commands];
            if (immediate.emitted_vertices >= immediate.expected_vertices) {
                CAPTURE_ERROR(immediate.emitted_vertices, immediate.expected_vertices, 1);
                immediate.active = 0;
                return;
            }
            int decode_result = decode_vertex(&cursor, end,
                                               (GXVtxFmt) immediate.vtxfmt,
                                               &vertices[stats.vertices + immediate.emitted_vertices],
                                               &command->attr_mask);
            if (decode_result != 0 || cursor != end) {
                uint32_t consumed = (uint32_t) (cursor - immediate.buffer);
                if (decode_result == 0 && cursor != end) {
                    uint32_t types_lo = 0, types_hi = 0;
                    active_attr_type_packs(&types_lo, &types_hi);
                    if (!packet_mismatch_logged) {
                        OSReport("VITA_GX_PACKET_MISMATCH command=%u primitive=%u vtxfmt=%u nverts=%u emitted=%u consumed=%u expected=%u active=%08x types_lo=%08x types_hi=%08x current_mtx=%u normal_is_nbt=%u\n",
                                 stats.commands, immediate.primitive, immediate.vtxfmt,
                                 immediate.expected_vertices, immediate.emitted_vertices,
                                 consumed, immediate.vertex_bytes, active_attr_mask(),
                                 types_lo, types_hi, current_mtx, normal_is_nbt);
                        packet_mismatch_logged = 1;
                    }
                    CAPTURE_ERROR(0xfeu, active_attr_mask(),
                                  ((consumed & 0xffffu) << 16) | immediate.vertex_bytes);
                } else {
                    uint32_t packed = ((decode_error_type & 0xffu) << 24) |
                                      (((uint32_t) immediate.vtxfmt & 0xffu) << 16) |
                                      ((consumed & 0xffu) << 8) |
                                      (immediate.vertex_bytes & 0xffu);
                    CAPTURE_ERROR(decode_error_reason ? decode_error_reason : 0xffu,
                                  decode_error_attr, packed);
                }
                immediate.active = 0;
                return;
            }
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
    if (immediate.active || !nverts || !bytes || bytes > sizeof(immediate.buffer) || triangles == UINT32_MAX || stats.commands >= MV_CAPTURE_MAX_COMMANDS || stats.vertices + nverts > MV_CAPTURE_MAX_VERTICES) { CAPTURE_ERROR(type, vtxfmt, nverts); immediate.active = 0; return; }
    MvGxCaptureCommand *command = &commands[stats.commands];
    memset(command, 0, sizeof(*command));
    command->first_vertex = stats.vertices; command->vertex_count = nverts; command->triangle_count = triangles;
    command->primitive = (uint8_t)type; command->vtxfmt = (uint8_t)vtxfmt; command->current_mtx = current_mtx; command->cull_mode = cull_mode; command->material = material_state;
    capture_snapshot_camera(command);
    int slot = matrix_slot(current_mtx);
    if (slot >= 0) { memcpy(command->pos_mtx, pos_mtx[slot], sizeof(command->pos_mtx)); command->pos_mtx_valid = pos_mtx_valid[slot]; }
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
    if (!mtx || slot < 0) { CAPTURE_ERROR(id, slot, mtx != NULL); return; }
    memcpy(pos_mtx[slot], mtx, sizeof(pos_mtx[slot]));
    pos_mtx_valid[slot] = 1;
    ++stats.pos_mtx_loads;
}

void GXLoadNrmMtxImm(f32 mtx[3][4], u32 id)
{
    int slot = matrix_slot(id);
    if (!mtx || slot < 0) { CAPTURE_ERROR(id, slot, mtx != NULL); return; }
    memcpy(nrm_mtx[slot], mtx, sizeof(nrm_mtx[slot]));
    nrm_mtx_valid[slot] = 1;
    ++stats.nrm_mtx_loads;
}

void GXLoadTexMtxImm(f32 mtx[][4], u32 id, GXTexMtxType type)
{
    /* Texture-matrix IDs are not laid out like PNMTX IDs. Capturing the exact
     * matrix payload is enough for the next TEV/texgen stage; slot selection is
     * intentionally bounded instead of pretending to implement GX XF memory. */
    unsigned slot = ((unsigned)id / 3u) % MV_CAPTURE_MATRIX_SLOTS;
    if (!mtx) { CAPTURE_ERROR(id, type, 0); return; }
    memset(tex_mtx[slot], 0, sizeof(tex_mtx[slot]));
    if (type == GX_MTX2x4)
        memcpy(tex_mtx[slot], mtx, sizeof(float) * 2u * 4u);
    else if (type == GX_MTX3x4)
        memcpy(tex_mtx[slot], mtx, sizeof(tex_mtx[slot]));
    else {
        CAPTURE_ERROR(id, type, slot);
        return;
    }
    tex_mtx_valid[slot] = 1;
    tex_mtx_id[slot] = id;
    ++stats.tex_mtx_loads;
}

int mv_gx_capture_get_tex_mtx(uint32_t id, float out[2][3])
{
    if (!out) return -1;
    unsigned slot = ((unsigned)id / 3u) % MV_CAPTURE_MATRIX_SLOTS;
    if (!tex_mtx_valid[slot] || tex_mtx_id[slot] != id) return -1;
    out[0][0] = tex_mtx[slot][0][0];
    out[0][1] = tex_mtx[slot][0][1];
    out[0][2] = tex_mtx[slot][0][3];
    out[1][0] = tex_mtx[slot][1][0];
    out[1][1] = tex_mtx[slot][1][1];
    out[1][2] = tex_mtx[slot][1][3];
    for (unsigned r = 0; r < 2; ++r)
        for (unsigned c = 0; c < 3; ++c)
            if (!isfinite(out[r][c])) return -1;
    return 0;
}

void GXSetCullMode(GXCullMode mode)
{
    cull_mode = (uint32_t)mode;
    ++stats.cull_changes;
}

void GXCallDisplayList(void *list, u32 nbytes)
{
    if (!list || !nbytes) { CAPTURE_ERROR(nbytes, list != NULL, 0); return; }
    ++stats.display_lists;
    const uint8_t *cursor = list;
    const uint8_t *end = cursor + nbytes;
    while (cursor < end) {
        uint8_t opcode = *cursor++;
        if (opcode == GX_NOP) continue;
        uint8_t primitive = opcode & GX_OPCODE_MASK;
        GXVtxFmt vtxfmt = (GXVtxFmt)(opcode & GX_VAT_MASK);
        if ((unsigned)vtxfmt >= GX_MAX_VTXFMT || (size_t)(end - cursor) < 2) {
            CAPTURE_ERROR(opcode, vtxfmt, (uint32_t)(end - cursor));
            return;
        }
        uint16_t count = read_be16(cursor);
        cursor += 2;
        uint32_t triangles = primitive_triangles(primitive, count);
        if (triangles == UINT32_MAX || stats.commands >= MV_CAPTURE_MAX_COMMANDS ||
            stats.vertices + count > MV_CAPTURE_MAX_VERTICES) {
            CAPTURE_ERROR(primitive, count, stats.commands);
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
        if (slot >= 0) { memcpy(command->pos_mtx, pos_mtx[slot], sizeof(command->pos_mtx)); command->pos_mtx_valid = pos_mtx_valid[slot]; }
        for (uint16_t i = 0; i < count; ++i) {
            if (decode_vertex(&cursor, end, vtxfmt, &vertices[stats.vertices + i],
                              &command->attr_mask)) {
                uint32_t packed = ((decode_error_type & 0xffu) << 24) |
                                  (((uint32_t) vtxfmt & 0xffu) << 16) |
                                  (command->attr_mask & 0xffffu);
                CAPTURE_ERROR(decode_error_reason ? decode_error_reason : 0xffu,
                              decode_error_attr, packed);
                return;
            }
        }
        capture_apply_vertex_matrices(command, stats.vertices, count);
        if (stats.errors) return;
        ++stats.commands;
        stats.vertices += count;
        stats.triangles += triangles;
        if (primitive == GX_LINES || primitive == GX_LINESTRIP || primitive == GX_POINTS)
            ++stats.line_point_commands;
    }
}
