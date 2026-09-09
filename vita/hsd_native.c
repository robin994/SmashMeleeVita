#include "hsd_native.h"
#include "gx_texture.h"

#include <stdlib.h>
#include <string.h>

/* Keep this portable conversion unit independent from the GameCube SDK
   headers. Those headers intentionally model the original 32-bit runtime and
   conflict with the macOS host libc in sanitizer builds. These definitions
   mirror the upstream descriptor field order/types; ARM32 size assertions at
   the bottom make layout drift a build failure on Vita. */
typedef struct { float x, y, z; } MvVec3;
typedef struct { uint8_t r, g, b, a; } MvGXColor;
typedef float MvMtx[3][4];
typedef float (*MvMtxPtr)[4];

typedef struct HSD_DObjDesc HSD_DObjDesc;
typedef struct HSD_MObjDesc HSD_MObjDesc;
typedef struct HSD_PObjDesc HSD_PObjDesc;
typedef struct HSD_TObjDesc HSD_TObjDesc;
typedef struct HSD_VtxDescList HSD_VtxDescList;
typedef struct HSD_ImageDesc HSD_ImageDesc;
typedef struct HSD_TlutDesc HSD_TlutDesc;
typedef struct HSD_TexLODDesc HSD_TexLODDesc;
typedef struct HSD_TObjTevDesc HSD_TObjTevDesc;
typedef struct HSD_Material HSD_Material;
typedef struct HSD_PEDesc HSD_PEDesc;

struct HSD_VtxDescList {
    int32_t attr, attr_type, comp_cnt, comp_type;
    uint8_t frac;
    uint16_t stride;
    void *vertex;
};

struct HSD_PObjDesc {
    char *class_name;
    HSD_PObjDesc *next;
    HSD_VtxDescList *verts;
    uint16_t flags, n_display;
    uint8_t *display;
    union { HSD_Joint *joint; void *shape_set; void *envelope_p; } u;
};

struct HSD_Material {
    MvGXColor ambient, diffuse, specular;
    float alpha, shininess;
};

struct HSD_PEDesc {
    uint8_t flags, ref0, ref1, dst_alpha, type, src_factor, dst_factor,
            logic_op, z_comp, alpha_comp0, alpha_op, alpha_comp1;
};

struct HSD_ImageDesc {
    void *image_ptr;
    uint16_t width, height;
    int32_t format;
    uint32_t mipmap;
    float minLOD, maxLOD;
};

struct HSD_TlutDesc {
    void *lut;
    int32_t fmt;
    uint32_t tlut_name;
    uint16_t n_entries;
};

struct HSD_TexLODDesc {
    int32_t minFilt;
    float LODBias;
    uint8_t bias_clamp, edgeLODEnable;
    int32_t max_anisotropy;
};

struct HSD_TObjTevDesc {
    uint8_t color_op, alpha_op, color_bias, alpha_bias, color_scale, alpha_scale,
            color_clamp, alpha_clamp, color_a, color_b, color_c, color_d,
            alpha_a, alpha_b, alpha_c, alpha_d;
    MvGXColor konst, tev0, tev1;
    uint32_t active;
};

struct HSD_TObjDesc {
    char *class_name;
    HSD_TObjDesc *next;
    int32_t id, src;
    MvVec3 rotate, scale, translate;
    int32_t wrap_s, wrap_t;
    uint8_t repeat_s, repeat_t;
    uint32_t blend_flags;
    float blending;
    int32_t magFilt;
    HSD_ImageDesc *imagedesc;
    HSD_TlutDesc *tlutdesc;
    HSD_TexLODDesc *lod;
    HSD_TObjTevDesc *tev;
};

struct HSD_MObjDesc {
    char *class_name;
    uint32_t rendermode;
    HSD_TObjDesc *texdesc;
    HSD_Material *mat;
    void *renderdesc;
    HSD_PEDesc *pedesc;
};

struct HSD_DObjDesc {
    char *class_name;
    HSD_DObjDesc *next;
    HSD_MObjDesc *mobjdesc;
    HSD_PObjDesc *pobjdesc;
};

struct HSD_Joint {
    char *class_name;
    uint32_t flags;
    HSD_Joint *child, *next;
    union { HSD_DObjDesc *dobjdesc; void *spline; void *ptcl; } u;
    MvVec3 rotation, scale, position;
    MvMtxPtr mtx;
    void *robjdesc;
};

