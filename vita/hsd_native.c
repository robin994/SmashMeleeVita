#include "hsd_native.h"
#include "gx_texture.h"

#include <math.h>
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
typedef struct HSD_EnvelopeDesc HSD_EnvelopeDesc;

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
    union {
        HSD_Joint *joint;
        void *shape_set;
        HSD_EnvelopeDesc **envelope_p;
    } u;
};

struct HSD_EnvelopeDesc {
    HSD_Joint *joint;
    float weight;
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
#define MV_NATIVE_ENTRY_HASH_CAPACITY 65536u
#define MV_NATIVE_MAX_OWNED_BYTES (32u * 1024u * 1024u)
#define MV_NATIVE_MAX_ENVELOPE_MATRICES 32u
#define MV_NATIVE_MAX_ENVELOPE_WEIGHTS 32u

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
    MV_JOBJ_BILLBOARD_FIELD = 0xe00u,
    MV_JOBJ_BILLBOARD = 0x200u,
    MV_JOBJ_PBILLBOARD = 0x2000u,
    MV_POBJ_TYPE_MASK = 0x3000u,
    MV_POBJ_SHAPEANIM = 0x1000u,
    MV_POBJ_ENVELOPE = 0x2000u,
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
    HSD_EnvelopeDesc *desc;
    uint32_t joint_offset;
} NativeEnvelopePatch;

typedef struct {
    HSD_PObjDesc *desc;
    uint32_t joint_offset;
} NativeSkinPatch;

