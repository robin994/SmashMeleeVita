#include "hsd_scene.h"
#include "gx_texture.h"
#include "hsd_anim.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NONE UINT32_MAX
#define MAX_NODES 32768u
#define MAX_SCENE_VERTICES (1024u * 1024u)
#define MAX_SCENE_MESHES 8192u
#define MAX_VERTEX_ATTRS 32u

typedef struct { uint32_t offset; uint8_t kind; } Node;
/* Kinds are bits so shared descriptors are visited once per actual type. */
enum { JOINT = 1, DOBJ = 2, MOBJ = 4, TOBJ = 8 };

static int push_pointer(const MvDat *v, uint32_t field, unsigned kind, Node *todo, size_t *count)
{
    uint32_t target;
    int result = mv_dat_pointer(v, field, &target);
    if (result < 0) return -1;
    if (result == 0) return 0;
    if (*count >= MAX_NODES) return -1;
    todo[(*count)++] = (Node){target, (uint8_t)kind};
    return 0;
}
int mv_image_from_desc(const MvDat *v, uint32_t im, uint32_t tlut, MvImage *image)
{
    uint32_t palette = NONE, pixels;
    int has_tlut = tlut != NONE;
    const uint8_t *desc = mv_dat_span(v, im, 24);
    if (!desc || mv_dat_pointer(v, im, &pixels) != 1) return -1;
    memset(image, 0, sizeof(*image));
    image->image_desc = im; image->tlut_desc = tlut; image->pixels = pixels;
    image->width = mv_be16(desc + 4); image->height = mv_be16(desc + 6);
    image->format = mv_be32(desc + 8); image->palette = NONE;
    size_t bytes = mv_gx_texture_size(image->width, image->height, image->format);
    if (!bytes || !mv_dat_span(v, pixels, bytes)) return -1;
    if (has_tlut) {
        const uint8_t *p = mv_dat_span(v, tlut, 16);
        if (!p || mv_dat_pointer(v, tlut, &palette) != 1) return -1;
        image->palette = palette;
        image->palette_format = mv_be32(p + 4);
        image->palette_entries = mv_be16(p + 12);
        if (image->palette_format > 2 || !image->palette_entries ||
            !mv_dat_span(v, palette, (size_t)image->palette_entries * 2)) return -1;
    }
    if (image->format >= 8 && image->format <= 10 && !has_tlut) return -1;
    return 0;
}

static int read_image(const MvDat *v, uint32_t tobj, MvImage *image)
{
    uint32_t im, tlut = NONE;
    if (mv_dat_pointer(v, tobj + 0x4c, &im) != 1) return -1;
    int has_tlut = mv_dat_pointer(v, tobj + 0x50, &tlut);
    if (has_tlut < 0) return -1;
    return mv_image_from_desc(v, im, has_tlut ? tlut : NONE, image);
}
int mv_menu_textures(const MvDat *v, MvImage *images, size_t capacity, size_t *count)
{
    *count = 0;
    if (!images || !v->pointer_bits) return -1;
    Node *todo = malloc(MAX_NODES * sizeof(*todo));
    uint8_t *visited = calloc((size_t)v->data_size + 1, 1);
    if (!todo || !visited) { free(todo); free(visited); return -1; }
    size_t pending = 0, processed = 0, roots = 0;
    int result = -1;
    for (uint32_t i = 0; i < v->public_count; ++i) {
        const char *name;
        uint32_t offset;
        if (mv_dat_public(v, i, &name, &offset)) goto done;
        size_t length = strlen(name);
        if (length < 6 || strcmp(name + length - 6, "_joint") ||
            strstr(name, "matanim_joint") || strstr(name, "shapeanim_joint")) continue;
        if (pending >= MAX_NODES) goto done;
        todo[pending++] = (Node){offset, JOINT};
        ++roots;
    }
    while (pending) {
        Node node = todo[--pending];
        uint32_t offset = node.offset;
        size_t size = node.kind == JOINT ? 64 : node.kind == DOBJ ? 16 :
                      node.kind == MOBJ ? 24 : 92;
        const uint8_t *p = mv_dat_span(v, offset, size);
        if (!p) goto done;
        if (visited[offset] & node.kind) continue;
        visited[offset] |= node.kind;
        if (++processed > MAX_NODES) goto done;
        switch (node.kind) {
        case JOINT:
            if (push_pointer(v, offset + 8, JOINT, todo, &pending) ||
                push_pointer(v, offset + 12, JOINT, todo, &pending)) goto done;
            /* The union contains particles or a spline under these flags. */
            if (!(mv_be32(p + 4) & ((1u << 5) | (1u << 14))) &&
                push_pointer(v, offset + 16, DOBJ, todo, &pending)) goto done;
            break;
        case DOBJ:
            if (push_pointer(v, offset + 4, DOBJ, todo, &pending) ||
                push_pointer(v, offset + 8, MOBJ, todo, &pending)) goto done;
            break;
        case MOBJ:
            if (push_pointer(v, offset + 8, TOBJ, todo, &pending)) goto done;
            break;
        case TOBJ: {
            MvImage im;
            if (read_image(v, offset, &im)) goto done;
            size_t i;
            for (i = 0; i < *count; ++i)
                if (images[i].image_desc == im.image_desc && images[i].tlut_desc == im.tlut_desc) break;
            if (i == *count) {
                if (*count >= capacity) goto done;
                images[(*count)++] = im;
            }
            if (push_pointer(v, offset + 4, TOBJ, todo, &pending)) goto done;
            break;
        }
        default: goto done;
        }
    }
    result = roots ? 0 : -1;
done:
    free(todo); free(visited);
    if (result) *count = 0;
    return result;
}
int mv_image_decode(const MvDat *v, const MvImage *im, uint8_t *rgba,
                    size_t capacity, size_t stride)
{
    size_t bytes = mv_gx_texture_size(im->width, im->height, im->format);
    const uint8_t *pixels = mv_dat_span(v, im->pixels, bytes);
    size_t pal_size = (size_t)im->palette_entries * 2;
    const uint8_t *palette = im->palette == NONE ? NULL : mv_dat_span(v, im->palette, pal_size);
    if (!bytes || !pixels || (im->palette != NONE && !palette)) return -1;
    return mv_gx_decode(rgba, capacity, stride, pixels, bytes, im->width, im->height,
                        im->format, palette, pal_size, im->palette_format);
}

/* Serialized GX/HSD values used by the static scene adapter. Keeping these
   local avoids pulling the PowerPC-oriented Dolphin headers into the portable
   data library. */
