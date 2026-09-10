#include "hsd_matanim_native.h"

#include "gx_texture.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/fobj.h>
#include <sysdolphin/baselib/tobj.h>

#define MV_MATANIM_MAX_NODES 8192u
#define MV_MATANIM_MAX_TABLE_ENTRIES 1024u
#define MV_MATANIM_MAX_OWNED_BYTES (8u * 1024u * 1024u)

enum {
    NK_MATJOINT = 1,
    NK_MATANIM,
    NK_TEXANIM,
    NK_AOBJ_MOBJ,
    NK_AOBJ_TOBJ,
    NK_FOBJ_MOBJ,
    NK_FOBJ_TOBJ,
    NK_IMAGE,
    NK_TLUT,
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
    MvNativeMatAnim *out;
    NativeEntry *entries;
    size_t entry_count;
    void **owned;
    size_t owned_count;
    size_t owned_bytes;
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
    if (b->owned_count >= MV_MATANIM_MAX_NODES ||
        bytes > MV_MATANIM_MAX_OWNED_BYTES - b->owned_bytes)
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
    if (b->entry_count >= MV_MATANIM_MAX_NODES) {
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

static HSD_ImageDesc *build_image(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_IMAGE, offset, sizeof(HSD_ImageDesc),
                                     &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;

    const uint8_t *raw = mv_dat_span(b->dat, offset, 24);
    if (!raw) { b->status = -1; return NULL; }
    HSD_ImageDesc *out = entry->ptr;
    out->width = mv_be16(raw + 4);
    out->height = mv_be16(raw + 6);
    out->format = (GXTexFmt)mv_be32(raw + 8);
    out->mipmap = mv_be32(raw + 12);
    out->minLOD = be_float(raw + 16);
    out->maxLOD = be_float(raw + 20);
    if (!out->width || !out->height || !isfinite(out->minLOD) ||
        !isfinite(out->maxLOD)) {
        b->status = -1;
        return NULL;
    }

    size_t image_bytes = mv_gx_texture_size(out->width, out->height, out->format);
    uint32_t image_offset;
    if (!image_bytes || pointer_offset(b->dat, offset, &image_offset) != 1) {
        b->status = -1;
        return NULL;
    }
    out->image_ptr = (void *)(uintptr_t)mv_dat_span(b->dat, image_offset,
                                                    image_bytes);
    if (!out->image_ptr) { b->status = -1; return NULL; }

    entry->state = 2;
    ++b->out->image_count;
    return out;
}

static HSD_TlutDesc *build_tlut(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_TLUT, offset, sizeof(HSD_TlutDesc),
                                     &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;

    const uint8_t *raw = mv_dat_span(b->dat, offset, 16);
    if (!raw) { b->status = -1; return NULL; }
    HSD_TlutDesc *out = entry->ptr;
    out->fmt = (GXTlutFmt)mv_be32(raw + 4);
    out->tlut_name = mv_be32(raw + 8);
    out->n_entries = mv_be16(raw + 12);
    uint32_t lut_offset;
    if (!out->n_entries ||
        pointer_offset(b->dat, offset, &lut_offset) != 1) {
        b->status = -1;
        return NULL;
    }
    out->lut = (void *)(uintptr_t)mv_dat_span(
        b->dat, lut_offset, (size_t)out->n_entries * 2u);
    if (!out->lut) { b->status = -1; return NULL; }

    entry->state = 2;
    ++b->out->tlut_count;
    return out;
}

static HSD_FObjDesc *build_fobj(NativeBuild *b, uint32_t offset, int tobj)
{
    const uint8_t kind = tobj ? NK_FOBJ_TOBJ : NK_FOBJ_MOBJ;
    int existing;
    NativeEntry *entry = begin_entry(b, kind, offset, sizeof(HSD_FObjDesc),
                                     &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;

    const uint8_t *raw = mv_dat_span(b->dat, offset, 20);
    if (!raw) { b->status = -1; return NULL; }
    HSD_FObjDesc *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_fobj(b, target, tobj))) return NULL;

    out->length = mv_be32(raw + 4);
    out->startframe = be_float(raw + 8);
    out->type = raw[12];
    out->frac_value = raw[13];
    out->frac_slope = raw[14];
    out->dummy0 = raw[15];
    const unsigned max_type = tobj ? 24u : 13u;
    if (!out->length || !isfinite(out->startframe) || out->type < 1u ||
        out->type > max_type) {
        b->status = -2;
        return NULL;
    }
    uint32_t stream_offset;
    if (pointer_offset(b->dat, offset + 16, &stream_offset) != 1) {
        b->status = -1;
        return NULL;
    }
    out->ad = (uint8_t *)(uintptr_t)mv_dat_span(b->dat, stream_offset,
                                                out->length);
    if (!out->ad) { b->status = -1; return NULL; }

    entry->state = 2;
    ++b->out->fobj_count;
    return out;
}

static HSD_AObjDesc *build_aobj(NativeBuild *b, uint32_t offset, int tobj)
{
    const uint8_t kind = tobj ? NK_AOBJ_TOBJ : NK_AOBJ_MOBJ;
    int existing;
    NativeEntry *entry = begin_entry(b, kind, offset, sizeof(HSD_AObjDesc),
                                     &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;

    const uint8_t *raw = mv_dat_span(b->dat, offset, 16);
    if (!raw) { b->status = -1; return NULL; }
    HSD_AObjDesc *out = entry->ptr;
    out->flags = mv_be32(raw);
    out->end_frame = be_float(raw + 4);
    out->obj_id = mv_be32(raw + 12);
    if (!isfinite(out->end_frame) || out->end_frame < 0.0f ||
        out->end_frame > 10000000.0f || out->obj_id != 0) {
        b->status = -2;
        return NULL;
    }
    uint32_t target;
    int result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->fobjdesc = build_fobj(b, target, tobj))) return NULL;

