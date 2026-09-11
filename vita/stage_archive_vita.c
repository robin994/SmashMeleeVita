#include <melee/gr/types.h>
#include <melee/it/forward.h>
#include <melee/mp/types.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/debug.h>
#include <sysdolphin/baselib/forward.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/pobj.h>

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "hsd_data.h"
#include "hsd_anim_native.h"
#include "hsd_matanim_native.h"
#include "hsd_native.h"

typedef struct StageJointMapEntry {
    void* joint;
    s16* pairs;
    s32 pair_count;
} StageJointMapEntry;

enum StageHsdRawKind {
    STAGE_HSD_RAW_JOINT = 1,
    STAGE_HSD_RAW_DOBJ,
    STAGE_HSD_RAW_MOBJ,
    STAGE_HSD_RAW_POBJ,
    STAGE_HSD_RAW_TOBJ,
    STAGE_HSD_RAW_VTX,
    STAGE_HSD_RAW_IMAGE,
    STAGE_HSD_RAW_TLUT,
    STAGE_HSD_RAW_MATERIAL,
    STAGE_HSD_RAW_LOD,
    STAGE_HSD_RAW_TEV,
    STAGE_HSD_RAW_MTX,
    STAGE_HSD_RAW_ENVELOPE,
    STAGE_HSD_RAW_ENVELOPE_DESC,
    STAGE_HSD_RAW_ANIM_JOINT,
    STAGE_HSD_RAW_AOBJ,
    STAGE_HSD_RAW_FOBJ,
    STAGE_HSD_RAW_MATANIM_JOINT,
    STAGE_HSD_RAW_MATANIM,
    STAGE_HSD_RAW_TEXANIM,
    STAGE_HSD_RAW_SPLINE,
    STAGE_HSD_RAW_SPLINE_CV,
    STAGE_HSD_RAW_SPLINE_SEG_LENGTH,
    STAGE_HSD_RAW_SPLINE_SEG_POLY,
    STAGE_HSD_RAW_SHAPE_JOINT,
    STAGE_HSD_RAW_SHAPE_DOBJ,
    STAGE_HSD_RAW_KIND_COUNT,
};

#define STAGE_HSD_RAW_MAX_SEEN 32768u
#define STAGE_HSD_RAW_MAX_VTXDESC 32u
#define STAGE_HSD_RAW_MAX_ENVELOPE_MATRICES 32u
#define STAGE_HSD_RAW_MAX_ENVELOPE_WEIGHTS 32u

typedef struct StageHsdRawSeen {
    u32 offset;
    u8 kind;
} StageHsdRawSeen;

typedef struct StageHsdRawContext {
    MvDat* dat;
    StageHsdRawSeen* seen;
    size_t seen_count;
    u32 converted[STAGE_HSD_RAW_KIND_COUNT];
} StageHsdRawContext;

static u32 stage_be32(const void* ptr)
{
    const u8* p = ptr;
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static float stage_be_float(const void* ptr)
{
    u32 bits = stage_be32(ptr);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void stage_require_range(HSD_Archive* archive, const void* ptr, size_t size)
{
    uintptr_t start;
    uintptr_t value;

    if (ptr == NULL || archive == NULL || archive->data == NULL) {
        HSD_Panic(__FILE__, __LINE__, "stage descriptor is null");
    }

    start = (uintptr_t) archive->data;
    value = (uintptr_t) ptr;
    if (value < start || size > archive->header.data_size ||
        value - start > archive->header.data_size - size)
    {
        HSD_Panic(__FILE__, __LINE__, "stage descriptor outside archive");
    }
}

static size_t stage_public_root_span(HSD_Archive* archive, const char* symbol,
                                     const void* root, size_t min_size)
{
    u32 root_offset = UINT32_MAX;
    u32 next_offset;

    if (archive == NULL || archive->data == NULL || archive->public_info == NULL ||
        archive->symbols == NULL || symbol == NULL || root == NULL)
    {
        HSD_Panic(__FILE__, __LINE__, "invalid stage public root");
    }

    for (u32 i = 0; i < archive->header.nb_public; ++i) {
        u32 symbol_offset = stage_be32(&archive->public_info[i].symbol);
        u32 data_offset = stage_be32(&archive->public_info[i].offset);

        if (data_offset > archive->header.data_size) {
            OSReport("VITA_DAT_ROOT_INVALID symbol=%s reason=public_offset offset=%u data=%u\n",
                     symbol, data_offset, archive->header.data_size);
            HSD_Panic(__FILE__, __LINE__, "stage public root offset invalid");
        }
        if (strcmp(archive->symbols + symbol_offset, symbol) == 0) {
            root_offset = data_offset;
            break;
        }
    }

    if (root_offset == UINT32_MAX || archive->data + root_offset != root) {
        OSReport("VITA_DAT_ROOT_INVALID symbol=%s reason=lookup root=%p\n", symbol,
                 root);
        HSD_Panic(__FILE__, __LINE__, "stage public root mismatch");
    }

    next_offset = archive->header.data_size;
    for (u32 i = 0; i < archive->header.nb_public; ++i) {
        u32 data_offset = stage_be32(&archive->public_info[i].offset);
        if (data_offset > root_offset && data_offset < next_offset) {
            next_offset = data_offset;
        }
    }

    if (next_offset < root_offset || next_offset - root_offset < min_size) {
        OSReport("VITA_DAT_ROOT_INVALID symbol=%s reason=span start=%u end=%u min=%u\n",
                 symbol, root_offset, next_offset, (unsigned) min_size);
        HSD_Panic(__FILE__, __LINE__, "stage public root too small");
    }

    OSReport("VITA_DAT_ROOT_SPAN symbol=%s offset=%08x size=%u\n", symbol,
             root_offset, next_offset - root_offset);
    return (size_t) (next_offset - root_offset);
}

static void stage_swap16(void* ptr)
{
    uint16_t value;
    memcpy(&value, ptr, sizeof(value));
    value = __builtin_bswap16(value);
    memcpy(ptr, &value, sizeof(value));
}

static void stage_swap32(void* ptr)
{
    uint32_t value;
    memcpy(&value, ptr, sizeof(value));
    value = __builtin_bswap32(value);
    memcpy(ptr, &value, sizeof(value));
}

static void stage_hsd_raw_fail(StageHsdRawContext* ctx, const char* what,
                               u32 offset)
{
    OSReport("VITA_STAGE_HSD_RAW_INVALID kind=%s offset=%08x data=%u\n", what,
             offset, ctx != NULL && ctx->dat != NULL ? ctx->dat->data_size : 0);
    HSD_Panic(__FILE__, __LINE__, "stage HSD descriptor invalid");
}

static u8* stage_hsd_raw_span(StageHsdRawContext* ctx, u32 offset, size_t size,
                              const char* what)
{
    const u8* span = mv_dat_span(ctx->dat, offset, size);
    if (span == NULL) {
        stage_hsd_raw_fail(ctx, what, offset);
    }
    return (u8*) (uintptr_t) span;
}

static int stage_hsd_raw_pointer(StageHsdRawContext* ctx, u32 field,
                                 uint32_t* target, const char* what)
{
    int result = mv_dat_pointer(ctx->dat, field, target);
    if (result < 0) {
        stage_hsd_raw_fail(ctx, what, field);
    }
    return result;
}

static int stage_hsd_raw_seen(StageHsdRawContext* ctx, u8 kind, u32 offset)
{
    for (size_t i = 0; i < ctx->seen_count; ++i) {
        if (ctx->seen[i].kind == kind && ctx->seen[i].offset == offset) {
            return 1;
        }
    }
    if (ctx->seen_count >= STAGE_HSD_RAW_MAX_SEEN) {
        stage_hsd_raw_fail(ctx, "visited-overflow", offset);
    }
    ctx->seen[ctx->seen_count].kind = kind;
    ctx->seen[ctx->seen_count].offset = offset;
    ++ctx->seen_count;
    if (kind < STAGE_HSD_RAW_KIND_COUNT) {
        ++ctx->converted[kind];
    }
    return 0;
}

static void stage_hsd_raw_joint(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_dobj(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_mobj(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_pobj(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_tobj(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_anim_joint(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_matanim_joint(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_spline(StageHsdRawContext* ctx, u32 offset);
static void stage_hsd_raw_shape_joint(StageHsdRawContext* ctx, u32 offset);

static int stage_hsd_jobj_fobj_type_supported(u8 type)
{
    return (type >= 1 && type <= 12) || (type >= 20 && type <= 42);
}

static void stage_hsd_raw_matrix(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_MTX, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, sizeof(float) * 12,
                                 "matrix");
    for (unsigned i = 0; i < 12; ++i) {
        stage_swap32(raw + i * 4);
    }
}

static void stage_hsd_raw_image(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_IMAGE, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 24, "image");
    stage_swap16(raw + 4);
    stage_swap16(raw + 6);
    stage_swap32(raw + 8);
    stage_swap32(raw + 12);
    stage_swap32(raw + 16);
    stage_swap32(raw + 20);
}

static void stage_hsd_raw_tlut(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_TLUT, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 16, "tlut");
    stage_swap32(raw + 4);
    stage_swap32(raw + 8);
    stage_swap16(raw + 12);
}

static void stage_hsd_raw_lod(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_LOD, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 16, "tex-lod");
    stage_swap32(raw + 0);
    stage_swap32(raw + 4);
    stage_swap32(raw + 12);
}

static void stage_hsd_raw_tev(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_TEV, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 32, "tobj-tev");
    /* Ops and colors are serialized byte fields. Only active is a host u32. */
    stage_swap32(raw + 28);
}

static void stage_hsd_raw_material(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_MATERIAL, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 20, "material");
    /* The three GXColor values are byte arrays; alpha/shininess are floats. */
    stage_swap32(raw + 12);
    stage_swap32(raw + 16);
}

static void stage_hsd_raw_vtx(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_VTX, offset)) {
        return;
    }

    u32 cursor = offset;
    for (unsigned i = 0; i < STAGE_HSD_RAW_MAX_VTXDESC; ++i, cursor += 24) {
        u8* raw = stage_hsd_raw_span(ctx, cursor, 24, "vtxdesc");
        u32 attr = mv_be32(raw);
        stage_swap32(raw + 0);
        if (attr == GX_VA_NULL) {
            return;
        }
        stage_swap32(raw + 4);
        stage_swap32(raw + 8);
        stage_swap32(raw + 12);
        stage_swap16(raw + 18);
    }
    stage_hsd_raw_fail(ctx, "vtxdesc-unterminated", offset);
}

static void stage_hsd_raw_envelope(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_ENVELOPE, offset)) {
        return;
    }

    for (unsigned matrix = 0; matrix < STAGE_HSD_RAW_MAX_ENVELOPE_MATRICES;
         ++matrix)
    {
        uint32_t desc_offset;
        int matrix_result = stage_hsd_raw_pointer(
            ctx, offset + matrix * 4, &desc_offset, "envelope-matrix");
        if (matrix_result == 0) {
            return;
        }

        if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_ENVELOPE_DESC,
                               desc_offset)) {
            continue;
        }

        for (unsigned weight = 0;
             weight < STAGE_HSD_RAW_MAX_ENVELOPE_WEIGHTS; ++weight)
        {
            uint32_t joint_offset;
            uint32_t entry_offset = desc_offset + weight * 8;
            u8* raw = stage_hsd_raw_span(ctx, entry_offset, 8,
                                         "envelope-weight");
            int joint_result = stage_hsd_raw_pointer(
                ctx, entry_offset, &joint_offset, "envelope-joint");
            if (joint_result == 0) {
                break;
            }
            stage_swap32(raw + 4);
            stage_hsd_raw_joint(ctx, joint_offset);
            if (weight + 1 == STAGE_HSD_RAW_MAX_ENVELOPE_WEIGHTS) {
                stage_hsd_raw_fail(ctx, "envelope-weight-unterminated",
                                   desc_offset);
            }
        }
        if (matrix + 1 == STAGE_HSD_RAW_MAX_ENVELOPE_MATRICES) {
            stage_hsd_raw_fail(ctx, "envelope-matrix-unterminated", offset);
        }
    }
}