typedef struct {
    const MvDat *dat;
    MvNativeHsd *out;
    NativeEntry *entries;
    size_t entry_count, entry_capacity;
    uint32_t *entry_hash;
    size_t entry_hash_capacity;
    void **owned;
    size_t owned_count, owned_capacity, owned_bytes;
    NativeEnvelopePatch *envelope_patches;
    size_t envelope_patch_count, envelope_patch_capacity;
    NativeSkinPatch *skin_patches;
    size_t skin_patch_count, skin_patch_capacity;
    int raw_validation;
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

static size_t entry_hash_slot(const NativeBuild *b, uint8_t kind,
                              uint32_t offset)
{
    uint32_t hash = (offset >> 2) * 2654435761u;
    hash ^= (uint32_t) kind * 2246822519u;
    return hash & (b->entry_hash_capacity - 1u);
}

static NativeEntry *find_entry(NativeBuild *b, uint8_t kind, uint32_t offset)
{
    if (b->entry_hash == NULL || b->entry_hash_capacity == 0) {
        return NULL;
    }
    size_t slot = entry_hash_slot(b, kind, offset);
    for (size_t probe = 0; probe < b->entry_hash_capacity; ++probe) {
        uint32_t encoded = b->entry_hash[slot];
        if (encoded == 0) {
            return NULL;
        }
        NativeEntry *entry = &b->entries[encoded - 1u];
        if (entry->kind == kind && entry->offset == offset) {
            return entry;
        }
        slot = (slot + 1u) & (b->entry_hash_capacity - 1u);
    }
    return NULL;
}

static int index_entry(NativeBuild *b, size_t entry_index)
{
    NativeEntry *entry = &b->entries[entry_index];
    size_t slot = entry_hash_slot(b, entry->kind, entry->offset);
    for (size_t probe = 0; probe < b->entry_hash_capacity; ++probe) {
        if (b->entry_hash[slot] == 0) {
            b->entry_hash[slot] = (uint32_t) entry_index + 1u;
            return 0;
        }
        slot = (slot + 1u) & (b->entry_hash_capacity - 1u);
    }
    return -1;
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
    size_t entry_index = b->entry_count++;
    entry = &b->entries[entry_index];
    entry->offset = offset;
    entry->kind = kind;
    entry->state = 1;
    entry->ptr = ptr;
    if (index_entry(b, entry_index) != 0) {
        b->status = -1;
        return NULL;
    }
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

static void mark_unsupported(NativeBuild *b, uint32_t kind,
                             uint32_t offset, uint32_t value)
{
    ++b->out->unsupported_count;
    if (b->out->unsupported_kind == MV_NATIVE_UNSUPPORTED_NONE) {
        b->out->unsupported_kind = kind;
        b->out->unsupported_offset = offset;
        b->out->unsupported_value = value;
    }
    if (!b->status) b->status = 1;
}

const char *mv_hsd_native_unsupported_name(uint32_t kind)
{
    switch (kind) {
    case MV_NATIVE_UNSUPPORTED_MOBJ_RENDERDESC: return "MObj.renderdesc";
    case MV_NATIVE_UNSUPPORTED_POBJ_TYPE: return "PObj.type";
    case MV_NATIVE_UNSUPPORTED_POBJ_UNION: return "PObj.u";
    case MV_NATIVE_UNSUPPORTED_JOBJ_FLAGS: return "JObj.flags";
    case MV_NATIVE_UNSUPPORTED_JOBJ_ROBJ: return "JObj.robjdesc";
    default: return "none";
    }
}

static HSD_DObjDesc *build_dobj(NativeBuild *b, uint32_t offset);
static HSD_MObjDesc *build_mobj(NativeBuild *b, uint32_t offset);
static HSD_PObjDesc *build_pobj(NativeBuild *b, uint32_t offset);
static HSD_TObjDesc *build_tobj(NativeBuild *b, uint32_t offset);
static HSD_Joint *build_joint(NativeBuild *b, uint32_t offset);

static int validate_raw_spline(const MvDat *dat, uint32_t offset)
{
    const uint8_t *p = mv_dat_span(dat, offset, 24);
    if (!p) return -1;
    uint8_t type = p[0];
    int16_t numcv = (int16_t)mv_be16(p + 2);
    float tension = be_float(p + 4);
    float total_length = be_float(p + 0x0c);
    if (type > 3 || numcv < 2 || numcv > 4096 || !isfinite(tension) ||
        !isfinite(total_length) || total_length < 0.0f)
        return -1;

    size_t cv_count = type == 0 ? (size_t)numcv
                      : type == 1 ? (size_t)numcv * 3u - 2u
                                  : (size_t)numcv + 2u;
    uint32_t target;
    if (mv_dat_pointer(dat, offset + 8, &target) != 1) return -1;
    const uint8_t *cv = mv_dat_span(dat, target, cv_count * 3u * sizeof(float));
    if (!cv) return -1;
    for (size_t i = 0; i < cv_count * 3u; ++i)
        if (!isfinite(be_float(cv + i * 4))) return -1;

    if (mv_dat_pointer(dat, offset + 0x10, &target) != 1) return -1;
    const uint8_t *lengths = mv_dat_span(dat, target, (size_t)numcv * sizeof(float));
    if (!lengths) return -1;
    for (size_t i = 0; i < (size_t)numcv; ++i)
        if (!isfinite(be_float(lengths + i * 4))) return -1;

    size_t poly_count = ((size_t)numcv - 1u) * 5u;
    if (mv_dat_pointer(dat, offset + 0x14, &target) != 1) return -1;
    const uint8_t *poly = mv_dat_span(dat, target, poly_count * sizeof(float));
    if (!poly) return -1;
    for (size_t i = 0; i < poly_count; ++i)
        if (!isfinite(be_float(poly + i * 4))) return -1;
    return 0;
}

static int add_envelope_patch(NativeBuild *b, HSD_EnvelopeDesc *desc,
                              uint32_t joint_offset)
{
    if (b->envelope_patch_count >= b->envelope_patch_capacity) {
        b->status = -1;
        return -1;
    }
    NativeEnvelopePatch *patch = &b->envelope_patches[b->envelope_patch_count++];
    patch->desc = desc;
    patch->joint_offset = joint_offset;
    return 0;
}

static int add_skin_patch(NativeBuild *b, HSD_PObjDesc *desc,
                          uint32_t joint_offset)
{
    if (b->skin_patch_count >= b->skin_patch_capacity) {
        b->status = -1;
        return -1;
    }
    NativeSkinPatch *patch = &b->skin_patches[b->skin_patch_count++];
    patch->desc = desc;
    patch->joint_offset = joint_offset;
    return 0;
}

static HSD_EnvelopeDesc **build_envelope_descs(NativeBuild *b,
                                                uint32_t array_offset)
{
    unsigned matrix_count = 0;
    for (; matrix_count < MV_NATIVE_MAX_ENVELOPE_MATRICES; ++matrix_count) {
        uint32_t target;
        int result = pointer_offset(b->dat, array_offset + matrix_count * 4u,
                                    &target);
        if (result < 0) { b->status = -1; return NULL; }
        if (!result) break;
    }
    if (matrix_count == MV_NATIVE_MAX_ENVELOPE_MATRICES) {
        b->status = -1;
        return NULL;
    }

    HSD_EnvelopeDesc **lists = own_calloc(b, matrix_count + 1u,
                                          sizeof(*lists));
    if (!lists) { b->status = -1; return NULL; }

    for (unsigned matrix = 0; matrix < matrix_count; ++matrix) {
        uint32_t envelope_offset;
        if (pointer_offset(b->dat, array_offset + matrix * 4u,
                           &envelope_offset) != 1) {
            b->status = -1;
            return NULL;
        }

        unsigned weight_count = 0;
        for (; weight_count < MV_NATIVE_MAX_ENVELOPE_WEIGHTS; ++weight_count) {
            uint32_t joint_offset;
            int result = pointer_offset(b->dat,
                                        envelope_offset + weight_count * 8u,
                                        &joint_offset);
            if (result < 0) { b->status = -1; return NULL; }
            if (!result) break;
        }
        if (!weight_count || weight_count == MV_NATIVE_MAX_ENVELOPE_WEIGHTS) {
            b->status = -1;
            return NULL;
        }

        HSD_EnvelopeDesc *descs = own_calloc(b, weight_count + 1u,
                                             sizeof(*descs));
        if (!descs) { b->status = -1; return NULL; }
        lists[matrix] = descs;
        for (unsigned weight = 0; weight < weight_count; ++weight) {
            uint32_t joint_offset;
            const uint8_t *raw = mv_dat_span(
                b->dat, envelope_offset + weight * 8u, 8u);
            if (!raw || pointer_offset(b->dat,
                                       envelope_offset + weight * 8u,
                                       &joint_offset) != 1) {
                b->status = -1;
                return NULL;
            }
            descs[weight].weight = be_float(raw + 4);
            if (!isfinite(descs[weight].weight) || descs[weight].weight < 0.0f ||
                add_envelope_patch(b, &descs[weight], joint_offset)) {
                b->status = -1;
                return NULL;
            }
        }
    }
    return lists;
}

static int resolve_envelope_patches(NativeBuild *b)
{
    for (size_t i = 0; i < b->envelope_patch_count; ++i) {
        NativeEnvelopePatch *patch = &b->envelope_patches[i];
        NativeEntry *joint = find_entry(b, NK_JOINT, patch->joint_offset);
        if (!joint || joint->state != 2 || !joint->ptr) return -1;
        patch->desc->joint = joint->ptr;
    }
    return 0;
}

static int resolve_skin_patches(NativeBuild *b)
{
    for (size_t i = 0; i < b->skin_patch_count; ++i) {
        NativeSkinPatch *patch = &b->skin_patches[i];
        NativeEntry *joint = find_entry(b, NK_JOINT, patch->joint_offset);
        /* A shared-skin reference must resolve to a JObj that belongs to the
           structural child/next tree. Merely converting a detached descriptor
           would not make HSD_IDGetData() able to resolve it at runtime. */
        if (!joint || joint->state != 2 || !joint->ptr) return -1;
        patch->desc->u.joint = joint->ptr;
    }
    return 0;
}

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
        mark_unsupported(b, MV_NATIVE_UNSUPPORTED_MOBJ_RENDERDESC,
                         offset, target);
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
    const uint16_t pobj_type = out->flags & MV_POBJ_TYPE_MASK;
    if (pobj_type == MV_POBJ_SHAPEANIM || pobj_type == MV_POBJ_TYPE_MASK) {
        mark_unsupported(b, MV_NATIVE_UNSUPPORTED_POBJ_TYPE,
                         offset, out->flags);
        return NULL;
    }
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 20, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (pobj_type == MV_POBJ_ENVELOPE) {
        if (result != 1 ||
            !(out->u.envelope_p = build_envelope_descs(b, target))) {
            if (!b->status) b->status = -1;
            return NULL;
        }
    } else if (pobj_type == 0) {
        if (result && add_skin_patch(b, out, target)) return NULL;
    } else if (result) {
        mark_unsupported(b, MV_NATIVE_UNSUPPORTED_POBJ_UNION,
                         offset, target);
        return NULL;
    }
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
    uint32_t unsupported_flags = out->flags &
        (MV_JOBJ_PTCL | MV_JOBJ_INSTANCE | MV_JOBJ_USE_QUATERNION |
         MV_JOBJ_JOINT_MASK | MV_JOBJ_USER_DEF_MTX);
    if (!b->raw_validation) {
        unsupported_flags |= out->flags & (MV_JOBJ_SPLINE | MV_JOBJ_PBILLBOARD);
    }
    const uint32_t billboard = out->flags & MV_JOBJ_BILLBOARD_FIELD;
    if (billboard) {
        if (!b->raw_validation) {
            if (billboard != MV_JOBJ_BILLBOARD) unsupported_flags |= billboard;
        } else if (billboard != 0x200u && billboard != 0x400u &&
                   billboard != 0x600u && billboard != 0x800u) {
            unsupported_flags |= billboard;
        }
    }
    if (unsupported_flags) {
        mark_unsupported(b, MV_NATIVE_UNSUPPORTED_JOBJ_FLAGS,
                         offset, out->flags);
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
    if (out->flags & MV_JOBJ_SPLINE) {
        if (!b->raw_validation || result != 1 || validate_raw_spline(b->dat, target)) {
            if (!b->status) b->status = -1;
            return NULL;
        }
        out->u.spline = (void *)(uintptr_t) mv_dat_span(b->dat, target, 24);
    } else if (result && !(out->u.dobjdesc = build_dobj(b, target))) {
        return NULL;
    }
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
    if (result && !b->raw_validation) {
        mark_unsupported(b, MV_NATIVE_UNSUPPORTED_JOBJ_ROBJ,
                         offset, target);
        return NULL;
    }
    /* Raw validation leaves RObjDesc to the typed pre-relocation walker. */
    out->robjdesc = NULL;
    entry->state = 2;
    ++b->out->joint_count;
    return out;
}

static void release_build(NativeBuild *b, int keep_owned)
{
    free(b->skin_patches);
    free(b->envelope_patches);
    free(b->entry_hash);
    free(b->entries);
    if (!keep_owned) {
        for (size_t i = 0; i < b->owned_count; ++i) free(b->owned[i]);
        free(b->owned);
    }
}

static int mv_hsd_native_build_at_mode(const MvDat *dat, uint32_t root_offset,
                                       MvNativeHsd *out, int raw_validation)
{
    if (!dat || !out || !dat->pointer_bits || root_offset > dat->data_size) return -1;
    memset(out, 0, sizeof(*out));
    NativeBuild b = {.dat = dat, .out = out, .raw_validation = raw_validation};
    /* Entries must not move while recursive builders retain an entry pointer. */
    b.entries = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.entries));
    b.entry_hash = calloc(MV_NATIVE_ENTRY_HASH_CAPACITY,
                          sizeof(*b.entry_hash));
    b.owned = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.owned));
    b.envelope_patches = calloc(MV_NATIVE_MAX_NODES,
                                sizeof(*b.envelope_patches));
    b.skin_patches = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.skin_patches));
    if (!b.entries || !b.entry_hash || !b.owned ||
        !b.envelope_patches || !b.skin_patches) {
        free(b.skin_patches);
        free(b.envelope_patches);
        free(b.entry_hash);
        free(b.entries);
        free(b.owned);
        return -1;
    }
    b.entry_capacity = MV_NATIVE_MAX_NODES;
    b.entry_hash_capacity = MV_NATIVE_ENTRY_HASH_CAPACITY;
    b.owned_capacity = MV_NATIVE_MAX_NODES;
    b.envelope_patch_capacity = MV_NATIVE_MAX_NODES;
    b.skin_patch_capacity = MV_NATIVE_MAX_NODES;
    out->root = build_joint(&b, root_offset);
    if (!b.status && resolve_envelope_patches(&b)) b.status = -1;
    if (!b.status && resolve_skin_patches(&b)) b.status = -1;
    int result = b.status;
    if (!result && !out->root) result = -1;
    if (result) {
        uint32_t unsupported_kind = out->unsupported_kind;
        uint32_t unsupported_offset = out->unsupported_offset;
        uint32_t unsupported_value = out->unsupported_value;
        size_t unsupported_count = out->unsupported_count;
        release_build(&b, 0);
        memset(out, 0, sizeof(*out));
        out->unsupported_kind = unsupported_kind;
        out->unsupported_offset = unsupported_offset;
        out->unsupported_value = unsupported_value;
        out->unsupported_count = unsupported_count;
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

int mv_hsd_native_build_at(const MvDat *dat, uint32_t root_offset, MvNativeHsd *out)
{
    return mv_hsd_native_build_at_mode(dat, root_offset, out, 0);
}

int mv_hsd_native_validate_raw_at(const MvDat *dat, uint32_t root_offset,
                                  MvNativeHsd *out)
{
    return mv_hsd_native_build_at_mode(dat, root_offset, out, 1);
}

int mv_hsd_native_validate_raw_set(const MvDat *dat,
                                   const uint32_t *root_offsets,
                                   size_t root_count, MvNativeHsd *out)
{
    if (!dat || !out || !dat->pointer_bits ||
        (root_count != 0 && root_offsets == NULL))
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (root_count == 0) {
        return 0;
    }

    NativeBuild b = {.dat = dat, .out = out, .raw_validation = 1};
    b.entries = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.entries));
    b.entry_hash = calloc(MV_NATIVE_ENTRY_HASH_CAPACITY,
                          sizeof(*b.entry_hash));
    b.owned = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.owned));
    b.envelope_patches = calloc(MV_NATIVE_MAX_NODES,
                                sizeof(*b.envelope_patches));
    b.skin_patches = calloc(MV_NATIVE_MAX_NODES, sizeof(*b.skin_patches));
    if (!b.entries || !b.entry_hash || !b.owned ||
        !b.envelope_patches || !b.skin_patches) {
        free(b.skin_patches);
        free(b.envelope_patches);
        free(b.entry_hash);
        free(b.entries);
        free(b.owned);
        return -1;
    }
    b.entry_capacity = MV_NATIVE_MAX_NODES;
    b.entry_hash_capacity = MV_NATIVE_ENTRY_HASH_CAPACITY;
    b.owned_capacity = MV_NATIVE_MAX_NODES;
    b.envelope_patch_capacity = MV_NATIVE_MAX_NODES;
    b.skin_patch_capacity = MV_NATIVE_MAX_NODES;

    for (size_t i = 0; i < root_count && !b.status; ++i) {
        if (root_offsets[i] >= dat->data_size ||
            build_joint(&b, root_offsets[i]) == NULL)
        {
            if (!b.status) {
                b.status = -1;
            }
            break;
        }
    }
    if (!b.status && resolve_envelope_patches(&b)) b.status = -1;
    if (!b.status && resolve_skin_patches(&b)) b.status = -1;

    int result = b.status;
    if (result) {
        uint32_t unsupported_kind = out->unsupported_kind;
        uint32_t unsupported_offset = out->unsupported_offset;
        uint32_t unsupported_value = out->unsupported_value;
        size_t unsupported_count = out->unsupported_count;
        release_build(&b, 0);
        memset(out, 0, sizeof(*out));
        out->unsupported_kind = unsupported_kind;
        out->unsupported_offset = unsupported_offset;
        out->unsupported_value = unsupported_value;
        out->unsupported_count = unsupported_count;
        return result;
    }

    /* Validation never returns the temporary native graph.  Keep the aggregate
     * counts/diagnostics but release all scratch allocations immediately. */
    release_build(&b, 0);
    out->root = NULL;
    out->storage = NULL;
    return 0;
}

int mv_hsd_native_build(const MvDat *dat, const char *root_name, MvNativeHsd *out)
{
    if (!dat || !root_name || !out || !dat->pointer_bits) return -1;
    uint32_t root_offset = UINT32_MAX;
    for (uint32_t i = 0; i < dat->public_count; ++i) {
        const char *name; uint32_t offset;
        if (mv_dat_public(dat, i, &name, &offset)) return -1;
        if (!strcmp(name, root_name)) { root_offset = offset; break; }
    }
    if (root_offset == UINT32_MAX) return -1;
    return mv_hsd_native_build_at(dat, root_offset, out);
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