    entry->state = 2;
    ++b->out->aobj_count;
    return out;
}

static int build_image_table(NativeBuild *b, uint32_t field, uint16_t count,
                             HSD_ImageDesc ***out)
{
    *out = NULL;
    uint32_t table_offset;
    int result = pointer_offset(b->dat, field, &table_offset);
    if (!count) return result == 0 ? 0 : -1;
    if (count > MV_MATANIM_MAX_TABLE_ENTRIES || result != 1) return -1;

    HSD_ImageDesc **table = own_calloc(b, (size_t)count + 1u,
                                       sizeof(*table));
    if (!table) return -1;
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t image_offset;
        result = pointer_offset(b->dat, table_offset + (uint32_t)i * 4u,
                                &image_offset);
        if (result < 0) return -1;
        if (result && !(table[i] = build_image(b, image_offset))) return -1;
    }
    *out = table;
    return 0;
}

static int build_tlut_table(NativeBuild *b, uint32_t field, uint16_t count,
                            HSD_TlutDesc ***out)
{
    *out = NULL;
    uint32_t table_offset;
    int result = pointer_offset(b->dat, field, &table_offset);
    if (!count) return result == 0 ? 0 : -1;
    if (count > MV_MATANIM_MAX_TABLE_ENTRIES || result != 1) return -1;

    HSD_TlutDesc **table = own_calloc(b, (size_t)count + 1u,
                                      sizeof(*table));
    if (!table) return -1;
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t tlut_offset;
        result = pointer_offset(b->dat, table_offset + (uint32_t)i * 4u,
                                &tlut_offset);
        if (result != 1 || !(table[i] = build_tlut(b, tlut_offset))) return -1;
    }
    *out = table;
    return 0;
}