static void stage_hsd_raw_tobj(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_TOBJ, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 92, "tobj");
    uint32_t target;

    if (stage_hsd_raw_pointer(ctx, offset + 4, &target, "tobj.next")) {
        stage_hsd_raw_tobj(ctx, target);
    }

    stage_swap32(raw + 8);
    stage_swap32(raw + 12);
    for (unsigned field = 0x10; field <= 0x30; field += 4) {
        stage_swap32(raw + field);
    }
    stage_swap32(raw + 0x34);
    stage_swap32(raw + 0x38);
    stage_swap32(raw + 0x40);
    stage_swap32(raw + 0x44);
    stage_swap32(raw + 0x48);

    if (stage_hsd_raw_pointer(ctx, offset + 0x4C, &target,
                              "tobj.image")) {
        stage_hsd_raw_image(ctx, target);
    } else {
        stage_hsd_raw_fail(ctx, "tobj.image-null", offset);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 0x50, &target,
                              "tobj.tlut")) {
        stage_hsd_raw_tlut(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 0x54, &target,
                              "tobj.lod")) {
        stage_hsd_raw_lod(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 0x58, &target,
                              "tobj.tev")) {
        stage_hsd_raw_tev(ctx, target);
    }
}

static void stage_hsd_raw_pobj(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_POBJ, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 24, "pobj");
    u16 flags = mv_be16(raw + 12);
    u16 type = flags & 0x3000;
    uint32_t target;

    if (type == POBJ_SHAPEANIM || type == 0x3000) {
        stage_hsd_raw_fail(ctx, "pobj-unsupported-type", offset);
    }

    if (stage_hsd_raw_pointer(ctx, offset + 4, &target, "pobj.next")) {
        stage_hsd_raw_pobj(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "pobj.verts")) {
        stage_hsd_raw_vtx(ctx, target);
    } else {
        stage_hsd_raw_fail(ctx, "pobj.verts-null", offset);
    }

    stage_swap16(raw + 12);
    stage_swap16(raw + 14);

    int union_result = stage_hsd_raw_pointer(ctx, offset + 20, &target,
                                             "pobj.union");
    if (type == POBJ_ENVELOPE) {
        if (!union_result) {
            stage_hsd_raw_fail(ctx, "pobj.envelope-null", offset);
        }
        stage_hsd_raw_envelope(ctx, target);
    } else if (union_result) {
        /* POBJ_SKIN stores a descriptor reference to another JObj in the same
         * structural tree. Convert that target's scalar graph as well; the
         * runtime HSD ID table resolves the relocated descriptor identity. */
        stage_hsd_raw_joint(ctx, target);
    }
}

static void stage_hsd_raw_mobj(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_MOBJ, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 24, "mobj");
    uint32_t target;

    stage_swap32(raw + 4);
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "mobj.tobj")) {
        stage_hsd_raw_tobj(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 12, &target, "mobj.material")) {
        stage_hsd_raw_material(ctx, target);
    } else {
        stage_hsd_raw_fail(ctx, "mobj.material-null", offset);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 16, &target, "mobj.renderdesc")) {
        stage_hsd_raw_fail(ctx, "mobj.renderdesc", offset);
    }
    /* PEDesc is a packed 12-byte structure and contains no host-endian scalar. */
}

static void stage_hsd_raw_dobj(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_DOBJ, offset)) {
        return;
    }
    (void) stage_hsd_raw_span(ctx, offset, 16, "dobj");
    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset + 4, &target, "dobj.next")) {
        stage_hsd_raw_dobj(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "dobj.mobj")) {
        stage_hsd_raw_mobj(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 12, &target, "dobj.pobj")) {
        stage_hsd_raw_pobj(ctx, target);
    }
}

static void stage_hsd_raw_joint(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_JOINT, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 64, "joint");
    u32 flags = mv_be32(raw + 4);
    uint32_t target;

    if (flags & (JOBJ_PTCL | JOBJ_INSTANCE)) {
        OSReport("VITA_STAGE_HSD_RAW_UNSUPPORTED kind=joint-flags offset=%08x flags=%08x\n",
                 offset, flags);
        HSD_Panic(__FILE__, __LINE__, "stage HSD joint requires adapter");
    }

    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "joint.child")) {
        stage_hsd_raw_joint(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 12, &target, "joint.next")) {
        stage_hsd_raw_joint(ctx, target);
    }
    int union_result = stage_hsd_raw_pointer(ctx, offset + 16, &target,
                                             "joint.union");
    if (flags & JOBJ_SPLINE) {
        if (!union_result) {
            stage_hsd_raw_fail(ctx, "joint.spline-null", offset);
        }
        stage_hsd_raw_spline(ctx, target);
    } else if (union_result) {
        stage_hsd_raw_dobj(ctx, target);
    }

    stage_swap32(raw + 4);
    for (unsigned field = 0x14; field <= 0x34; field += 4) {
        stage_swap32(raw + field);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 0x38, &target, "joint.matrix")) {
        stage_hsd_raw_matrix(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 0x3C, &target, "joint.robj")) {
        stage_hsd_raw_fail(ctx, "joint.robj", offset);
    }
}