enum {
    MV_GX_VA_PNMTXIDX = 0,
    MV_GX_VA_TEX0MTXIDX = 1,
    MV_GX_VA_TEX7MTXIDX = 8,
    MV_GX_VA_POS = 9,
    MV_GX_VA_NRM = 10,
    MV_GX_VA_CLR0 = 11,
    MV_GX_VA_CLR1 = 12,
    MV_GX_VA_TEX0 = 13,
    MV_GX_VA_TEX7 = 20,
    MV_GX_VA_NBT = 25,
    MV_GX_VA_NULL = 0xff,
    MV_GX_NONE = 0,
    MV_GX_DIRECT = 1,
    MV_GX_INDEX8 = 2,
    MV_GX_INDEX16 = 3,
    MV_GX_U8 = 0,
    MV_GX_S8 = 1,
    MV_GX_U16 = 2,
    MV_GX_S16 = 3,
    MV_GX_F32 = 4,
    MV_GX_NRM_NBT3 = 2,
    MV_GX_QUADS = 0x80,
    MV_GX_TRIANGLES = 0x90,
    MV_GX_TRIANGLESTRIP = 0x98,
    MV_GX_TRIANGLEFAN = 0xa0,
    MV_GX_LINES = 0xa8,
    MV_GX_LINESTRIP = 0xb0,
    MV_GX_POINTS = 0xb8,
};

enum {
    MV_JOBJ_CLASSICAL_SCALE = 1u << 3,
    MV_JOBJ_HIDDEN = 1u << 4,
    MV_JOBJ_PTCL = 1u << 5,
    MV_JOBJ_INSTANCE = 1u << 12,
    MV_JOBJ_SPLINE = 1u << 14,
    MV_JOBJ_USE_QUATERNION = 1u << 17,
    MV_JOBJ_JOINT_MASK = 3u << 21,
    MV_JOBJ_USER_DEF_MTX = 1u << 23,
    MV_JOBJ_BILLBOARD_MASK = 0xe00u | 0x2000u,
    MV_POBJ_TYPE_MASK = 0x3000u,
    MV_POBJ_CULL_BOTH = 0xc000u,
};

typedef struct {
    uint32_t attr, attr_type, comp_cnt, comp_type, array;
    uint8_t frac;
    uint16_t stride;
} MvAttr;

typedef struct {
    uint32_t offset;
    uint32_t anim_offset;
    float parent[12], parent_scl[3];
    uint8_t parent_has_scl, has_anim;
    int8_t branch_visibility;
} MvJointWork;

static float be_float(const uint8_t *p)
{
    uint32_t bits = mv_be32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int camera_position(const MvDat *v, uint32_t offset, float out[3])
{
    const uint8_t *p = mv_dat_span(v, offset, 20);
    if (!p) return -1;
    out[0] = be_float(p + 4); out[1] = be_float(p + 8); out[2] = be_float(p + 12);
    for (unsigned i = 0; i < 3; ++i) if (!isfinite(out[i])) return -1;
    uint32_t robj;
    int has_robj = mv_dat_pointer(v, offset + 16, &robj);
    if (has_robj < 0) return -1;
    return has_robj ? 1 : 0;
}

static int normalize3(float v[3])
{
    float mag2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!isfinite(mag2) || mag2 < 1.0e-12f) return -1;
    float inv = 1.0f / sqrtf(mag2);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
    return 0;
}

static void rotate_axis(float out[3], const float v[3], const float axis[3], float angle)
{
    float s = sinf(angle), c = cosf(angle), one_c = 1.0f - c;
    float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
    float cross[3] = {
        axis[1] * v[2] - axis[2] * v[1],
        axis[2] * v[0] - axis[0] * v[2],
        axis[0] * v[1] - axis[1] * v[0]
    };
    for (unsigned i = 0; i < 3; ++i)
        out[i] = v[i] * c + cross[i] * s + axis[i] * dot * one_c;
}

static int camera_up_from_roll(const float eye[3], const float interest[3],
                               float roll, float up[3])
{
    float look[3] = {interest[0] - eye[0], interest[1] - eye[1], interest[2] - eye[2]};
    if (normalize3(look) || !isfinite(roll)) return -1;
    float base[3];
    if (1.0f - fabsf(look[1]) < 0.0001f) {
        float d = sqrtf(look[1] * look[1] + look[2] * look[2]);
        if (d < 1.0e-6f) return -1;
        base[0] = d;
        base[1] = look[1] * (-look[0] / d);
        base[2] = look[2] * (-look[0] / d);
    } else {
        float d = sqrtf(look[0] * look[0] + look[2] * look[2]);
        if (d < 1.0e-6f) return -1;
        base[1] = d;
        base[0] = look[0] * (-look[1] / d);
        base[2] = look[2] * (-look[1] / d);
    }
    rotate_axis(up, base, look, -roll);
    return normalize3(up);
}

int mv_camera_read_at(const MvDat *v, uint32_t root, MvCamera *camera)
{
    if (!v || !camera || !v->pointer_bits || root > v->data_size) return -1;
    memset(camera, 0, sizeof(*camera));
    const uint8_t *p = mv_dat_span(v, root, 64);
    if (!p) return -1;
    camera->flags = mv_be16(p + 4);
    camera->projection_type = mv_be16(p + 6);
    camera->viewport_xmin = (int16_t)mv_be16(p + 8);
    camera->viewport_xmax = (int16_t)mv_be16(p + 10);
    camera->viewport_ymin = (int16_t)mv_be16(p + 12);
    camera->viewport_ymax = (int16_t)mv_be16(p + 14);
    camera->scissor_left = mv_be16(p + 16); camera->scissor_right = mv_be16(p + 18);
    camera->scissor_top = mv_be16(p + 20); camera->scissor_bottom = mv_be16(p + 22);
    uint32_t target;
    if (mv_dat_pointer(v, root + 0x18, &target) != 1 || camera_position(v, target, camera->eye)) return -1;
    if (mv_dat_pointer(v, root + 0x1c, &target) != 1 || camera_position(v, target, camera->interest)) return -1;
    if (camera->flags & 1) {
        if (mv_dat_pointer(v, root + 0x24, &target) != 1) return -1;
        const uint8_t *u = mv_dat_span(v, target, 12);
        if (!u) return -1;
        for (unsigned i = 0; i < 3; ++i) camera->up[i] = be_float(u + i * 4);
        if (normalize3(camera->up)) return -1;
    } else {
        if (camera_up_from_roll(camera->eye, camera->interest, be_float(p + 0x20), camera->up)) return -1;
    }
    camera->near_z = be_float(p + 0x28); camera->far_z = be_float(p + 0x2c);
    if (!isfinite(camera->near_z) || !isfinite(camera->far_z) || camera->near_z <= 0.0f ||
        camera->far_z <= camera->near_z) return -1;
    if (camera->projection_type == 1) {
        camera->fov_y = be_float(p + 0x30); camera->aspect = be_float(p + 0x34);
        if (!isfinite(camera->fov_y) || !isfinite(camera->aspect) || camera->fov_y <= 0.0f ||
            camera->fov_y >= 179.0f || camera->aspect <= 0.0f) return -1;
    } else if (camera->projection_type == 2 || camera->projection_type == 3) {
        camera->top = be_float(p + 0x30); camera->bottom = be_float(p + 0x34);
        camera->left = be_float(p + 0x38); camera->right = be_float(p + 0x3c);
        if (!isfinite(camera->top) || !isfinite(camera->bottom) || !isfinite(camera->left) ||
            !isfinite(camera->right) || camera->top == camera->bottom || camera->left == camera->right) return -1;
    } else return -1;
    return 0;
}