enum { GX_VA_NULL = 0xff, GX_DIRECT = 1 };

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(HSD_Joint) == 64, "HSD_Joint ARM32 layout");
_Static_assert(sizeof(HSD_DObjDesc) == 16, "HSD_DObjDesc ARM32 layout");
_Static_assert(sizeof(HSD_MObjDesc) == 24, "HSD_MObjDesc ARM32 layout");
_Static_assert(sizeof(HSD_PObjDesc) == 24, "HSD_PObjDesc ARM32 layout");
_Static_assert(sizeof(HSD_VtxDescList) == 24, "HSD_VtxDescList ARM32 layout");
_Static_assert(sizeof(HSD_TObjDesc) == 92, "HSD_TObjDesc ARM32 layout");
_Static_assert(sizeof(HSD_ImageDesc) == 24, "HSD_ImageDesc ARM32 layout");
_Static_assert(sizeof(HSD_TlutDesc) == 16, "HSD_TlutDesc ARM32 layout");
_Static_assert(sizeof(HSD_TexLODDesc) == 16, "HSD_TexLODDesc ARM32 layout");
_Static_assert(sizeof(HSD_TObjTevDesc) == 32, "HSD_TObjTevDesc ARM32 layout");
_Static_assert(sizeof(HSD_Material) == 20, "HSD_Material ARM32 layout");
_Static_assert(sizeof(HSD_PEDesc) == 12, "HSD_PEDesc ARM32 layout");
#endif

#define MV_NATIVE_MAX_NODES 32768u
#define MV_NATIVE_MAX_OWNED_BYTES (32u * 1024u * 1024u)

enum {
    NK_JOINT = 1,
    NK_DOBJ,
    NK_MOBJ,
    NK_POBJ,
    NK_TOBJ,
    NK_VTX,
    NK_IMAGE,
    NK_TLUT,
    NK_MATERIAL,
    NK_PEDESC,
    NK_LOD,
    NK_TEV,
    NK_MTX,
};

enum {
    MV_JOBJ_PTCL = 1u << 5,
    MV_JOBJ_INSTANCE = 1u << 12,
    MV_JOBJ_SPLINE = 1u << 14,
    MV_JOBJ_USE_QUATERNION = 1u << 17,
    MV_JOBJ_JOINT_MASK = 3u << 21,
    MV_JOBJ_USER_DEF_MTX = 1u << 23,
    MV_JOBJ_BILLBOARD_MASK = 0xe00u | 0x2000u,
    MV_POBJ_TYPE_MASK = 0x3000u,
};

typedef struct {
    uint32_t offset;
    uint8_t kind;
    uint8_t state;
    void *ptr;
} NativeEntry;

typedef struct {
    void **owned;
    size_t owned_count;
} NativeStorage;

typedef struct {
    const MvDat *dat;
    MvNativeHsd *out;
    NativeEntry *entries;
    size_t entry_count, entry_capacity;
    void **owned;
    size_t owned_count, owned_capacity, owned_bytes;
    int status;
} NativeBuild;

