#include "hsd_shapeanim_native.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/fobj.h>

#define MV_SHAPEANIM_MAX_NODES 4096u
#define MV_SHAPEANIM_MAX_OWNED_BYTES (2u * 1024u * 1024u)

enum {
    NK_JOINT = 1,
    NK_DOBJ,
    NK_SHAPE,
    NK_AOBJ,
    NK_FOBJ,
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
    MvNativeShapeAnim *out;
    NativeEntry *entries;
    size_t entry_count;
    void **owned;
    size_t owned_count;
    size_t owned_bytes;
    int status;
} NativeBuild;

static float be_float(const uint8_t *raw)
{
    uint32_t bits = mv_be32(raw);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void *own_calloc(NativeBuild *b, size_t count, size_t size)
{
    if (!count || !size || count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    if (b->owned_count >= MV_SHAPEANIM_MAX_NODES ||
        bytes > MV_SHAPEANIM_MAX_OWNED_BYTES - b->owned_bytes)
        return NULL;
    void *ptr = calloc(count, size);
    if (!ptr) return NULL;
    b->owned[b->owned_count++] = ptr;
    b->owned_bytes += bytes;
    return ptr;
}

static NativeEntry *find_entry(NativeBuild *b, uint8_t kind, uint32_t offset)
{
    for (size_t i = 0; i < b->entry_count; ++i)
        if (b->entries[i].kind == kind && b->entries[i].offset == offset)
            return &b->entries[i];
    return NULL;
}

static NativeEntry *begin_entry(NativeBuild *b, uint8_t kind, uint32_t offset,
                                size_t size, int *existing)
{
    NativeEntry *entry = find_entry(b, kind, offset);
    if (entry) {
        *existing = 1;
        if (entry->state != 2) b->status = -1;
        return entry;
    }
    *existing = 0;
    if (b->entry_count >= MV_SHAPEANIM_MAX_NODES) {
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

static int pointer_offset(const MvDat *dat, uint32_t field, uint32_t *target)
{
    int result = mv_dat_pointer(dat, field, target);
    if (result < 0) return -1;
    if (!result) *target = UINT32_MAX;
    return result;
}

static HSD_FObjDesc *build_fobj(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_FOBJ, offset,
                                     sizeof(HSD_FObjDesc), &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;
    const uint8_t *raw = mv_dat_span(b->dat, offset, 20);
    if (!raw) { b->status = -1; return NULL; }
    HSD_FObjDesc *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_fobj(b, target))) return NULL;
    out->length = mv_be32(raw + 4);
    out->startframe = be_float(raw + 8);
    out->type = raw[12];
    out->frac_value = raw[13];
    out->frac_slope = raw[14];
    out->dummy0 = raw[15];
    if (!out->length || !isfinite(out->startframe) || out->type < 1u ||
        out->type > 32u ||
        pointer_offset(b->dat, offset + 16, &target) != 1) {
        b->status = -2;
        return NULL;
    }
    out->ad = (uint8_t *)(uintptr_t)mv_dat_span(b->dat, target, out->length);
    if (!out->ad) { b->status = -1; return NULL; }
    entry->state = 2;
    ++b->out->fobj_count;
    return out;
}

static HSD_AObjDesc *build_aobj(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_AOBJ, offset,
                                     sizeof(HSD_AObjDesc), &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;
    const uint8_t *raw = mv_dat_span(b->dat, offset, 16);
    if (!raw) { b->status = -1; return NULL; }
    HSD_AObjDesc *out = entry->ptr;
    out->flags = mv_be32(raw);
    out->end_frame = be_float(raw + 4);
    if (!isfinite(out->end_frame) || out->end_frame < 0.0f ||
        out->end_frame > 10000000.0f || mv_be32(raw + 12) != 0) {
        b->status = -2;
        return NULL;
    }
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->fobjdesc = build_fobj(b, target))) return NULL;
    out->obj_id = 0;
    entry->state = 2;
    ++b->out->aobj_count;
    return out;
}

static HSD_ShapeAnim *build_shape(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_SHAPE, offset,
                                     sizeof(HSD_ShapeAnim), &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;
    if (!mv_dat_span(b->dat, offset, 8)) { b->status = -1; return NULL; }
    HSD_ShapeAnim *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_shape(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->aobjdesc = build_aobj(b, target))) return NULL;
    entry->state = 2;
    ++b->out->shapeanim_count;
    return out;
}

static HSD_ShapeAnimDObj *build_dobj(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_DOBJ, offset,
                                     sizeof(HSD_ShapeAnimDObj), &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;
    if (!mv_dat_span(b->dat, offset, 8)) { b->status = -1; return NULL; }
    HSD_ShapeAnimDObj *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_dobj(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->shapeanim = build_shape(b, target))) return NULL;
    entry->state = 2;
    ++b->out->dobj_count;
    return out;
}

static HSD_ShapeAnimJoint *build_joint(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_JOINT, offset,
                                     sizeof(HSD_ShapeAnimJoint), &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;
    if (!mv_dat_span(b->dat, offset, 12)) { b->status = -1; return NULL; }
    HSD_ShapeAnimJoint *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->child = build_joint(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_joint(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->shapeanimdobj = build_dobj(b, target))) return NULL;
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

int mv_native_shapeanim_build_at(const MvDat *dat, uint32_t root_offset,
                                 MvNativeShapeAnim *out)
{
    if (!dat || !out || !dat->pointer_bits || root_offset >= dat->data_size)
        return -1;
    memset(out, 0, sizeof(*out));
    NativeBuild b = {.dat = dat, .out = out};
    b.entries = calloc(MV_SHAPEANIM_MAX_NODES, sizeof(*b.entries));
    b.owned = calloc(MV_SHAPEANIM_MAX_NODES, sizeof(*b.owned));
    if (!b.entries || !b.owned) {
        free(b.entries);
        free(b.owned);
        return -1;
    }
    out->root = build_joint(&b, root_offset);
    if (b.status || !out->root) {
        int result = b.status ? b.status : -1;
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

void mv_native_shapeanim_free(MvNativeShapeAnim *anim)
{
    if (!anim) return;
    NativeStorage *storage = anim->storage;
    if (storage) {
        for (size_t i = 0; i < storage->owned_count; ++i)
            free(storage->owned[i]);
        free(storage->owned);
        free(storage);
    }
    memset(anim, 0, sizeof(*anim));
}