int mv_camera_read(const MvDat *v, const char *root_name, MvCamera *camera)
{
    if (!v || !root_name || !camera || !v->pointer_bits) return -1;
    uint32_t root = NONE;
    for (uint32_t i = 0; i < v->public_count; ++i) {
        const char *name; uint32_t offset;
        if (mv_dat_public(v, i, &name, &offset)) return -1;
        if (!strcmp(name, root_name)) { root = offset; break; }
    }
    if (root == NONE) return -1;
    return mv_camera_read_at(v, root, camera);
}

int mv_camera_visibility(const MvCamera *camera, const MvScene *scene,
                         MvCameraVisibility *visibility)
{
    if (!camera || !scene || !visibility) return -1;
    memset(visibility, 0, sizeof(*visibility));
    float forward[3] = {camera->interest[0] - camera->eye[0],
                        camera->interest[1] - camera->eye[1],
                        camera->interest[2] - camera->eye[2]};
    if (normalize3(forward)) return -1;
    for (size_t m = 0; m < scene->mesh_count; ++m) {
        const MvSceneMesh *mesh = &scene->meshes[m];
        if (mesh->first_vertex > scene->vertex_count ||
            mesh->vertex_count > scene->vertex_count - mesh->first_vertex) return -1;
        const MvSceneVertex *vertices = scene->vertices + mesh->first_vertex;
        for (size_t i = 0; i + 2 < mesh->vertex_count; i += 3) {
            unsigned inside = 0, near_count = 0, far_count = 0;
            for (unsigned j = 0; j < 3; ++j) {
                const MvSceneVertex *v = &vertices[i + j];
                float dx = v->x - camera->eye[0], dy = v->y - camera->eye[1],
                      dz = v->z - camera->eye[2];
                float depth = dx * forward[0] + dy * forward[1] + dz * forward[2];
                if (!isfinite(depth)) return -1;
                if (depth < camera->near_z) ++near_count;
                else if (depth > camera->far_z) ++far_count;
                else ++inside;
            }
            if (inside == 3) ++visibility->front;
            else if (near_count == 3) ++visibility->behind;
            else if (far_count == 3) ++visibility->far_count;
            else ++visibility->partial;
        }
    }
    return 0;
}

static void identity_mtx(float m[12])
{
    memset(m, 0, 12 * sizeof(*m));
    m[0] = m[5] = m[10] = 1.0f;
}

static void concat_mtx(const float a[12], const float b[12], float out[12])
{
    float r[12];
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 3; ++col) {
            r[row * 4 + col] = a[row * 4] * b[col] +
                               a[row * 4 + 1] * b[4 + col] +
                               a[row * 4 + 2] * b[8 + col];
        }
        r[row * 4 + 3] = a[row * 4] * b[3] +
                         a[row * 4 + 1] * b[7] +
                         a[row * 4 + 2] * b[11] + a[row * 4 + 3];
    }
    memcpy(out, r, sizeof(r));
}

/* Mirrors upstream HSD_MtxSRT for Euler Joint descriptors, including the
   inverse-parent-scale correction used by non-classical HSD hierarchies. */
static int make_srt(float m[12], const float scale[3], const float rotate[3],
                    const float translate[3], const float *parent_scl)
{
    for (unsigned i = 0; i < 3; ++i)
        if (!isfinite(scale[i]) || !isfinite(rotate[i]) || !isfinite(translate[i])) return -1;
    float sx0 = scale[0], sy0 = scale[1], sz0 = scale[2];
    float sx1 = scale[0], sy1 = scale[1], sz1 = scale[2];
    float sx2 = scale[0], sy2 = scale[1], sz2 = scale[2];
    if (parent_scl) {
        if (!isfinite(parent_scl[0]) || !isfinite(parent_scl[1]) ||
            !isfinite(parent_scl[2]) || parent_scl[0] == 0.0f ||
            parent_scl[1] == 0.0f || parent_scl[2] == 0.0f) return -1;
        sy2 *= parent_scl[1] / parent_scl[0];
        sz2 *= parent_scl[2] / parent_scl[0];
        sx1 *= parent_scl[0] / parent_scl[1];
        sz1 *= parent_scl[2] / parent_scl[1];
        sx0 *= parent_scl[0] / parent_scl[2];
        sy0 *= parent_scl[1] / parent_scl[2];
    }
    float sin_x = sinf(rotate[0]), cos_x = cosf(rotate[0]);
    float sin_y = sinf(rotate[1]), cos_y = cosf(rotate[1]);
    float sin_z = sinf(rotate[2]), cos_z = cosf(rotate[2]);
    m[0] = cos_z * (sx2 * cos_y);
    m[4] = sin_z * (sx1 * cos_y);
    m[8] = -sx0 * sin_y;
    m[1] = sy2 * ((cos_z * (sin_x * sin_y)) - (cos_x * sin_z));
    m[5] = sy1 * ((sin_z * (sin_x * sin_y)) + (cos_x * cos_z));
    m[9] = cos_y * (sy0 * sin_x);
    m[2] = sz2 * ((cos_z * (cos_x * sin_y)) + (sin_x * sin_z));
    m[6] = sz1 * ((sin_z * (cos_x * sin_y)) - (sin_x * cos_z));
    m[10] = cos_y * (sz0 * cos_x);
    m[3] = translate[0]; m[7] = translate[1]; m[11] = translate[2];
    return 0;
}

static void transform_point(const float m[12], float *x, float *y, float *z)
{
    float ox = *x, oy = *y, oz = *z;
    *x = m[0] * ox + m[1] * oy + m[2] * oz + m[3];
    *y = m[4] * ox + m[5] * oy + m[6] * oz + m[7];
    *z = m[8] * ox + m[9] * oy + m[10] * oz + m[11];
}