static void stage_hsd_raw_swap_float_array(StageHsdRawContext* ctx, u8 kind,
                                           u32 offset, size_t count,
                                           const char* what)
{
    if (stage_hsd_raw_seen(ctx, kind, offset)) {
        return;
    }
    if (count > SIZE_MAX / sizeof(float)) {
        stage_hsd_raw_fail(ctx, what, offset);
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, count * sizeof(float), what);
    for (size_t i = 0; i < count; ++i) {
        float value = stage_be_float(raw + i * 4);
        if (!isfinite(value)) {
            stage_hsd_raw_fail(ctx, what, offset + (u32)i * 4);
        }
        stage_swap32(raw + i * 4);
    }
}

static void stage_hsd_raw_spline(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_SPLINE, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 24, "spline");
    u8 type = raw[0];
    s16 numcv = (s16)mv_be16(raw + 2);
    float tension = stage_be_float(raw + 4);
    float total_length = stage_be_float(raw + 0x0C);
    if (type > 3 || numcv < 2 || numcv > 4096 || !isfinite(tension) ||
        !isfinite(total_length) || total_length < 0.0f)
    {
        stage_hsd_raw_fail(ctx, "spline.scalar", offset);
    }

    size_t cv_count;
    switch (type) {
    case 0: cv_count = (size_t)numcv; break;
    case 1: cv_count = (size_t)numcv * 3u - 2u; break;
    case 2:
    case 3: cv_count = (size_t)numcv + 2u; break;
    default: cv_count = 0; break;
    }

    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "spline.cv") != 1) {
        stage_hsd_raw_fail(ctx, "spline.cv", offset);
    }
    stage_hsd_raw_swap_float_array(ctx, STAGE_HSD_RAW_SPLINE_CV, target,
                                   cv_count * 3u, "spline.cv");

    if (stage_hsd_raw_pointer(ctx, offset + 0x10, &target,
                              "spline.segLength") != 1)
    {
        stage_hsd_raw_fail(ctx, "spline.segLength", offset);
    }
    stage_hsd_raw_swap_float_array(ctx, STAGE_HSD_RAW_SPLINE_SEG_LENGTH,
                                   target, (size_t)numcv,
                                   "spline.segLength");

    if (stage_hsd_raw_pointer(ctx, offset + 0x14, &target,
                              "spline.segPoly") != 1)
    {
        stage_hsd_raw_fail(ctx, "spline.segPoly", offset);
    }
    stage_hsd_raw_swap_float_array(ctx, STAGE_HSD_RAW_SPLINE_SEG_POLY,
                                   target, ((size_t)numcv - 1u) * 5u,
                                   "spline.segPoly");

    stage_swap16(raw + 2);
    stage_swap32(raw + 4);
    stage_swap32(raw + 0x0C);
}

static void stage_hsd_raw_fobj(StageHsdRawContext* ctx, u32 offset,
                               unsigned max_type)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_FOBJ, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 20, "fobj");
    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset, &target, "fobj.next")) {
        stage_hsd_raw_fobj(ctx, target, max_type);
    }

    u32 length = mv_be32(raw + 4);
    float start = stage_be_float(raw + 8);
    u8 type = raw[12];
    int type_ok = max_type == 12
                      ? stage_hsd_jobj_fobj_type_supported(type)
                      : (type >= 1 && type <= max_type);
    if (length == 0 || !isfinite(start) || !type_ok) {
        stage_hsd_raw_fail(ctx, "fobj.scalar", offset);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 16, &target, "fobj.stream") != 1 ||
        mv_dat_span(ctx->dat, target, length) == NULL)
    {
        stage_hsd_raw_fail(ctx, "fobj.stream", offset);
    }
    stage_swap32(raw + 4);
    stage_swap32(raw + 8);
}

static void stage_hsd_raw_aobj(StageHsdRawContext* ctx, u32 offset,
                               unsigned max_fobj_type)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_AOBJ, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 16, "aobj");
    uint32_t target;
    float end_frame = stage_be_float(raw + 4);
    if (!isfinite(end_frame) || end_frame < 0.0f) {
        stage_hsd_raw_fail(ctx, "aobj.scalar", offset);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "aobj.fobj")) {
        stage_hsd_raw_fobj(ctx, target, max_fobj_type);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 12, &target, "aobj.obj_id")) {
        if (max_fobj_type != 12) {
            stage_hsd_raw_fail(ctx, "aobj.obj_id-nonjobj", offset);
        }
        stage_hsd_raw_joint(ctx, target);
    }
    stage_swap32(raw + 0);
    stage_swap32(raw + 4);
}

static void stage_hsd_raw_anim_joint(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_ANIM_JOINT, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 20, "anim-joint");
    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset, &target, "anim.child")) {
        stage_hsd_raw_anim_joint(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 4, &target, "anim.next")) {
        stage_hsd_raw_anim_joint(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "anim.aobj")) {
        stage_hsd_raw_aobj(ctx, target, 12);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 12, &target, "anim.legacy") != 0) {
        stage_hsd_raw_fail(ctx, "anim.legacy", offset);
    }
    stage_swap32(raw + 16);
}

static void stage_hsd_raw_texanim(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_TEXANIM, offset)) {
        return;
    }
    u8* raw = stage_hsd_raw_span(ctx, offset, 24, "texanim");
    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset, &target, "texanim.next")) {
        stage_hsd_raw_texanim(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "texanim.aobj")) {
        stage_hsd_raw_aobj(ctx, target, 24);
    }

    u16 image_count = mv_be16(raw + 20);
    u16 tlut_count = mv_be16(raw + 22);
    if (image_count > 1024 || tlut_count > 1024) {
        stage_hsd_raw_fail(ctx, "texanim.table-count", offset);
    }
    int image_result = stage_hsd_raw_pointer(ctx, offset + 12, &target,
                                             "texanim.images");
    if ((image_count == 0 && image_result != 0) ||
        (image_count != 0 && image_result != 1))
    {
        stage_hsd_raw_fail(ctx, "texanim.images", offset);
    }
    if (image_result == 1) {
        uint32_t table = target;
        for (u16 i = 0; i < image_count; ++i) {
            if (stage_hsd_raw_pointer(ctx, table + (u32)i * 4, &target,
                                      "texanim.image-entry") != 1)
            {
                stage_hsd_raw_fail(ctx, "texanim.image-entry", table);
            }
            stage_hsd_raw_image(ctx, target);
        }
    }

    int tlut_result = stage_hsd_raw_pointer(ctx, offset + 16, &target,
                                            "texanim.tluts");
    if ((tlut_count == 0 && tlut_result != 0) ||
        (tlut_count != 0 && tlut_result != 1))
    {
        stage_hsd_raw_fail(ctx, "texanim.tluts", offset);
    }
    if (tlut_result == 1) {
        uint32_t table = target;
        for (u16 i = 0; i < tlut_count; ++i) {
            if (stage_hsd_raw_pointer(ctx, table + (u32)i * 4, &target,
                                      "texanim.tlut-entry") != 1)
            {
                stage_hsd_raw_fail(ctx, "texanim.tlut-entry", table);
            }
            stage_hsd_raw_tlut(ctx, target);
        }
    }

    stage_swap32(raw + 4);
    stage_swap16(raw + 20);
    stage_swap16(raw + 22);
}

static void stage_hsd_raw_matanim(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_MATANIM, offset)) {
        return;
    }
    (void) stage_hsd_raw_span(ctx, offset, 16, "matanim");
    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset, &target, "matanim.next")) {
        stage_hsd_raw_matanim(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 4, &target, "matanim.aobj")) {
        stage_hsd_raw_aobj(ctx, target, 13);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "matanim.texanim")) {
        stage_hsd_raw_texanim(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 12, &target, "matanim.renderanim") != 0) {
        stage_hsd_raw_fail(ctx, "matanim.renderanim", offset);
    }
}