static float be_float(const uint8_t *p)
{
    uint32_t bits = mv_be32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void *own_calloc(NativeBuild *b, size_t count, size_t size)
{
    if (!count || !size || count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    if (bytes > MV_NATIVE_MAX_OWNED_BYTES - b->owned_bytes ||
        b->owned_count >= b->owned_capacity) return NULL;
    void *p = calloc(count, size);
    if (!p) return NULL;
    b->owned[b->owned_count++] = p;
    b->owned_bytes += bytes;
    return p;
}

static NativeEntry *find_entry(NativeBuild *b, uint8_t kind, uint32_t offset)
{
    for (size_t i = 0; i < b->entry_count; ++i)
        if (b->entries[i].kind == kind && b->entries[i].offset == offset) return &b->entries[i];
    return NULL;
}

static NativeEntry *begin_entry(NativeBuild *b, uint8_t kind, uint32_t offset, size_t size,
                                int *existing)
{
    NativeEntry *entry = find_entry(b, kind, offset);
    if (entry) {
        *existing = 1;
        if (entry->state != 2) b->status = -1;
        return entry;
    }
    *existing = 0;
    if (b->entry_count >= b->entry_capacity) {
        b->status = -1;
        return NULL;
    }
    void *ptr = own_calloc(b, 1, size);
    if (!ptr) {
        b->status = -1;
        return NULL;
    }
    entry = &b->entries[b->entry_count++];
    entry->offset = offset;
    entry->kind = kind;
    entry->state = 1;
    entry->ptr = ptr;
    return entry;
}

static int pointer_offset(const MvDat *dat, uint32_t field, uint32_t *offset)
{
    int result = mv_dat_pointer(dat, field, offset);
    if (result < 0) return -1;
    if (!result) *offset = UINT32_MAX;
    return result;
}

static int raw_pointer(const MvDat *dat, uint32_t field, size_t min_size, void **ptr)
{
    uint32_t offset;
    int result = pointer_offset(dat, field, &offset);
    if (result <= 0) {
        *ptr = NULL;
        return result;
    }
    const uint8_t *p = mv_dat_span(dat, offset, min_size);
    if (!p) return -1;
    *ptr = (void *)(uintptr_t)p;
    return 1;
}

static int class_name(const MvDat *dat, uint32_t field, char **name)
{
    uint32_t offset;
    int result = pointer_offset(dat, field, &offset);
    if (result <= 0) {
        *name = NULL;
        return result;
    }
    const uint8_t *p = mv_dat_span(dat, offset, 1);
    if (!p || !memchr(p, 0, dat->data_size - offset)) return -1;
    *name = (char *)(uintptr_t)p;
    return 1;
}

static void mark_unsupported(NativeBuild *b)
{
    ++b->out->unsupported_count;
    if (!b->status) b->status = 1;
}

static HSD_DObjDesc *build_dobj(NativeBuild *b, uint32_t offset);
static HSD_MObjDesc *build_mobj(NativeBuild *b, uint32_t offset);
static HSD_PObjDesc *build_pobj(NativeBuild *b, uint32_t offset);
static HSD_TObjDesc *build_tobj(NativeBuild *b, uint32_t offset);

static HSD_ImageDesc *build_image(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_IMAGE, offset, sizeof(HSD_ImageDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 24);
    if (!p) { b->status = -1; return NULL; }
    HSD_ImageDesc *out = entry->ptr;
    out->width = mv_be16(p + 4);
    out->height = mv_be16(p + 6);
    out->format = (int32_t)mv_be32(p + 8);
    out->mipmap = mv_be32(p + 12);
    out->minLOD = be_float(p + 16);
    out->maxLOD = be_float(p + 20);
    size_t bytes = mv_gx_texture_size(out->width, out->height, out->format);
    if (!bytes || raw_pointer(b->dat, offset, bytes, &out->image_ptr) != 1) {
        b->status = -1;
        return NULL;
    }
    entry->state = 2;
    ++b->out->image_count;
    return out;
}

static HSD_TlutDesc *build_tlut(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_TLUT, offset, sizeof(HSD_TlutDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 16);
    if (!p) { b->status = -1; return NULL; }
    HSD_TlutDesc *out = entry->ptr;
    out->fmt = (int32_t)mv_be32(p + 4);
    out->tlut_name = mv_be32(p + 8);
    out->n_entries = mv_be16(p + 12);
    if (!out->n_entries || raw_pointer(b->dat, offset, (size_t)out->n_entries * 2, &out->lut) != 1) {
        b->status = -1;
        return NULL;
    }
    entry->state = 2;
    ++b->out->tlut_count;
    return out;
}

static HSD_TexLODDesc *build_lod(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_LOD, offset, sizeof(HSD_TexLODDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 16);
    if (!p) { b->status = -1; return NULL; }
    HSD_TexLODDesc *out = entry->ptr;
    out->minFilt = (int32_t)mv_be32(p);
    out->LODBias = be_float(p + 4);
    out->bias_clamp = p[8];
    out->edgeLODEnable = p[9];
    out->max_anisotropy = (int32_t)mv_be32(p + 12);
    entry->state = 2;
    ++b->out->lod_count;
    return out;
}

static HSD_TObjTevDesc *build_tev(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_TEV, offset, sizeof(HSD_TObjTevDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 32);
    if (!p) { b->status = -1; return NULL; }
    HSD_TObjTevDesc *out = entry->ptr;
    memcpy(&out->color_op, p, 16);
    memcpy(&out->konst, p + 16, sizeof(MvGXColor));
    memcpy(&out->tev0, p + 20, sizeof(MvGXColor));
    memcpy(&out->tev1, p + 24, sizeof(MvGXColor));
    out->active = mv_be32(p + 28);
    entry->state = 2;
    ++b->out->tev_count;
    return out;
}

static HSD_Material *build_material(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_MATERIAL, offset, sizeof(HSD_Material), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 20);
    if (!p) { b->status = -1; return NULL; }
    HSD_Material *out = entry->ptr;
    memcpy(&out->ambient, p, sizeof(MvGXColor));
    memcpy(&out->diffuse, p + 4, sizeof(MvGXColor));
    memcpy(&out->specular, p + 8, sizeof(MvGXColor));
    out->alpha = be_float(p + 12);
    out->shininess = be_float(p + 16);
    entry->state = 2;
    ++b->out->material_count;
    return out;
}

static HSD_PEDesc *build_pedesc(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_PEDESC, offset, sizeof(HSD_PEDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 12);
    if (!p) { b->status = -1; return NULL; }
    memcpy(entry->ptr, p, 12);
    entry->state = 2;
    ++b->out->pedesc_count;
    return entry->ptr;
}

static HSD_VtxDescList *build_vtx(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *found = find_entry(b, NK_VTX, offset);
    if (found) {
        if (found->state != 2) b->status = -1;
        return b->status ? NULL : found->ptr;
    }
    unsigned count = 0;
    uint32_t cursor = offset;
    for (; count < 32; ++count, cursor += 24) {
        const uint8_t *p = mv_dat_span(b->dat, cursor, 24);
        if (!p) { b->status = -1; return NULL; }
        if (mv_be32(p) == GX_VA_NULL) break;
    }
    if (!count || count == 32) { b->status = -1; return NULL; }
    NativeEntry *entry = begin_entry(b, NK_VTX, offset,
                                     (size_t)(count + 1) * sizeof(HSD_VtxDescList), &existing);
    if (!entry || existing || b->status) return NULL;
    HSD_VtxDescList *out = entry->ptr;
    cursor = offset;
    for (unsigned i = 0; i < count; ++i, cursor += 24) {
        const uint8_t *p = mv_dat_span(b->dat, cursor, 24);
        out[i].attr = (int32_t)mv_be32(p);
        out[i].attr_type = (int32_t)mv_be32(p + 4);
        out[i].comp_cnt = (int32_t)mv_be32(p + 8);
        out[i].comp_type = (int32_t)mv_be32(p + 12);
        out[i].frac = p[16];
        out[i].stride = mv_be16(p + 18);
        int result = raw_pointer(b->dat, cursor + 20, 1, &out[i].vertex);
        if (result < 0 || (out[i].attr_type != GX_DIRECT && result != 1)) {
            b->status = -1;
            return NULL;
        }
    }
    out[count].attr = GX_VA_NULL;
    entry->state = 2;
    b->out->vtxdesc_count += count;
    return out;
}

static HSD_TObjDesc *build_tobj(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_TOBJ, offset, sizeof(HSD_TObjDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 92);
    if (!p || class_name(b->dat, offset, &((HSD_TObjDesc *)entry->ptr)->class_name) < 0) {
        b->status = -1;
        return NULL;
    }
    HSD_TObjDesc *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_tobj(b, target))) return NULL;
    out->id = (int32_t)mv_be32(p + 8);
    out->src = (int32_t)mv_be32(p + 12);
    out->rotate.x = be_float(p + 0x10); out->rotate.y = be_float(p + 0x14); out->rotate.z = be_float(p + 0x18);
    out->scale.x = be_float(p + 0x1c); out->scale.y = be_float(p + 0x20); out->scale.z = be_float(p + 0x24);
    out->translate.x = be_float(p + 0x28); out->translate.y = be_float(p + 0x2c); out->translate.z = be_float(p + 0x30);
    out->wrap_s = (int32_t)mv_be32(p + 0x34);
    out->wrap_t = (int32_t)mv_be32(p + 0x38);
    out->repeat_s = p[0x3c]; out->repeat_t = p[0x3d];
    out->blend_flags = mv_be32(p + 0x40);
    out->blending = be_float(p + 0x44);
    out->magFilt = (int32_t)mv_be32(p + 0x48);
    result = pointer_offset(b->dat, offset + 0x4c, &target);
    if (result != 1 || !(out->imagedesc = build_image(b, target))) {
        if (!b->status) b->status = -1;
        return NULL;
    }
    result = pointer_offset(b->dat, offset + 0x50, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->tlutdesc = build_tlut(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 0x54, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->lod = build_lod(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 0x58, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->tev = build_tev(b, target))) return NULL;
    entry->state = 2;
    ++b->out->tobj_count;
    return out;
}