static int reserve_vertices(MvScene *scene, size_t additional)
{
    if (additional > MAX_SCENE_VERTICES - scene->vertex_count) return -1;
    size_t need = scene->vertex_count + additional;
    if (need <= scene->vertex_capacity) return 0;
    size_t capacity = scene->vertex_capacity ? scene->vertex_capacity : 1024;
    while (capacity < need) {
        size_t next = capacity * 2;
        if (next > MAX_SCENE_VERTICES) next = MAX_SCENE_VERTICES;
        if (next == capacity) return -1;
        capacity = next;
    }
    MvSceneVertex *vertices = malloc(capacity * sizeof(*vertices));
    if (!vertices) return -1;
    if (scene->vertex_count) memcpy(vertices, scene->vertices, scene->vertex_count * sizeof(*vertices));
    free(scene->vertices); scene->vertices = vertices; scene->vertex_capacity = capacity;
    return 0;
}

static int reserve_meshes(MvScene *scene, size_t additional)
{
    if (additional > MAX_SCENE_MESHES - scene->mesh_count) return -1;
    size_t need = scene->mesh_count + additional;
    if (need <= scene->mesh_capacity) return 0;
    size_t capacity = scene->mesh_capacity ? scene->mesh_capacity : 64;
    while (capacity < need) {
        size_t next = capacity * 2;
        if (next > MAX_SCENE_MESHES) next = MAX_SCENE_MESHES;
        if (next == capacity) return -1;
        capacity = next;
    }
    MvSceneMesh *meshes = malloc(capacity * sizeof(*meshes));
    if (!meshes) return -1;
    if (scene->mesh_count) memcpy(meshes, scene->meshes, scene->mesh_count * sizeof(*meshes));
    free(scene->meshes); scene->meshes = meshes; scene->mesh_capacity = capacity;
    return 0;
}

static int append_triangle(MvScene *scene, const MvSceneVertex *a,
                           const MvSceneVertex *b, const MvSceneVertex *c)
{
    if (reserve_vertices(scene, 3)) return -1;
    scene->vertices[scene->vertex_count++] = *a;
    scene->vertices[scene->vertex_count++] = *b;
    scene->vertices[scene->vertex_count++] = *c;
    return 0;
}

static int component_size(uint32_t type)
{
    switch (type) {
    case MV_GX_U8: case MV_GX_S8: return 1;
    case MV_GX_U16: case MV_GX_S16: return 2;
    case MV_GX_F32: return 4;
    default: return 0;
    }
}

static size_t color_size(uint32_t type)
{
    static const uint8_t sizes[] = {2, 3, 4, 2, 3, 4};
    return type < sizeof(sizes) ? sizes[type] : 0;
}

static size_t attr_element_size(const MvAttr *attr)
{
    if (attr->attr <= MV_GX_VA_TEX7MTXIDX) return 1;
    if (attr->attr == MV_GX_VA_CLR0 || attr->attr == MV_GX_VA_CLR1) return color_size(attr->comp_type);
    int bytes = component_size(attr->comp_type);
    if (!bytes) return 0;
    unsigned components;
    if (attr->attr == MV_GX_VA_POS) components = attr->comp_cnt ? 3 : 2;
    else if (attr->attr == MV_GX_VA_NRM || attr->attr == MV_GX_VA_NBT)
        components = attr->comp_cnt ? 9 : 3;
    else if (attr->attr >= MV_GX_VA_TEX0 && attr->attr <= MV_GX_VA_TEX7)
        components = attr->comp_cnt ? 2 : 1;
    else return 0;
    return (size_t)bytes * components;
}

static int read_scalar(const uint8_t *p, uint32_t type, uint8_t frac, float *value)
{
    float scale = frac < 31 ? 1.0f / (float)(1u << frac) : 0.0f;
    switch (type) {
    case MV_GX_U8: *value = (float)p[0] * scale; return 0;
    case MV_GX_S8: *value = (float)(int8_t)p[0] * scale; return 0;
    case MV_GX_U16: *value = (float)mv_be16(p) * scale; return 0;
    case MV_GX_S16: *value = (float)(int16_t)mv_be16(p) * scale; return 0;
    case MV_GX_F32: *value = be_float(p); return isfinite(*value) ? 0 : -1;
    default: return -1;
    }
}