static void stage_hsd_raw_matanim_joint(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_MATANIM_JOINT, offset)) {
        return;
    }
    (void) stage_hsd_raw_span(ctx, offset, 12, "matanim-joint");
    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset, &target, "matanim-joint.child")) {
        stage_hsd_raw_matanim_joint(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 4, &target, "matanim-joint.next")) {
        stage_hsd_raw_matanim_joint(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "matanim-joint.anim")) {
        stage_hsd_raw_matanim(ctx, target);
    }
}

static void stage_hsd_raw_shape_dobj(StageHsdRawContext* ctx, u32 offset)
{
    for (unsigned guard = 0; offset != UINT32_MAX && guard < 4096; ++guard) {
        if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_SHAPE_DOBJ, offset)) {
            return;
        }
        (void)stage_hsd_raw_span(ctx, offset, 8, "shape-dobj");
        uint32_t target;
        int shape_result = stage_hsd_raw_pointer(ctx, offset + 4, &target,
                                                 "shape-dobj.anim");
        if (shape_result != 0) {
            /* EfCoData's retail shape trees are structural placeholders only.
               Keep a real morph-animation payload fail-closed until the
               ShapeSet/AObj adapter is implemented and hardware-tested. */
            stage_hsd_raw_fail(ctx, "shape-dobj.payload", offset);
        }
        int next_result = stage_hsd_raw_pointer(ctx, offset, &target,
                                                "shape-dobj.next");
        if (!next_result) return;
        offset = target;
    }
    if (offset != UINT32_MAX) {
        stage_hsd_raw_fail(ctx, "shape-dobj-unterminated", offset);
    }
}

static void stage_hsd_raw_shape_joint(StageHsdRawContext* ctx, u32 offset)
{
    if (stage_hsd_raw_seen(ctx, STAGE_HSD_RAW_SHAPE_JOINT, offset)) {
        return;
    }
    (void)stage_hsd_raw_span(ctx, offset, 12, "shape-joint");
    uint32_t target;
    if (stage_hsd_raw_pointer(ctx, offset, &target, "shape-joint.child")) {
        stage_hsd_raw_shape_joint(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 4, &target, "shape-joint.next")) {
        stage_hsd_raw_shape_joint(ctx, target);
    }
    if (stage_hsd_raw_pointer(ctx, offset + 8, &target, "shape-joint.dobj")) {
        stage_hsd_raw_shape_dobj(ctx, target);
    }
}

static int stage_hsd_find_public(const MvDat* dat, const char* wanted,
                                 uint32_t* offset)
{
    for (u32 i = 0; i < dat->public_count; ++i) {
        const char* name;
        uint32_t target;
        if (mv_dat_public(dat, i, &name, &target) != 0) {
            return -1;
        }
        if (strcmp(name, wanted) == 0) {
            *offset = target;
            return 1;
        }
    }
    return 0;
}

static int stage_hsd_find_effect_public(const MvDat* dat, uint32_t* offset,
                                        const char** public_name)
{
    for (u32 i = 0; i < dat->public_count; ++i) {
        const char* name;
        uint32_t target;
        if (mv_dat_public(dat, i, &name, &target) != 0) {
            return -1;
        }
        size_t len = strlen(name);
        if (len >= 12 && strncmp(name, "eff", 3) == 0 &&
            strcmp(name + len - 9, "DataTable") == 0)
        {
            *offset = target;
            if (public_name != NULL) {
                *public_name = name;
            }
            return 1;
        }
    }
    return 0;
}

static int stage_hsd_name_ends_with(const char* name, const char* suffix)
{
    size_t name_len = strlen(name);
    size_t suffix_len = strlen(suffix);
    return name_len >= suffix_len &&
           strcmp(name + name_len - suffix_len, suffix) == 0;
}

void mv_fighter_archive_prepare_raw(void* bytes, size_t size, const char* filename)
{
    MvDat dat;
    uint32_t joint_root = UINT32_MAX;
    uint32_t matanim_root = UINT32_MAX;
    const char* joint_name = NULL;

    if (bytes == NULL || size == 0 || mv_dat_open(&dat, bytes, size) != 0) {
        return;
    }

    for (u32 i = 0; i < dat.public_count; ++i) {
        const char* name;
        uint32_t target;
        if (mv_dat_public(&dat, i, &name, &target) != 0) {
            mv_dat_close(&dat);
            return;
        }
        if (strncmp(name, "Ply", 3) != 0) {
            continue;
        }
        if (stage_hsd_name_ends_with(name, "5K_Share_joint")) {
            joint_root = target;
            joint_name = name;
        } else if (stage_hsd_name_ends_with(name,
                                            "5K_Share_matanim_joint")) {
            matanim_root = target;
        }
    }
    if (joint_root == UINT32_MAX) {
        mv_dat_close(&dat);
        return;
    }

    /* Validate the optional material-animation graph before touching the DAT.
       The model graph itself is validated structurally by the same bounded raw
       walker used for stages; that walker deliberately supports more retail
       JObj flag combinations than the immutable menu/title proxy builder. */
    if (matanim_root != UINT32_MAX) {
        MvNativeMatAnim probe;
        int result = mv_native_matanim_build_at(&dat, matanim_root, &probe);
        if (result != 0) {
            OSReport("VITA_FIGHTER_HSD_RAW_VALIDATE_FAIL file=%s root=%s kind=matanim code=%d off=%08x\n",
                     filename != NULL ? filename : "?",
                     joint_name != NULL ? joint_name : "?", result,
                     matanim_root);
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "fighter MatAnim graph unsupported");
        }
        mv_native_matanim_free(&probe);
    }

    StageHsdRawContext ctx = { 0 };
    ctx.dat = &dat;
    ctx.seen = calloc(STAGE_HSD_RAW_MAX_SEEN, sizeof(*ctx.seen));
    if (ctx.seen == NULL) {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "fighter raw visited allocation failed");
    }

    stage_hsd_raw_joint(&ctx, joint_root);
    if (matanim_root != UINT32_MAX) {
        stage_hsd_raw_matanim_joint(&ctx, matanim_root);
    }

    OSReport("VITA_FIGHTER_HSD_RAW_NATIVE_PASS file=%s root=%s joint_off=%08x matanim_off=%08x joints=%u dobjs=%u mobjs=%u pobjs=%u tobjs=%u vtx=%u images=%u tluts=%u matjoints=%u matanims=%u texanims=%u aobjs=%u fobjs=%u\n",
             filename != NULL ? filename : "?",
             joint_name != NULL ? joint_name : "?", joint_root,
             matanim_root,
             ctx.converted[STAGE_HSD_RAW_JOINT],
             ctx.converted[STAGE_HSD_RAW_DOBJ],
             ctx.converted[STAGE_HSD_RAW_MOBJ],
             ctx.converted[STAGE_HSD_RAW_POBJ],
             ctx.converted[STAGE_HSD_RAW_TOBJ],
             ctx.converted[STAGE_HSD_RAW_VTX],
             ctx.converted[STAGE_HSD_RAW_IMAGE],
             ctx.converted[STAGE_HSD_RAW_TLUT],
             ctx.converted[STAGE_HSD_RAW_MATANIM_JOINT],
             ctx.converted[STAGE_HSD_RAW_MATANIM],
             ctx.converted[STAGE_HSD_RAW_TEXANIM],
             ctx.converted[STAGE_HSD_RAW_AOBJ],
             ctx.converted[STAGE_HSD_RAW_FOBJ]);

    free(ctx.seen);
    mv_dat_close(&dat);
}