static HSD_TexAnim *build_texanim(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_TEXANIM, offset, sizeof(HSD_TexAnim),
                                     &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;

    const uint8_t *raw = mv_dat_span(b->dat, offset, 24);
    if (!raw) { b->status = -1; return NULL; }
    HSD_TexAnim *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_texanim(b, target))) return NULL;

    uint32_t id = mv_be32(raw + 4);
    if (id > GX_TEXMAP7) { b->status = -2; return NULL; }
    out->id = (GXTexMapID)id;
    result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->aobjdesc = build_aobj(b, target, 1))) return NULL;

    out->n_imagetbl = mv_be16(raw + 20);
    out->n_tluttbl = mv_be16(raw + 22);
    if (build_image_table(b, offset + 12, out->n_imagetbl, &out->imagetbl) ||
        build_tlut_table(b, offset + 16, out->n_tluttbl, &out->tluttbl)) {
        if (!b->status) b->status = -1;
        return NULL;
    }

    entry->state = 2;
    ++b->out->texanim_count;
    return out;
}

static HSD_MatAnim *build_matanim(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_MATANIM, offset, sizeof(HSD_MatAnim),
                                     &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;

    const uint8_t *raw = mv_dat_span(b->dat, offset, 16);
    if (!raw) { b->status = -1; return NULL; }
    HSD_MatAnim *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_matanim(b, target))) return NULL;

    result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->aobjdesc = build_aobj(b, target, 0))) return NULL;
    result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->texanim = build_texanim(b, target))) return NULL;

    /* GmTtAll's title material animation has no RenderAnim graph. Supporting a
       serialized pointer here without ChanAnim/TevRegAnim conversion would be
       unsafe, so fail closed if a future asset introduces one. */
    result = pointer_offset(b->dat, offset + 12, &target);
    if (result != 0) {
        b->status = result < 0 ? -1 : -2;
        return NULL;
    }
    out->renderanim = NULL;

    entry->state = 2;
    ++b->out->matanim_count;
    return out;
}

static HSD_MatAnimJoint *build_matjoint(NativeBuild *b, uint32_t offset)
{
    int existing;
    NativeEntry *entry = begin_entry(b, NK_MATJOINT, offset,
                                     sizeof(HSD_MatAnimJoint), &existing);
    if (!entry || b->status) return NULL;
    if (existing) return entry->ptr;

    if (!mv_dat_span(b->dat, offset, 12)) { b->status = -1; return NULL; }
    HSD_MatAnimJoint *out = entry->ptr;
    uint32_t target;
    int result = pointer_offset(b->dat, offset, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->child = build_matjoint(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 4, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->next = build_matjoint(b, target))) return NULL;
    result = pointer_offset(b->dat, offset + 8, &target);
    if (result < 0) { b->status = -1; return NULL; }
    if (result && !(out->matanim = build_matanim(b, target))) return NULL;

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

int mv_native_matanim_build_at(const MvDat *dat, uint32_t root_offset,
                               MvNativeMatAnim *out)
{
    if (!dat || !out || !dat->pointer_bits || root_offset > dat->data_size) return -1;
    memset(out, 0, sizeof(*out));

    NativeBuild b = {.dat = dat, .out = out};
    b.entries = calloc(MV_MATANIM_MAX_NODES, sizeof(*b.entries));
    b.owned = calloc(MV_MATANIM_MAX_NODES, sizeof(*b.owned));
    if (!b.entries || !b.owned) {
        free(b.entries);
        free(b.owned);
        return -1;
    }

    out->root = build_matjoint(&b, root_offset);
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

int mv_native_matanim_build(const MvDat *dat, const char *name,
                            MvNativeMatAnim *out)
{
    if (!dat || !name || !out || !dat->pointer_bits) return -1;
    uint32_t root_offset = UINT32_MAX;
    for (uint32_t i = 0; i < dat->public_count; ++i) {
        const char *public_name; uint32_t offset;
        if (mv_dat_public(dat, i, &public_name, &offset)) return -1;
        if (!strcmp(name, public_name)) { root_offset = offset; break; }
    }
    if (root_offset == UINT32_MAX) return -1;
    return mv_native_matanim_build_at(dat, root_offset, out);
}

void mv_native_matanim_free(MvNativeMatAnim *anim)
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