static uint8_t expand5(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t expand6(unsigned v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint8_t expand4(unsigned v) { return (uint8_t)((v << 4) | v); }
static uint32_t pack_rgba(unsigned r, unsigned g, unsigned b, unsigned a)
{ return (r << 24) | (g << 16) | (b << 8) | a; }

static int read_color(const uint8_t *p, uint32_t type, uint32_t *rgba)
{
    switch (type) {
    case 0: {
        uint16_t v = mv_be16(p);
        *rgba = pack_rgba(expand5(v >> 11), expand6((v >> 5) & 63), expand5(v & 31), 255);
        return 0;
    }
    case 1: *rgba = pack_rgba(p[0], p[1], p[2], 255); return 0;
    case 2: *rgba = pack_rgba(p[0], p[1], p[2], 255); return 0;
    case 3: {
        uint16_t v = mv_be16(p);
        *rgba = pack_rgba(expand4(v >> 12), expand4((v >> 8) & 15),
                          expand4((v >> 4) & 15), expand4(v & 15));
        return 0;
    }
    case 4: {
        uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        *rgba = pack_rgba(expand6(v >> 18), expand6((v >> 12) & 63),
                          expand6((v >> 6) & 63), expand6(v & 63));
        return 0;
    }
    case 5: *rgba = pack_rgba(p[0], p[1], p[2], p[3]); return 0;
    default: return -1;
    }
}

static int read_attrs(const MvDat *v, uint32_t offset, MvAttr attrs[MAX_VERTEX_ATTRS], size_t *count)
{
    *count = 0;
    for (unsigned i = 0; i < MAX_VERTEX_ATTRS; ++i, offset += 24) {
        const uint8_t *p = mv_dat_span(v, offset, 24);
        if (!p) return -1;
        uint32_t attr = mv_be32(p);
        if (attr == MV_GX_VA_NULL) return *count ? 0 : -1;
        MvAttr *out = &attrs[(*count)++];
        out->attr = attr; out->attr_type = mv_be32(p + 4);
        out->comp_cnt = mv_be32(p + 8); out->comp_type = mv_be32(p + 12);
        out->frac = p[16]; out->stride = mv_be16(p + 18); out->array = NONE;
        if (out->attr_type > MV_GX_INDEX16 || out->attr_type == MV_GX_NONE) return 1;
        if (out->attr_type != MV_GX_DIRECT) {
            if (mv_dat_pointer(v, offset + 20, &out->array) != 1 || !out->stride) return 1;
        }
        if (!attr_element_size(out)) return 1;
    }
    return 1;
}

static int indexed_source(const MvDat *v, const MvAttr *attr, uint32_t index,
                          const uint8_t **source)
{
    size_t element = attr_element_size(attr);
    uint64_t offset = (uint64_t)attr->array + (uint64_t)index * attr->stride;
    if (!element || offset > UINT32_MAX) return -1;
    *source = mv_dat_span(v, (uint32_t)offset, element);
    return *source ? 0 : -1;
}

/* 0 success, 1 unsupported matrix selection/format, -1 malformed stream. */
static int decode_display_vertex(const MvDat *v, const MvAttr *attrs, size_t attr_count,
                                 const uint8_t **cursor, const uint8_t *end,
                                 uint32_t material, const float world[12],
                                 MvSceneVertex *vertex)
{
    memset(vertex, 0, sizeof(*vertex));
    vertex->rgba = material;
    int have_pos = 0;
    for (size_t i = 0; i < attr_count; ++i) {
        const MvAttr *attr = &attrs[i];
        const uint8_t *source = NULL;
        size_t element = attr_element_size(attr);
        if (attr->attr_type == MV_GX_DIRECT) {
            if ((size_t)(end - *cursor) < element) return -1;
            source = *cursor; *cursor += element;
        } else {
            unsigned indices = (attr->attr == MV_GX_VA_NRM && attr->comp_cnt == MV_GX_NRM_NBT3) ? 3 : 1;
            size_t index_bytes = attr->attr_type == MV_GX_INDEX8 ? 1 : 2;
            if ((size_t)(end - *cursor) < index_bytes * indices) return -1;
            uint32_t index = index_bytes == 1 ? (*cursor)[0] : mv_be16(*cursor);
            *cursor += index_bytes * indices;
            if (indexed_source(v, attr, index, &source)) return -1;
        }
        if (attr->attr <= MV_GX_VA_TEX7MTXIDX) {
            if (source[0] != 0) return 1; /* Requires HSD matrix palette/texgen runtime. */
        } else if (attr->attr == MV_GX_VA_POS) {
            int bytes = component_size(attr->comp_type);
            if (!bytes || read_scalar(source, attr->comp_type, attr->frac, &vertex->x) ||
                read_scalar(source + bytes, attr->comp_type, attr->frac, &vertex->y)) return -1;
            if (attr->comp_cnt && read_scalar(source + bytes * 2, attr->comp_type, attr->frac, &vertex->z)) return -1;
            have_pos = 1;
        } else if (attr->attr == MV_GX_VA_CLR0) {
            if (read_color(source, attr->comp_type, &vertex->rgba)) return 1;
        } else if (attr->attr == MV_GX_VA_TEX0) {
            int bytes = component_size(attr->comp_type);
            if (!bytes || read_scalar(source, attr->comp_type, attr->frac, &vertex->u)) return -1;
            if (attr->comp_cnt && read_scalar(source + bytes, attr->comp_type, attr->frac, &vertex->v)) return -1;
        }
    }
    if (!have_pos) return 1;
    transform_point(world, &vertex->x, &vertex->y, &vertex->z);
    return 0;
}

static int append_primitive(MvScene *scene, uint8_t primitive, const MvSceneVertex *v, size_t count)
{
    switch (primitive) {
    case MV_GX_TRIANGLES:
        if (count % 3) return 1;
        for (size_t i = 0; i < count; i += 3)
            if (append_triangle(scene, &v[i], &v[i + 1], &v[i + 2])) return -1;
        return 0;
    case MV_GX_QUADS:
        if (count % 4) return 1;
        for (size_t i = 0; i < count; i += 4) {
            if (append_triangle(scene, &v[i], &v[i + 1], &v[i + 2]) ||
                append_triangle(scene, &v[i], &v[i + 2], &v[i + 3])) return -1;
        }
        return 0;
    case MV_GX_TRIANGLESTRIP:
        if (count < 3) return 0;
        for (size_t i = 2; i < count; ++i) {
            const MvSceneVertex *a = &v[i - 2], *b = &v[i - 1], *c = &v[i];
            if (i & 1) { const MvSceneVertex *swap = a; a = b; b = swap; }
            if (append_triangle(scene, a, b, c)) return -1;
        }
        return 0;
    case MV_GX_TRIANGLEFAN:
        if (count < 3) return 0;
        for (size_t i = 2; i < count; ++i)
            if (append_triangle(scene, &v[0], &v[i - 1], &v[i])) return -1;
        return 0;
    case MV_GX_LINES: case MV_GX_LINESTRIP: case MV_GX_POINTS:
        return 2;
    default: return 1;
    }
}

static int material_info(const MvDat *v, uint32_t mobj, uint32_t *rgba,
                         uint32_t *tobj_desc, uint32_t *image_desc, uint32_t *tlut_desc,
                         uint32_t *rendermode, uint32_t *tobj_flags, float *blending,
                         uint8_t *texture_count)
{
    *rgba = 0xffffffffu; *tobj_desc = NONE; *image_desc = NONE; *tlut_desc = NONE;
    *rendermode = 0; *tobj_flags = 0; *blending = 1.0f; *texture_count = 0;
    if (mobj == NONE) return 0;
    const uint8_t *m = mv_dat_span(v, mobj, 24);
    if (!m) return -1;
    *rendermode = mv_be32(m + 4);
    uint32_t material;
    int has_material = mv_dat_pointer(v, mobj + 12, &material);
    if (has_material < 0) return -1;
    if (has_material) {
        const uint8_t *p = mv_dat_span(v, material, 20);
        if (!p) return -1;
        float alpha = be_float(p + 12);
        if (!isfinite(alpha)) return -1;
        if (alpha < 0.0f) alpha = 0.0f; else if (alpha > 1.0f) alpha = 1.0f;
        unsigned a = (unsigned)((float)p[7] * alpha + 0.5f);
        *rgba = pack_rgba(p[4], p[5], p[6], a);
    }
    uint32_t tobj;
    int has_texture = mv_dat_pointer(v, mobj + 8, &tobj);
    if (has_texture < 0) return -1;
    if (has_texture) {
        *tobj_desc = tobj;
        const uint8_t *td = mv_dat_span(v, tobj, 92);
        if (!td) return -1;
        *tobj_flags = mv_be32(td + 64);
        *blending = be_float(td + 68);
        if (!isfinite(*blending)) return -1;
        MvImage image;
        if (read_image(v, tobj, &image)) return -1;
        *image_desc = image.image_desc; *tlut_desc = image.tlut_desc;
        uint32_t current = tobj;
        for (unsigned guard = 0; current != NONE && guard < 255; ++guard) {
            ++*texture_count;
            uint32_t next;
            int has_next = mv_dat_pointer(v, current + 4, &next);
            if (has_next < 0) return -1;
            if (!has_next) break;
            if (next == current || !mv_dat_span(v, next, 92)) return -1;
            current = next;
            if (guard == 254) return -1;
        }
    }
    return 0;
}

/* Returns 0 for a decoded PObj, 1 for an explicitly unsupported PObj, -1 for
   malformed data/resource exhaustion. */
static int decode_pobj(const MvDat *v, uint32_t joint, uint32_t dobj, uint32_t mobj,
                       uint32_t pobj, const float world[12], MvScene *scene)
{
    const uint8_t *p = mv_dat_span(v, pobj, 24);
    if (!p) return -1;
    uint16_t flags = mv_be16(p + 12), n_display = mv_be16(p + 14);
    if ((flags & MV_POBJ_CULL_BOTH) == MV_POBJ_CULL_BOTH) return 0;
    if (flags & MV_POBJ_TYPE_MASK) return 1; /* shape animation/envelope need runtime adapters */
    uint32_t shared;
    int has_shared = mv_dat_pointer(v, pobj + 20, &shared);
    if (has_shared < 0) return -1;
    if (has_shared) return 1; /* SetupSharedVtxModelMtx path is not rigid Joint SRT. */
    if (!n_display) return 0;
    uint32_t attrs_offset, display_offset;
    if (mv_dat_pointer(v, pobj + 8, &attrs_offset) != 1 ||
        mv_dat_pointer(v, pobj + 16, &display_offset) != 1) return -1;
    MvAttr attrs[MAX_VERTEX_ATTRS]; size_t attr_count;
    int attr_result = read_attrs(v, attrs_offset, attrs, &attr_count);
    if (attr_result) return attr_result < 0 ? -1 : 1;
    size_t display_size = (size_t)n_display << 5;
    const uint8_t *display = mv_dat_span(v, display_offset, display_size);
    if (!display) return -1;
    uint32_t material, tobj_desc, image_desc, tlut_desc, rendermode, tobj_flags;
    float blending;
    uint8_t texture_count;
    if (material_info(v, mobj, &material, &tobj_desc, &image_desc, &tlut_desc,
                      &rendermode, &tobj_flags, &blending, &texture_count)) return -1;
    size_t first = scene->vertex_count;
    const uint8_t *cursor = display, *end = display + display_size;
    while (cursor < end) {
        uint8_t command = *cursor++;
        if (!command) continue; /* GX display lists are padded to 32-byte units. */
        uint8_t primitive = command & 0xf8u;
        if ((command & 7u) != 0 || primitive < MV_GX_QUADS || primitive > MV_GX_POINTS ||
            (size_t)(end - cursor) < 2) { scene->vertex_count = first; return 1; }
        uint16_t count = mv_be16(cursor); cursor += 2;
        if (!count) continue;
        MvSceneVertex *input = malloc((size_t)count * sizeof(*input));
        if (!input) { scene->vertex_count = first; return -1; }
        int result = 0;
        for (uint16_t i = 0; i < count; ++i) {
            result = decode_display_vertex(v, attrs, attr_count, &cursor, end,
                                           material, world, &input[i]);
            if (result) break;
        }
        if (!result) result = append_primitive(scene, primitive, input, count);
        free(input);
        if (result < 0) { scene->vertex_count = first; return -1; }
        if (result == 1) { scene->vertex_count = first; return 1; }
        if (result == 2) ++scene->skipped_primitive_count;
        else ++scene->primitive_count;
    }
    if (scene->vertex_count == first) return 0;
    if (reserve_meshes(scene, 1)) { scene->vertex_count = first; return -1; }
    MvSceneMesh *mesh = &scene->meshes[scene->mesh_count++];
    mesh->joint_desc = joint; mesh->dobj_desc = dobj; mesh->mobj_desc = mobj;
    mesh->pobj_desc = pobj; mesh->tobj_desc = tobj_desc;
    mesh->image_desc = image_desc; mesh->tlut_desc = tlut_desc;
    mesh->rendermode = rendermode; mesh->tobj_flags = tobj_flags;
    mesh->texture_blending = blending; mesh->texture_count = texture_count;
    mesh->material_rgba = material; mesh->first_vertex = first;
    mesh->vertex_count = scene->vertex_count - first;
    if (image_desc != NONE) ++scene->textured_mesh_count;
    return 0;
}

static int decode_dobjs(const MvDat *v, uint32_t joint, uint32_t first,
                        const float world[12], MvScene *scene)
{
    uint32_t dobj = first;
    for (unsigned dguard = 0; dobj != NONE && dguard < MAX_NODES; ++dguard) {
        const uint8_t *p = mv_dat_span(v, dobj, 16);
        if (!p) return -1;
        ++scene->dobj_count;
        uint32_t mobj = NONE, pobj = NONE, next = NONE;
        int has_mobj = mv_dat_pointer(v, dobj + 8, &mobj);
        int has_pobj = mv_dat_pointer(v, dobj + 12, &pobj);
        int has_next = mv_dat_pointer(v, dobj + 4, &next);
        if (has_mobj < 0 || has_pobj < 0 || has_next < 0) return -1;
        if (!has_mobj) mobj = NONE;
        if (has_pobj) {
            uint32_t po = pobj;
            for (unsigned pguard = 0; po != NONE && pguard < MAX_NODES; ++pguard) {
                ++scene->pobj_count;
                int result = decode_pobj(v, joint, dobj, mobj, po, world, scene);
                if (result < 0) return -1;
                if (result > 0) ++scene->skipped_pobj_count;
                uint32_t next_po;
                int has_next_po = mv_dat_pointer(v, po + 4, &next_po);
                if (has_next_po < 0) return -1;
                if (!has_next_po) break;
                if (next_po == po) return -1;
                po = next_po;
                if (pguard + 1 == MAX_NODES) return -1;
            }
        }
        if (!has_next) break;
        if (next == dobj) return -1;
        dobj = next;
        if (dguard + 1 == MAX_NODES) return -1;
    }
    return 0;
}

static void compute_bounds(MvScene *scene)
{
    scene->bounds_valid = 0;
    for (size_t i = 0; i < scene->vertex_count; ++i) {
        const MvSceneVertex *v = &scene->vertices[i];
        if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z)) continue;
        if (!scene->bounds_valid) {
            scene->min_x = scene->max_x = v->x; scene->min_y = scene->max_y = v->y;
            scene->min_z = scene->max_z = v->z; scene->bounds_valid = 1;
        } else {
            if (v->x < scene->min_x) scene->min_x = v->x;
            if (v->x > scene->max_x) scene->max_x = v->x;
            if (v->y < scene->min_y) scene->min_y = v->y;
            if (v->y > scene->max_y) scene->max_y = v->y;
            if (v->z < scene->min_z) scene->min_z = v->z;
            if (v->z > scene->max_z) scene->max_z = v->z;
        }
    }
}