void mv_effect_archive_prepare_raw(void* bytes, size_t size, const char* filename)
{
    MvDat dat;
    uint32_t root_offset = 0;
    const char* root_name = NULL;

    if (bytes == NULL || size == 0 || mv_dat_open(&dat, bytes, size) != 0) {
        return;
    }
    int public_result = stage_hsd_find_effect_public(&dat, &root_offset,
                                                     &root_name);
    if (public_result <= 0) {
        mv_dat_close(&dat);
        return;
    }
    if (mv_dat_span(&dat, root_offset, 8) == NULL) {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "effect data table root invalid");
    }

    /* EF_DAT_Entry overlays two particle-bank pointers followed by an inline
       EF_EffectDesc[] array. Every real effect descriptor owns a relocated
       model JObj pointer at +4, which gives us a type-safe table terminator
       without guessing from the following opaque payload. */
    u32 effect_count = 0;
    for (; effect_count < 256; ++effect_count) {
        uint32_t base = root_offset + 8 + effect_count * 20;
        if (mv_dat_span(&dat, base, 20) == NULL) {
            break;
        }
        uint32_t field = base + 4;
        if (!(dat.pointer_bits[field / 8] & (1u << (field % 8)))) {
            break;
        }
        uint32_t ignored;
        if (mv_dat_pointer(&dat, field, &ignored) != 1) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "effect JObj pointer invalid");
        }
    }
    if (effect_count == 256) {
        OSReport("VITA_EFFECT_HSD_RAW_INVALID file=%s root=%s effects=%u\n",
                 filename != NULL ? filename : "?",
                 root_name != NULL ? root_name : "?", effect_count);
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "effect descriptor table invalid");
    }
    if (effect_count == 0) {
        /* A small set of retail effect archives (including EfMnData.dat) are
         * particle-bank-only: the public EF_DAT_Entry contains the command and
         * texture bank pointers at +0/+4 and intentionally has no inline
         * EF_EffectDesc array at +8.  Keep the serialized bank pointers intact
         * for HSD_ArchiveParse; psInitDataBankLocate() nativeizes the particle
         * banks after relocation.  A zero-effect root without both bank
         * relocations is still malformed and fails closed. */
        uint32_t cmd_bank;
        uint32_t tex_bank;
        if (mv_dat_pointer(&dat, root_offset, &cmd_bank) != 1 ||
            mv_dat_pointer(&dat, root_offset + 4, &tex_bank) != 1)
        {
            OSReport("VITA_EFFECT_HSD_RAW_INVALID file=%s root=%s effects=0 reason=missing_particle_banks\n",
                     filename != NULL ? filename : "?",
                     root_name != NULL ? root_name : "?");
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "zero-effect archive missing particle banks");
        }
        OSReport("VITA_EFFECT_HSD_RAW_PARTICLE_ONLY_PASS file=%s root=%s cmd=%08x tex=%08x effects=0\n",
                 filename != NULL ? filename : "?",
                 root_name != NULL ? root_name : "?", cmd_bank, tex_bank);
        mv_dat_close(&dat);
        return;
    }

    size_t validated_joints = 0;
    size_t validated_pobjs = 0;
    u32 anim_count = 0;
    u32 matanim_count = 0;
    for (u32 i = 0; i < effect_count; ++i) {
        uint32_t base = root_offset + 8 + i * 20;
        const u8* raw = mv_dat_span(&dat, base, 20);
        float lifetime = stage_be_float(raw);
        if (!isfinite(lifetime) || lifetime < 0.0f || lifetime > 1000000.0f) {
            OSReport("VITA_EFFECT_HSD_RAW_VALIDATE_FAIL file=%s effect=%u reason=lifetime value=%f\n",
                     filename != NULL ? filename : "?", i, lifetime);
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "effect lifetime invalid");
        }

        uint32_t joint;
        if (mv_dat_pointer(&dat, base + 4, &joint) != 1) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "effect JObj missing");
        }
        MvNativeHsd hsd_probe;
        int result = mv_hsd_native_validate_raw_at(&dat, joint, &hsd_probe);
        if (result != 0) {
            OSReport("VITA_EFFECT_HSD_RAW_VALIDATE_FAIL file=%s effect=%u kind=model code=%d unsupported=%s off=%08x val=%08x\n",
                     filename != NULL ? filename : "?", i, result,
                     mv_hsd_native_unsupported_name(hsd_probe.unsupported_kind),
                     hsd_probe.unsupported_offset, hsd_probe.unsupported_value);
            mv_hsd_native_free(&hsd_probe);
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "effect HSD model unsupported");
        }
        validated_joints += hsd_probe.joint_count;
        validated_pobjs += hsd_probe.pobj_count;
        mv_hsd_native_free(&hsd_probe);

        uint32_t target;
        result = mv_dat_pointer(&dat, base + 8, &target);
        if (result < 0) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "effect AnimJoint pointer invalid");
        }
        if (result == 1) {
            int ar = mv_native_anim_validate_at(&dat, target);
            if (ar != 0) {
                OSReport("VITA_EFFECT_HSD_RAW_VALIDATE_FAIL file=%s effect=%u kind=anim code=%d off=%08x\n",
                         filename != NULL ? filename : "?", i, ar, target);
                mv_dat_close(&dat);
                HSD_Panic(__FILE__, __LINE__, "effect AnimJoint unsupported");
            }
            ++anim_count;
        }

        result = mv_dat_pointer(&dat, base + 12, &target);
        if (result < 0) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "effect MatAnim pointer invalid");
        }
        if (result == 1) {
            MvNativeMatAnim probe;
            int mr = mv_native_matanim_build_at(&dat, target, &probe);
            if (mr != 0) {
                OSReport("VITA_EFFECT_HSD_RAW_VALIDATE_FAIL file=%s effect=%u kind=matanim code=%d off=%08x\n",
                         filename != NULL ? filename : "?", i, mr, target);
                mv_dat_close(&dat);
                HSD_Panic(__FILE__, __LINE__, "effect MatAnim unsupported");
            }
            mv_native_matanim_free(&probe);
            ++matanim_count;
        }

        result = mv_dat_pointer(&dat, base + 16, &target);
        if (result < 0) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "effect ShapeAnim pointer invalid");
        }
    }

    StageHsdRawContext ctx = { 0 };
    ctx.dat = &dat;
    ctx.seen = calloc(STAGE_HSD_RAW_MAX_SEEN, sizeof(*ctx.seen));
    if (ctx.seen == NULL) {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "effect raw visited allocation failed");
    }

    for (u32 i = 0; i < effect_count; ++i) {
        uint32_t base = root_offset + 8 + i * 20;
        u8* raw = (u8*) (uintptr_t) mv_dat_span(&dat, base, 20);
        uint32_t target;
        stage_swap32(raw);
        if (stage_hsd_raw_pointer(&ctx, base + 4, &target, "effect.joint") != 1) {
            stage_hsd_raw_fail(&ctx, "effect.joint", base);
        }
        stage_hsd_raw_joint(&ctx, target);
        if (stage_hsd_raw_pointer(&ctx, base + 8, &target, "effect.anim") == 1) {
            stage_hsd_raw_anim_joint(&ctx, target);
        }
        if (stage_hsd_raw_pointer(&ctx, base + 12, &target, "effect.matanim") == 1) {
            stage_hsd_raw_matanim_joint(&ctx, target);
        }
        if (stage_hsd_raw_pointer(&ctx, base + 16, &target, "effect.shapeanim") == 1) {
            stage_hsd_raw_shape_joint(&ctx, target);
        }
    }

    OSReport("VITA_EFFECT_HSD_RAW_NATIVE_PASS file=%s root=%s effects=%u validate_joints=%u validate_pobjs=%u anims=%u matanims=%u joints=%u dobjs=%u mobjs=%u pobjs=%u tobjs=%u vtx=%u images=%u tluts=%u animjoints=%u aobjs=%u fobjs=%u matjoints=%u matanims_raw=%u texanims=%u splines=%u shapejoints=%u shapedobjs=%u\n",
             filename != NULL ? filename : "?",
             root_name != NULL ? root_name : "?", effect_count,
             (unsigned) validated_joints, (unsigned) validated_pobjs,
             anim_count, matanim_count,
             ctx.converted[STAGE_HSD_RAW_JOINT],
             ctx.converted[STAGE_HSD_RAW_DOBJ],
             ctx.converted[STAGE_HSD_RAW_MOBJ],
             ctx.converted[STAGE_HSD_RAW_POBJ],
             ctx.converted[STAGE_HSD_RAW_TOBJ],
             ctx.converted[STAGE_HSD_RAW_VTX],
             ctx.converted[STAGE_HSD_RAW_IMAGE],
             ctx.converted[STAGE_HSD_RAW_TLUT],
             ctx.converted[STAGE_HSD_RAW_ANIM_JOINT],
             ctx.converted[STAGE_HSD_RAW_AOBJ],
             ctx.converted[STAGE_HSD_RAW_FOBJ],
             ctx.converted[STAGE_HSD_RAW_MATANIM_JOINT],
             ctx.converted[STAGE_HSD_RAW_MATANIM],
             ctx.converted[STAGE_HSD_RAW_TEXANIM],
             ctx.converted[STAGE_HSD_RAW_SPLINE],
             ctx.converted[STAGE_HSD_RAW_SHAPE_JOINT],
             ctx.converted[STAGE_HSD_RAW_SHAPE_DOBJ]);

    free(ctx.seen);
    mv_dat_close(&dat);
}