static HSD_MObjDesc *build_mobj(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_MOBJ, offset, sizeof(HSD_MObjDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 24);
    HSD_MObjDesc *out = entry->ptr;
    if (!p || class_name(b->dat, offset, &out->class_name) < 0) { b->status = -1; return NULL; }
    out->rendermode = mv_be32(p + 4);
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->texdesc = build_tobj(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 12, &target);
    if (result != 1 || !(out->mat = build_material(b, target))) {
        if (!b->status) b->status = -1;
        return NULL;
    }
    result = raw_pointer(b->dat, offset + 16, 1, &out->renderdesc);
    if (result < 0) { b->status = -1; return NULL; }
    if (result > 0) {
        /* Default upstream MObjLoad ignores this legacy field, but passing a
           serialized pointer as if it were native would violate the typed
           conversion contract. Add its adapter before accepting such roots. */
        mark_unsupported(b);
        return NULL;
    }
    result = pointer_offset(b->dat, offset + 20, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->pedesc = build_pedesc(b, target))) return NULL;
    entry->state = 2;
    ++b->out->mobj_count;
    return out;
}

static HSD_PObjDesc *build_pobj(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_POBJ, offset, sizeof(HSD_PObjDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 24);
    HSD_PObjDesc *out = entry->ptr;
    if (!p || class_name(b->dat, offset, &out->class_name) < 0) { b->status = -1; return NULL; }
    out->flags = mv_be16(p + 12);
    out->n_display = mv_be16(p + 14);
    if (out->flags & MV_POBJ_TYPE_MASK) { mark_unsupported(b); return NULL; }
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 20, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result) { mark_unsupported(b); return NULL; }
    result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_pobj(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 8, &target);
    if (result != 1 || !(out->verts = build_vtx(b, target))) {
        if (!b->status) b->status = -1;
        return NULL;
    }
    size_t display_size = (size_t)out->n_display << 5;
    if (!display_size || raw_pointer(b->dat, offset + 16, display_size, (void **)&out->display) != 1) {
        b->status = -1;
        return NULL;
    }
    out->u.joint = NULL;
    entry->state = 2;
    ++b->out->pobj_count;
    return out;
}