void mv_scene_free(MvScene *scene)
{
    if (!scene) return;
    free(scene->vertices); free(scene->meshes); memset(scene, 0, sizeof(*scene));
}

static int find_public_root(const MvDat *v, const char *name, uint32_t *root)
{
    *root = NONE;
    for (uint32_t i = 0; i < v->public_count; ++i) {
        const char *symbol;
        uint32_t offset;
        if (mv_dat_public(v, i, &symbol, &offset)) return -1;
        if (!strcmp(symbol, name)) {
            *root = offset;
            return 0;
        }
    }
    return -1;
}

static void apply_anim_channels(const MvAnimJointSample *anim, float rotate[3],
                                float translate[3], float scale[3])
{
    if (anim->channel_mask & (1u << (1 - 1))) rotate[0] = anim->channels[1 - 1];
    if (anim->channel_mask & (1u << (2 - 1))) rotate[1] = anim->channels[2 - 1];
    if (anim->channel_mask & (1u << (3 - 1))) rotate[2] = anim->channels[3 - 1];
    if (anim->channel_mask & (1u << (5 - 1))) translate[0] = anim->channels[5 - 1];
    if (anim->channel_mask & (1u << (6 - 1))) translate[1] = anim->channels[6 - 1];
    if (anim->channel_mask & (1u << (7 - 1))) translate[2] = anim->channels[7 - 1];
    for (unsigned channel = 8; channel <= 10; ++channel) {
        if (!(anim->channel_mask & (1u << (channel - 1)))) continue;
        float value = anim->channels[channel - 1];
        /* JObjUpdateFunc clamps animated scale channels before dirtying mtx. */
        if (fabsf(value) < 1.0e-3f) value = 1.0e-3f;
        scale[channel - 8] = value;
    }
}