void mv_stage_archive_prepare_raw(void* bytes, size_t size, const char* filename)
{
    MvDat dat;
    uint32_t map_head_offset;

    if (bytes == NULL || size == 0 || mv_dat_open(&dat, bytes, size) != 0) {
        return;
    }

    int public_result = stage_hsd_find_public(&dat, "map_head", &map_head_offset);
    if (public_result <= 0) {
        mv_dat_close(&dat);
        return;
    }

    const u8* map_head = mv_dat_span(&dat, map_head_offset, 0x30);
    if (map_head == NULL) {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "stage raw map_head invalid");
    }

    u32 map_count = mv_be32(map_head + 0x0C);
    if (map_count > 0x400) {
        OSReport("VITA_STAGE_HSD_RAW_INVALID kind=map-count value=%u file=%s\n",
                 map_count, filename != NULL ? filename : "?");
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "stage raw map count invalid");
    }

    uint32_t map_entries = 0;
    int maps_result = mv_dat_pointer(&dat, map_head_offset + 8, &map_entries);
    if (maps_result < 0 || (map_count != 0 && maps_result != 1) ||
        (map_count != 0 && mv_dat_span(&dat, map_entries,
                                      (size_t) map_count * 0x34) == NULL))
    {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "stage raw map table invalid");
    }

    uint32_t* roots = NULL;
    if (map_count != 0) {
        roots = calloc(map_count, sizeof(*roots));
        if (roots == NULL) {
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "stage raw root allocation failed");
        }
    }

    u32 root_count = 0;
    size_t validated_joints = 0;
    size_t validated_dobjs = 0;
    size_t validated_pobjs = 0;
    for (u32 i = 0; i < map_count; ++i) {
        uint32_t root;
        int result = mv_dat_pointer(&dat, map_entries + i * 0x34, &root);
        if (result < 0) {
            free(roots);
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "stage raw JObj pointer invalid");
        }
        roots[i] = result == 1 ? root : UINT32_MAX;
        if (result != 1) {
            continue;
        }

        MvNativeHsd probe;
        /* Raw stages share the effect/fighter ABI, including typed splines.
         * The compact menu-proxy builder intentionally rejects these. */
        int probe_result = mv_hsd_native_validate_raw_at(&dat, root, &probe);
        if (probe_result != 0) {
            OSReport("VITA_STAGE_HSD_RAW_VALIDATE_FAIL file=%s map=%u root=%08x code=%d unsupported=%s off=%08x val=%08x\n",
                     filename != NULL ? filename : "?", i, root, probe_result,
                     mv_hsd_native_unsupported_name(probe.unsupported_kind),
                     probe.unsupported_offset, probe.unsupported_value);
            mv_hsd_native_free(&probe);
            free(roots);
            mv_dat_close(&dat);
            HSD_Panic(__FILE__, __LINE__, "stage raw HSD graph unsupported");
        }
        ++root_count;
        validated_joints += probe.joint_count;
        validated_dobjs += probe.dobj_count;
        validated_pobjs += probe.pobj_count;
        mv_hsd_native_free(&probe);
    }

    StageHsdRawContext ctx = { 0 };
    ctx.dat = &dat;
    ctx.seen = calloc(STAGE_HSD_RAW_MAX_SEEN, sizeof(*ctx.seen));
    if (ctx.seen == NULL) {
        free(roots);
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "stage raw visited allocation failed");
    }

    for (u32 i = 0; i < map_count; ++i) {
        if (roots[i] != UINT32_MAX) {
            stage_hsd_raw_joint(&ctx, roots[i]);
        }
    }

    OSReport("VITA_STAGE_HSD_RAW_NATIVE_PASS file=%s maps=%u roots=%u validate_joints=%u validate_dobjs=%u validate_pobjs=%u joints=%u dobjs=%u mobjs=%u pobjs=%u tobjs=%u vtx=%u images=%u tluts=%u matrices=%u envelopes=%u splines=%u\n",
             filename != NULL ? filename : "?", map_count, root_count,
             (unsigned) validated_joints, (unsigned) validated_dobjs,
             (unsigned) validated_pobjs,
             ctx.converted[STAGE_HSD_RAW_JOINT],
             ctx.converted[STAGE_HSD_RAW_DOBJ],
             ctx.converted[STAGE_HSD_RAW_MOBJ],
             ctx.converted[STAGE_HSD_RAW_POBJ],
             ctx.converted[STAGE_HSD_RAW_TOBJ],
             ctx.converted[STAGE_HSD_RAW_VTX],
             ctx.converted[STAGE_HSD_RAW_IMAGE],
             ctx.converted[STAGE_HSD_RAW_TLUT],
             ctx.converted[STAGE_HSD_RAW_MTX],
             ctx.converted[STAGE_HSD_RAW_ENVELOPE],
             ctx.converted[STAGE_HSD_RAW_SPLINE]);

    free(ctx.seen);
    free(roots);
    mv_dat_close(&dat);
}

static int stage_normalize_count(void* value_ptr, u32 max, const char* what)
{
    uint32_t current;
    uint32_t swapped;

    memcpy(&current, value_ptr, sizeof(current));

    if (current <= max) {
        return 0;
    }

    swapped = __builtin_bswap32(current);
    if (swapped > max) {
        OSReport("VITA_STAGE_COUNT_INVALID field=%s raw=%08x swapped=%u max=%u\n",
                 what, current, swapped, max);
        HSD_Panic(__FILE__, __LINE__, "stage count outside sane range");
    }

    memcpy(value_ptr, &swapped, sizeof(swapped));
    return 1;
}

static void stage_normalize_ground_param(HSD_Archive* archive,
                                         GroundParam* param)
{
    uint32_t count;
    int raw;

    if (param == NULL) {
        return;
    }

    (void) stage_public_root_span(archive, "grGroundParam", param,
                                  sizeof(*param));
    count = (uint32_t) param->stage_param_count;
    raw = count > 0x100U;

    if (!raw) {
        return;
    }

    count = __builtin_bswap32(count);
    if (count == 0 || count > 0x100U) {
        OSReport("VITA_STAGE_PARAM_COUNT_INVALID raw=%08x swapped=%u\n",
                 (uint32_t) param->stage_param_count, count);
        HSD_Panic(__FILE__, __LINE__, "stage param count invalid");
    }

    stage_swap32(&param->y);
    stage_swap16(&param->x4);
    stage_swap16(&param->x8);
    stage_swap16(&param->xA);
    stage_swap32(&param->xC);
    stage_swap32(&param->x10);
    stage_swap32(&param->x14);
    stage_swap32(&param->x18);
    stage_swap32(&param->x1C);
    stage_swap32(&param->x20);
    stage_swap32(&param->x24);
    stage_swap32(&param->x28);
    stage_swap16(&param->x2E);
    stage_swap32(&param->x30);
    stage_swap32(&param->x34);
    stage_swap32(&param->x38);
    stage_swap32(&param->x3C);
    stage_swap32(&param->x40);
    stage_swap32(&param->x44);
    stage_swap32(&param->x48);
    stage_swap32(&param->x50);
    stage_swap32(&param->x54);
    stage_swap32(&param->x58);
    stage_swap32(&param->x5C);
    stage_swap32(&param->x60);
    stage_swap32(&param->x64);
    stage_swap16(&param->x68);
    for (unsigned i = 0; i < sizeof(param->x6A) / sizeof(param->x6A[0]); ++i) {
        stage_swap16(&param->x6A[i]);
    }
    param->stage_param_count = (s32) count;

    if (param->stage_params != NULL) {
        stage_require_range(archive, param->stage_params,
                            count * sizeof(*param->stage_params));
        for (uint32_t i = 0; i < count; ++i) {
            StageParam* entry = &param->stage_params[i];
            stage_swap32(&entry->stkind);
            stage_swap32(&entry->x4);
            stage_swap32(&entry->x8);
            stage_swap32(&entry->xC);
            stage_swap32(&entry->x10);
            stage_swap16(&entry->x14);
            stage_swap16(&entry->x16);
            stage_swap16(&entry->x18);
            for (unsigned j = 0;
                 j < sizeof(entry->x1A) / sizeof(entry->x1A[0]); ++j)
            {
                stage_swap16(&entry->x1A[j]);
            }
        }
    }
}