static HSD_DObjDesc *build_dobj(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_DOBJ, offset, sizeof(HSD_DObjDesc), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    HSD_DObjDesc *out = entry->ptr;
    if (!mv_dat_span(b->dat, offset, 16) || class_name(b->dat, offset, &out->class_name) < 0) {
        b->status = -1;
        return NULL;
    }
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_dobj(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->mobjdesc = build_mobj(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 12, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->pobjdesc = build_pobj(b, target))) return NULL;
    entry->state = 2;
    ++b->out->dobj_count;
    return out;
}

static HSD_Joint *build_joint(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_JOINT, offset, sizeof(HSD_Joint), &existing);
    if (!entry || b->status < 0) return NULL;
    if (existing) return b->status ? NULL : entry->ptr;
    const uint8_t *p = mv_dat_span(b->dat, offset, 64);
    HSD_Joint *out = entry->ptr;
    if (!p || class_name(b->dat, offset, &out->class_name) < 0) { b->status = -1; return NULL; }
    out->flags = mv_be32(p + 4);
    if (out->flags & (MV_JOBJ_PTCL | MV_JOBJ_INSTANCE | MV_JOBJ_SPLINE |
                      MV_JOBJ_USE_QUATERNION | MV_JOBJ_JOINT_MASK |
                      MV_JOBJ_USER_DEF_MTX | MV_JOBJ_BILLBOARD_MASK)) {
        mark_unsupported(b);
        return NULL;
    }
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->child = build_joint(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 12, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_joint(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 16, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->u.dobjdesc = build_dobj(b, target))) return NULL;
    out->rotation.x = be_float(p + 0x14); out->rotation.y = be_float(p + 0x18); out->rotation.z = be_float(p + 0x1c);
    out->scale.x = be_float(p + 0x20); out->scale.y = be_float(p + 0x24); out->scale.z = be_float(p + 0x28);
    out->position.x = be_float(p + 0x2c); out->position.y = be_float(p + 0x30); out->position.z = be_float(p + 0x34);
    result = pointer_offset(b->dat, offset + 0x38, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result) {
        int mtx_existing;
        NativeEntry *mtx_entry = begin_entry(b, NK_MTX, target, sizeof(MvMtx), &mtx_existing);
        if (!mtx_entry || b->status < 0) return NULL;
        if (mtx_existing) out->mtx = mtx_entry->ptr;
        else {
            const uint8_t *raw = mv_dat_span(b->dat, target, sizeof(MvMtx));
            if (!raw) { b->status = -1; return NULL; }
            float *dst = mtx_entry->ptr;
            for (unsigned i = 0; i < 12; ++i) dst[i] = be_float(raw + i * 4);
            mtx_entry->state = 2;
            out->mtx = mtx_entry->ptr;
        }
    }
    result = pointer_offset(b->dat, offset + 0x3c, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result) { mark_unsupported(b); return NULL; }
    out->robjdesc = NULL;
    entry->state = 2;
    ++b->out->joint_count;
    return out;
}