static int mv_scene_build_internal(const MvDat *v, const char *root_name,
                                   const char *anim_root_name, float anim_frame,
                                   MvScene *scene)
{
    if (!v || !root_name || !scene || !v->pointer_bits) return -1;
    memset(scene, 0, sizeof(*scene));
    if (!isfinite(anim_frame)) return -1;
    uint32_t root, anim_root = NONE;
    if (find_public_root(v, root_name, &root)) goto fail;
    if (anim_root_name && find_public_root(v, anim_root_name, &anim_root)) goto fail;
    scene->animated = anim_root_name != NULL;
    scene->anim_frame = anim_frame;
    MvJointWork *todo = malloc(MAX_NODES * sizeof(*todo));
    uint8_t *visited = calloc((size_t)v->data_size + 1, 1);
    if (!todo || !visited) { free(todo); free(visited); goto fail; }
    size_t pending = 1, processed = 0;
    todo[0].offset = root; identity_mtx(todo[0].parent);
    todo[0].anim_offset = anim_root;
    todo[0].has_anim = anim_root_name != NULL;
    todo[0].branch_visibility = -1;
    todo[0].parent_scl[0] = todo[0].parent_scl[1] = todo[0].parent_scl[2] = 1.0f;
    todo[0].parent_has_scl = 0;
    while (pending) {
        MvJointWork work = todo[--pending];
        uint32_t joint = work.offset;
        const uint8_t *p = mv_dat_span(v, joint, 64);
        if (!p || joint > v->data_size || visited[joint]) goto traversal_fail;
        visited[joint] = 1;
        if (++processed > MAX_NODES) goto traversal_fail;
        ++scene->joint_count;
        uint32_t flags = mv_be32(p + 4), child = NONE, next = NONE;
        if (work.branch_visibility == 0)
            flags |= MV_JOBJ_HIDDEN;
        else if (work.branch_visibility > 0)
            flags &= ~MV_JOBJ_HIDDEN;
        int has_child = mv_dat_pointer(v, joint + 8, &child);
        int has_next = mv_dat_pointer(v, joint + 12, &next);
        if (has_child < 0 || has_next < 0) goto traversal_fail;
        MvAnimJointSample anim;
        memset(&anim, 0, sizeof(anim));
        if (work.has_anim) {
            int anim_result = mv_anim_joint_sample(v, work.anim_offset, anim_frame, &anim);
            if (anim_result < 0) goto traversal_fail;
            if (anim_result > 0) {
                ++scene->unsupported_anim_count;
                goto traversal_fail;
            }
            ++scene->anim_joint_count;
            scene->anim_aobj_count += anim.aobj_count;
            scene->anim_fobj_count += anim.fobj_count;
            scene->anim_channel_count += anim.applied_channel_count;
            /* HSD_JObjAddAnim applies this AnimJoint flag before frame 0. */
            if (anim.flags & 1u) flags |= MV_JOBJ_CLASSICAL_SCALE;
            else flags &= ~MV_JOBJ_CLASSICAL_SCALE;
            /* Match JObjUpdateFunc. NODE changes only this JObj while BRANCH
             * applies the same HIDDEN state recursively to this whole
             * subtree. Siblings keep the inherited state from their parent. */
            if (anim.channel_mask & (1u << (11 - 1))) {
                if (anim.channels[11 - 1] > 0.5f) flags &= ~MV_JOBJ_HIDDEN;
                else flags |= MV_JOBJ_HIDDEN;
            }
            if (anim.channel_mask & (1u << (12 - 1))) {
                if (anim.channels[12 - 1] > 0.5f) flags &= ~MV_JOBJ_HIDDEN;
                else flags |= MV_JOBJ_HIDDEN;
            }
        }
        int unsupported = (flags & (MV_JOBJ_INSTANCE | MV_JOBJ_USE_QUATERNION |
                                    MV_JOBJ_JOINT_MASK | MV_JOBJ_USER_DEF_MTX |
                                    MV_JOBJ_BILLBOARD_MASK)) != 0;
        float world[12], current_scl[3]; int current_has_scl = 0;
        if (!unsupported) {
            float scale[3] = {be_float(p + 0x20), be_float(p + 0x24), be_float(p + 0x28)};
            float rotate[3] = {be_float(p + 0x14), be_float(p + 0x18), be_float(p + 0x1c)};
            float translate[3] = {be_float(p + 0x2c), be_float(p + 0x30), be_float(p + 0x34)};
            if (work.has_anim) {
                apply_anim_channels(&anim, rotate, translate, scale);
            }
            float local[12];
            if (make_srt(local, scale, rotate, translate,
                         work.parent_has_scl ? work.parent_scl : NULL)) unsupported = 1;
            else {
                concat_mtx(work.parent, local, world);
                if (flags & MV_JOBJ_CLASSICAL_SCALE) {
                    current_has_scl = work.parent_has_scl;
                    memcpy(current_scl, work.parent_scl, sizeof(current_scl));
                } else {
                    current_has_scl = 1;
                    for (unsigned i = 0; i < 3; ++i)
                        current_scl[i] = scale[i] * (work.parent_has_scl ? work.parent_scl[i] : 1.0f);
                }
            }

            if (!unsupported && has_child && !(flags & MV_JOBJ_INSTANCE)) {
                if (pending >= MAX_NODES) goto traversal_fail;
                MvJointWork *child_work = &todo[pending++];
                child_work->offset = child; memcpy(child_work->parent, world, sizeof(world));
                memcpy(child_work->parent_scl, current_scl, sizeof(current_scl));
                child_work->parent_has_scl = (uint8_t)current_has_scl;
                child_work->branch_visibility = work.branch_visibility;
                if (work.has_anim && (anim.channel_mask & (1u << (12 - 1))))
                    child_work->branch_visibility = anim.channels[12 - 1] > 0.5f ? 1 : 0;
                if (work.has_anim) {
                    child_work->has_anim = anim.has_child;
                    child_work->anim_offset = anim.child;
                } else {
                    child_work->has_anim = 0;
                    child_work->anim_offset = NONE;
                }
            }
        }
        if (has_next) {
            if (pending >= MAX_NODES) goto traversal_fail;
            MvJointWork *next_work = &todo[pending++];
            *next_work = work;
            next_work->offset = next;
            if (work.has_anim) {
                next_work->has_anim = anim.has_next;
                next_work->anim_offset = anim.next;
            }
        }
        if (unsupported) {
            ++scene->unsupported_joint_count;
            /* Without the runtime transform, children would inherit a false matrix.
               Siblings remain safe because they use the original parent context. */
            continue;
        }
        if (!(flags & (MV_JOBJ_PTCL | MV_JOBJ_SPLINE | MV_JOBJ_HIDDEN))) {
            uint32_t dobj;
            int has_dobj = mv_dat_pointer(v, joint + 16, &dobj);
            if (has_dobj < 0) goto traversal_fail;
            if (has_dobj && decode_dobjs(v, joint, dobj, world, scene)) goto traversal_fail;
        }
    }
    /* HSD_JObjAddAnimAll permits the AnimJoint tree to end before the Joint
     * tree. Remaining joints are still visited with ajoint == NULL and keep
     * their static R/T/S, so partial animation trees are valid. */
    free(todo); free(visited); compute_bounds(scene);
    return scene->mesh_count && scene->vertex_count ? 0 : -1;
traversal_fail:
    free(todo); free(visited);
fail:
    mv_scene_free(scene); return -1;
}