static int stage_normalize_s32_count(void* value_ptr, u32 max,
                                     const char* what)
{
    uint32_t current;
    uint32_t swapped;

    memcpy(&current, value_ptr, sizeof(current));
    if (current <= max) {
        return 0;
    }

    swapped = __builtin_bswap32(current);
    if (swapped > max) {
        OSReport("VITA_COLL_COUNT_INVALID field=%s raw=%08x swapped=%u max=%u\n",
                 what, current, swapped, max);
        HSD_Panic(__FILE__, __LINE__, "collision count outside sane range");
    }
    memcpy(value_ptr, &swapped, sizeof(swapped));
    return 1;
}

static void stage_validate_line_span(s16 start, s16 count, s32 line_count,
                                     const char* what)
{
    if (count < 0 ||
        (count > 0 && (start < 0 || start > line_count - count))) {
        OSReport("VITA_COLL_SPAN_INVALID field=%s start=%d count=%d lines=%d\n",
                 what, start, count, line_count);
        HSD_Panic(__FILE__, __LINE__, "collision line span invalid");
    }
}

static void stage_normalize_collision(HSD_Archive* archive, MapCollData* coll)
{
    int raw = 0;

    if (coll == NULL) {
        return;
    }
    /* The retail stage DAT payload ends after joint_count.  MapCollData::x2C
     * is inferred by the decomp and, in real stage archives, may alias the
     * next public symbol (map_ptcl).  Deriving the root span from the DAT
     * public table keeps schema conversion inside the serialized root. */
    (void) stage_public_root_span(archive, "coll_data", coll,
                                  offsetof(MapCollData, x2C));

    raw |= stage_normalize_s32_count(&coll->vert_count, 2048, "vert_count");
    raw |= stage_normalize_s32_count(&coll->line_count, 1536, "line_count");
    raw |= stage_normalize_s32_count(&coll->joint_count, 256, "joint_count");

    if (!raw) {
        return;
    }

    stage_swap16(&coll->floor_start);
    stage_swap16(&coll->floor_count);
    stage_swap16(&coll->ceiling_start);
    stage_swap16(&coll->ceiling_count);
    stage_swap16(&coll->right_wall_start);
    stage_swap16(&coll->right_wall_count);
    stage_swap16(&coll->left_wall_start);
    stage_swap16(&coll->left_wall_count);
    stage_swap16(&coll->dynamic_start);
    stage_swap16(&coll->dynamic_count);

    stage_validate_line_span(coll->floor_start, coll->floor_count,
                             coll->line_count, "floor");
    stage_validate_line_span(coll->ceiling_start, coll->ceiling_count,
                             coll->line_count, "ceiling");
    stage_validate_line_span(coll->right_wall_start, coll->right_wall_count,
                             coll->line_count, "right_wall");
    stage_validate_line_span(coll->left_wall_start, coll->left_wall_count,
                             coll->line_count, "left_wall");
    stage_validate_line_span(coll->dynamic_start, coll->dynamic_count,
                             coll->line_count, "dynamic");

    if (coll->verts != NULL && coll->vert_count > 0) {
        stage_require_range(archive, coll->verts,
                            (size_t) coll->vert_count * sizeof(*coll->verts));
        for (s32 i = 0; i < coll->vert_count; ++i) {
            stage_swap32(&coll->verts[i].x);
            stage_swap32(&coll->verts[i].y);
        }
    }

    if (coll->lines != NULL && coll->line_count > 0) {
        stage_require_range(archive, coll->lines,
                            (size_t) coll->line_count * sizeof(*coll->lines));
        for (s32 i = 0; i < coll->line_count; ++i) {
            MapLine* line = &coll->lines[i];
            stage_swap16(&line->v0_idx);
            stage_swap16(&line->v1_idx);
            stage_swap16(&line->prev_id0);
            stage_swap16(&line->next_id0);
            stage_swap16(&line->prev_id1);
            stage_swap16(&line->next_id1);
            stage_swap16(&line->hi_flags);
            stage_swap16(&line->lo_flags);
            if (line->v0_idx >= coll->vert_count ||
                line->v1_idx >= coll->vert_count) {
                OSReport("VITA_COLL_LINE_INVALID index=%d v0=%u v1=%u verts=%d\n",
                         i, line->v0_idx, line->v1_idx, coll->vert_count);
                HSD_Panic(__FILE__, __LINE__, "collision vertex index invalid");
            }
        }
    }

    if (coll->joints != NULL && coll->joint_count > 0) {
        stage_require_range(archive, coll->joints,
                            (size_t) coll->joint_count * sizeof(*coll->joints));
        for (s32 i = 0; i < coll->joint_count; ++i) {
            MapJoint* joint = &coll->joints[i];
            stage_swap16(&joint->floor_start);
            stage_swap16(&joint->floor_count);
            stage_swap16(&joint->ceiling_start);
            stage_swap16(&joint->ceiling_count);
            stage_swap16(&joint->right_wall_start);
            stage_swap16(&joint->right_wall_count);
            stage_swap16(&joint->left_wall_start);
            stage_swap16(&joint->left_wall_count);
            stage_swap16(&joint->dynamic_start);
            stage_swap16(&joint->dynamic_count);
            stage_swap32(&joint->left_bound);
            stage_swap32(&joint->bottom_bound);
            stage_swap32(&joint->right_bound);
            stage_swap32(&joint->top_bound);
            stage_swap16(&joint->vtx_start);
            stage_swap16(&joint->vtx_count);

            stage_validate_line_span(joint->floor_start, joint->floor_count,
                                     coll->line_count, "joint.floor");
            stage_validate_line_span(joint->ceiling_start,
                                     joint->ceiling_count, coll->line_count,
                                     "joint.ceiling");
            stage_validate_line_span(joint->right_wall_start,
                                     joint->right_wall_count, coll->line_count,
                                     "joint.right_wall");
            stage_validate_line_span(joint->left_wall_start,
                                     joint->left_wall_count, coll->line_count,
                                     "joint.left_wall");
            stage_validate_line_span(joint->dynamic_start,
                                     joint->dynamic_count, coll->line_count,
                                     "joint.dynamic");
            if (joint->vtx_count < 0 ||
                (joint->vtx_count > 0 &&
                 (joint->vtx_start < 0 ||
                  joint->vtx_start > coll->vert_count - joint->vtx_count))) {
                OSReport("VITA_COLL_JOINT_VTX_INVALID index=%d start=%d count=%d verts=%d\n",
                         i, joint->vtx_start, joint->vtx_count,
                         coll->vert_count);
                HSD_Panic(__FILE__, __LINE__,
                          "collision joint vertex span invalid");
            }
        }
    }

    OSReport("VITA_COLL_NATIVE_PASS coll=%p verts=%d lines=%d joints=%d floor=%d+%d ceil=%d+%d rw=%d+%d lw=%d+%d dyn=%d+%d\n",
             coll, coll->vert_count, coll->line_count, coll->joint_count,
             coll->floor_start, coll->floor_count, coll->ceiling_start,
             coll->ceiling_count, coll->right_wall_start,
             coll->right_wall_count, coll->left_wall_start,
             coll->left_wall_count, coll->dynamic_start,
             coll->dynamic_count);
}

static void stage_normalize_itemdata(HSD_Archive* archive,
                                     struct GroundItemData** itemdata)
{
    size_t root_span;
    size_t max_entries;

    if (itemdata == NULL) {
        return;
    }

    root_span = stage_public_root_span(archive, "itemdata", itemdata,
                                      sizeof(*itemdata));
    max_entries = root_span / sizeof(*itemdata);
    if (max_entries > 64) {
        max_entries = 64;
    }

    for (size_t i = 0; i < max_entries; ++i) {
        struct GroundItemData* entry = itemdata[i];
        uint32_t current;
        uint32_t swapped;

        if (entry == NULL) {
            OSReport("VITA_STAGE_ITEM_NATIVE_PASS items=%u\n", (unsigned) i);
            return;
        }

        stage_require_range(archive, entry, sizeof(*entry));
        memcpy(&current, &entry->unk0, sizeof(current));
        if ((s32) current >= It_Kind_Old_Kuri &&
            (s32) current <= It_Kind_Kyasarin_Egg)
        {
            continue;
        }

        swapped = __builtin_bswap32(current);
        if ((s32) swapped < It_Kind_Old_Kuri ||
            (s32) swapped > It_Kind_Kyasarin_Egg)
        {
            OSReport("VITA_STAGE_ITEM_KIND_INVALID index=%u raw=%08x swapped=%08x\n",
                     (unsigned) i, current, swapped);
            HSD_Panic(__FILE__, __LINE__, "stage item kind invalid");
        }

        memcpy(&entry->unk0, &swapped, sizeof(swapped));
        OSReport("VITA_STAGE_ITEM_KIND_NATIVE index=%u kind=%u article=%p\n",
                 (unsigned) i, swapped, entry->unk4);
    }