static void release_build(NativeBuild *b, int keep_owned)
{
    free(b->entries);
    if (!keep_owned) {
        for (size_t i = 0; i < b->owned_count; ++i) free(b->owned[i]);
        free(b->owned);
    }
}

int mv_hsd_native_build(const MvDat *dat, const char *root_name, MvNativeHsd *out)
{
    if (!dat || !root_name || !out || !dat->pointer_bits) return -1;
    memset(out, 0, sizeof(*out));
    uint32_t root_offset = UINT32_MAX;
    for (uint32_t i = 0; i < dat->public_count; ++i) {
        const char *name;
        uint32_t offset;
        if (mv_dat_public(dat, i, &name, &offset)) return -1;
        if (!strcmp(name, root_name)) { root_offset = offset; break; }
    }
    if (root_offset == UINT32_MAX) return -1;
    NativeBuild b = {.dat = dat, .out = out};
    /* Entries must not move while recursive builders retain an entry pointer. */
    b.entries = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.entries));
    b.owned = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.owned));
    if (!b.entries || !b.owned) {
        free(b.entries);
        free(b.owned);
        return -1;
    }
    b.entry_capacity = MV_NATIVE_MAX_NODES;
    b.owned_capacity = MV_NATIVE_MAX_NODES;
    out->root = build_joint(&b, root_offset);
    int result = b.status;
    if (!result && !out->root) result = -1;
    if (result) {
        release_build(&b, 0);
        memset(out, 0, sizeof(*out));
        return result;
    }
    NativeStorage *storage = malloc(sizeof(*storage));
    if (!storage) {
        release_build(&b, 0);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    storage->owned = b.owned;
    storage->owned_count = b.owned_count;
    out->storage = storage;
    release_build(&b, 1);
    return 0;
}

void mv_hsd_native_free(MvNativeHsd *graph)
{
    if (!graph) return;
    NativeStorage *storage = graph->storage;
    if (storage) {
        for (size_t i = 0; i < storage->owned_count; ++i) free(storage->owned[i]);
        free(storage->owned);
        free(storage);
    }
    memset(graph, 0, sizeof(*graph));
}

int mv_hsd_native_stats(const MvDat *dat, const char *root_name,
                        uint32_t stats[MV_NATIVE_STAT_COUNT])
{
    if (!stats) return -1;
    memset(stats, 0, MV_NATIVE_STAT_COUNT * sizeof(*stats));
    MvNativeHsd graph;
    int result = mv_hsd_native_build(dat, root_name, &graph);
    if (result) return result;
    stats[MV_NATIVE_STAT_JOINTS] = (uint32_t)graph.joint_count;
    stats[MV_NATIVE_STAT_DOBJS] = (uint32_t)graph.dobj_count;
    stats[MV_NATIVE_STAT_MOBJS] = (uint32_t)graph.mobj_count;
    stats[MV_NATIVE_STAT_POBJS] = (uint32_t)graph.pobj_count;
    stats[MV_NATIVE_STAT_TOBJS] = (uint32_t)graph.tobj_count;
    stats[MV_NATIVE_STAT_VTXDESCS] = (uint32_t)graph.vtxdesc_count;
    stats[MV_NATIVE_STAT_IMAGES] = (uint32_t)graph.image_count;
    stats[MV_NATIVE_STAT_TLUTS] = (uint32_t)graph.tlut_count;
    stats[MV_NATIVE_STAT_MATERIALS] = (uint32_t)graph.material_count;
    stats[MV_NATIVE_STAT_PEDESCS] = (uint32_t)graph.pedesc_count;
    stats[MV_NATIVE_STAT_LODS] = (uint32_t)graph.lod_count;
    stats[MV_NATIVE_STAT_TEVS] = (uint32_t)graph.tev_count;
    stats[MV_NATIVE_STAT_UNSUPPORTED] = (uint32_t)graph.unsupported_count;
    mv_hsd_native_free(&graph);
    return 0;
}