int mv_scene_build(const MvDat *v, const char *root_name, MvScene *scene)
{
    return mv_scene_build_internal(v, root_name, NULL, 0.0f, scene);
}

int mv_scene_build_animated(const MvDat *v, const char *root_name,
                            const char *anim_root_name, float frame, MvScene *scene)
{
    if (!anim_root_name) return -1;
    return mv_scene_build_internal(v, root_name, anim_root_name, frame, scene);
}

int mv_scene_build_frame0(const MvDat *v, const char *root_name,
                          const char *anim_root_name, MvScene *scene)
{
    return mv_scene_build_animated(v, root_name, anim_root_name, 0.0f, scene);
}

int mv_scene_stats(const MvDat *v, const char *root_name,
                   uint32_t stats[MV_SCENE_STAT_COUNT])
{
    if (!stats) return -1;
    memset(stats, 0, MV_SCENE_STAT_COUNT * sizeof(*stats));
    MvScene scene;
    if (mv_scene_build(v, root_name, &scene)) return -1;
    stats[MV_SCENE_STAT_MESHES] = (uint32_t)scene.mesh_count;
    stats[MV_SCENE_STAT_VERTICES] = (uint32_t)scene.vertex_count;
    stats[MV_SCENE_STAT_TRIANGLES] = (uint32_t)(scene.vertex_count / 3);
    stats[MV_SCENE_STAT_JOINTS] = (uint32_t)scene.joint_count;
    stats[MV_SCENE_STAT_DOBJS] = (uint32_t)scene.dobj_count;
    stats[MV_SCENE_STAT_POBJS] = (uint32_t)scene.pobj_count;
    stats[MV_SCENE_STAT_TEXTURED_MESHES] = (uint32_t)scene.textured_mesh_count;
    stats[MV_SCENE_STAT_SKIPPED_POBJS] = (uint32_t)scene.skipped_pobj_count;
    stats[MV_SCENE_STAT_SKIPPED_PRIMITIVES] = (uint32_t)scene.skipped_primitive_count;
    stats[MV_SCENE_STAT_UNSUPPORTED_JOINTS] = (uint32_t)scene.unsupported_joint_count;
    mv_scene_free(&scene); return 0;
}

int mv_scene_animated_stats(const MvDat *v, const char *root_name,
                            const char *anim_root_name, float frame,
                            uint32_t stats[MV_ANIM_STAT_COUNT])
{
    if (!stats) return -1;
    memset(stats, 0, MV_ANIM_STAT_COUNT * sizeof(*stats));
    MvScene scene;
    if (mv_scene_build_animated(v, root_name, anim_root_name, frame, &scene)) return -1;
    stats[MV_ANIM_STAT_MESHES] = (uint32_t)scene.mesh_count;
    stats[MV_ANIM_STAT_TRIANGLES] = (uint32_t)(scene.vertex_count / 3);
    stats[MV_ANIM_STAT_JOINTS] = (uint32_t)scene.joint_count;
    stats[MV_ANIM_STAT_ANIM_JOINTS] = (uint32_t)scene.anim_joint_count;
    stats[MV_ANIM_STAT_AOBJS] = (uint32_t)scene.anim_aobj_count;
    stats[MV_ANIM_STAT_FOBJS] = (uint32_t)scene.anim_fobj_count;
    stats[MV_ANIM_STAT_CHANNELS] = (uint32_t)scene.anim_channel_count;
    stats[MV_ANIM_STAT_UNSUPPORTED] = (uint32_t)scene.unsupported_anim_count;
    mv_scene_free(&scene);
    return 0;
}

int mv_scene_frame0_stats(const MvDat *v, const char *root_name,
                          const char *anim_root_name,
                          uint32_t stats[MV_ANIM_STAT_COUNT])
{
    return mv_scene_animated_stats(v, root_name, anim_root_name, 0.0f, stats);
}