    OSReport("VITA_STAGE_ITEM_TABLE_INVALID reason=no_terminator max=%u\n",
             (unsigned) max_entries);
    HSD_Panic(__FILE__, __LINE__, "stage item table not terminated");
}

static int stage_normalize_joint_map(HSD_Archive* archive, UnkStageDat* map_head)
{
    StageJointMapEntry* entries;
    u32 total_pairs = 0;
    u32 converted_entries = 0;
    int raw;

    raw = stage_normalize_count(&map_head->unk4, 0x400, "map_head.unk4");
    if (map_head->unk4 == 0) {
        OSReport("VITA_STAGE_JOINT_MAP_NATIVE_PASS entries=0 pairs=0 converted=0\n");
        return raw;
    }
    if (map_head->unk0 == NULL) {
        HSD_Panic(__FILE__, __LINE__, "stage joint map table is null");
    }

    entries = map_head->unk0;
    stage_require_range(archive, entries,
                        (size_t) map_head->unk4 * sizeof(*entries));

    for (s32 i = 0; i < map_head->unk4; ++i) {
        StageJointMapEntry* entry = &entries[i];
        int entry_raw = stage_normalize_count(&entry->pair_count, 0x1000,
                                              "map_joint_map.pair_count");

        if (entry->joint == NULL) {
            OSReport("VITA_STAGE_JOINT_MAP_INVALID entry=%d reason=joint_null\n", i);
            HSD_Panic(__FILE__, __LINE__, "stage joint map joint is null");
        }
        stage_require_range(archive, entry->joint, sizeof(u32));

        if (entry->pair_count == 0) {
            continue;
        }
        if (entry->pairs == NULL) {
            OSReport("VITA_STAGE_JOINT_MAP_INVALID entry=%d reason=pairs_null count=%d\n",
                     i, entry->pair_count);
            HSD_Panic(__FILE__, __LINE__, "stage joint map pairs are null");
        }
        if ((u32) entry->pair_count > UINT32_MAX - total_pairs) {
            HSD_Panic(__FILE__, __LINE__, "stage joint map pair count overflow");
        }
        total_pairs += (u32) entry->pair_count;
        stage_require_range(archive, entry->pairs,
                            (size_t) entry->pair_count * 2 * sizeof(s16));

        if (entry_raw) {
            ++converted_entries;
            for (s32 j = 0; j < entry->pair_count; ++j) {
                stage_swap16(&entry->pairs[j * 2]);
                stage_swap16(&entry->pairs[j * 2 + 1]);
            }
        }

        for (s32 j = 0; j < entry->pair_count; ++j) {
            s16 target = entry->pairs[j * 2];
            s16 slot = entry->pairs[j * 2 + 1];
            if (target < 0 || target > 0x4000 || slot < 0 || slot >= 261) {
                OSReport("VITA_STAGE_JOINT_MAP_INVALID entry=%d pair=%d target=%d slot=%d\n",
                         i, j, target, slot);
                HSD_Panic(__FILE__, __LINE__, "stage joint map pair invalid");
            }
        }
    }

    OSReport("VITA_STAGE_JOINT_MAP_NATIVE_PASS entries=%d pairs=%u converted=%u raw=%d\n",
             map_head->unk4, total_pairs, converted_entries, raw);
    return raw || converted_entries != 0;
}

static void stage_normalize_shadows(HSD_Archive* archive, UnkStageDat* map_head)
{
    u32 enabled = 0;
    u32 converted = 0;

    if (map_head->unk24 == 0) {
        OSReport("VITA_STAGE_SHADOW_NATIVE_PASS entries=0 enabled=0 converted=0\n");
        return;
    }
    if (map_head->unk20 == NULL) {
        HSD_Panic(__FILE__, __LINE__, "stage shadow table is null");
    }

    stage_require_range(archive, map_head->unk20,
                        (size_t) map_head->unk24 * sizeof(*map_head->unk20));
    for (s32 i = 0; i < map_head->unk24; ++i) {
        struct GroundShadowEntry* entry = &map_head->unk20[i];
        /* PowerPC allocates the one-bit field from the MSB of the byte while
         * ARM allocates it from the LSB. Pointer relocation cannot fix that. */
        u8* flag_byte = (u8*) entry + sizeof(entry->unk0);
        u8 value = *flag_byte;

        if (value == 0x80) {
            *flag_byte = 0x01;
            value = 0x01;
            ++converted;
        } else if (value != 0x00 && value != 0x01) {
            OSReport("VITA_STAGE_SHADOW_INVALID index=%d raw=%02x anim=%p\n",
                     i, value, entry->unk0);
            HSD_Panic(__FILE__, __LINE__, "stage shadow flag invalid");
        }
        if (value != 0) {
            ++enabled;
        }
    }

    OSReport("VITA_STAGE_SHADOW_NATIVE_PASS entries=%d enabled=%u converted=%u\n",
             map_head->unk24, enabled, converted);
}

void mv_stage_archive_prepare(HSD_Archive* archive, UnkStageDat* map_head,
                              GroundParam* param, MapCollData* coll_data,
                              struct GroundItemData** itemdata)
{
    int raw = 0;

    if (archive == NULL || map_head == NULL) {
        return;
    }

    (void) stage_public_root_span(archive, "map_head", map_head,
                                  sizeof(*map_head));

    raw |= stage_normalize_joint_map(archive, map_head);
    raw |= stage_normalize_count(&map_head->unkC, 0x400, "map_head.unkC");
    raw |= stage_normalize_count(&map_head->unk14, 0x400, "map_head.unk14");
    raw |= stage_normalize_count(&map_head->unk1C, 0x400, "map_head.unk1C");
    raw |= stage_normalize_count(&map_head->unk24, 0x400, "map_head.unk24");
    raw |= stage_normalize_count(&map_head->unk2C, 0x400, "map_head.unk2C");
    stage_normalize_shadows(archive, map_head);

    if (map_head->unk8 != NULL && map_head->unkC > 0) {
        stage_require_range(archive, map_head->unk8,
                            (size_t) map_head->unkC * sizeof(*map_head->unk8));
        for (s32 i = 0; i < map_head->unkC; ++i) {
            struct UnkStageDat_x8_t* entry = &map_head->unk8[i];
            int joints_raw =
                stage_normalize_count(&entry->unk24, 0x400, "map_entry.unk24");
            stage_normalize_count(&entry->x30, 0x4000, "map_entry.x30");

            if (entry->unk20 != NULL && entry->unk24 > 0) {
                stage_require_range(archive, entry->unk20,
                                    (size_t) entry->unk24 * sizeof(*entry->unk20));
                if (raw || joints_raw) {
                    for (s32 j = 0; j < entry->unk24; ++j) {
                        stage_swap16(&entry->unk20[j].x);
                        stage_swap16(&entry->unk20[j].y);
                        stage_swap16(&entry->unk20[j].z);
                    }
                }
            }
        }
    }

    if (map_head->unk28 != NULL && map_head->unk2C > 0) {
        stage_require_range(archive, map_head->unk28,
                            (size_t) map_head->unk2C * sizeof(*map_head->unk28));
        if (raw) {
            for (s32 i = 0; i < map_head->unk2C; ++i) {
                UnkStageDatInternal* entry = map_head->unk28[i];
                if (entry != NULL) {
                    stage_require_range(archive, entry, sizeof(*entry));
                    stage_swap32(&entry->unk4);
                }
            }
        }
    }

    stage_normalize_ground_param(archive, param);
    stage_normalize_collision(archive, coll_data);
    stage_normalize_itemdata(archive, itemdata);

    OSReport("VITA_STAGE_NATIVE_PASS map_head=%p maps=%d splines=%d shadows=%d "
             "internals=%d stage_params=%d raw=%d\n",
             map_head, map_head->unkC, map_head->unk14, map_head->unk24,
             map_head->unk2C, param != NULL ? param->stage_param_count : 0, raw);
}
