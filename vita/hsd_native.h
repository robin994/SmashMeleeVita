#pragma once

#include "hsd_data.h"
#include <stddef.h>
#include <stdint.h>

typedef struct HSD_Joint HSD_Joint;

/* Native-pointer descriptor graph suitable for the upstream HSD loaders.
   Scalar descriptor fields are converted to host endianness. GX payloads
   (display lists, indexed vertex arrays and tiled texture bytes) deliberately
   remain immutable references into the original archive because they are
   byte streams, not host scalar structures. */
typedef struct {
    HSD_Joint *root;
    void *storage;
    size_t joint_count;
    size_t dobj_count;
    size_t mobj_count;
    size_t pobj_count;
    size_t tobj_count;
    size_t vtxdesc_count;
    size_t image_count;
    size_t tlut_count;
    size_t material_count;
    size_t pedesc_count;
    size_t lod_count;
    size_t tev_count;
    size_t unsupported_count;
    /* Preserved even when mv_hsd_native_build() returns 1, so callers can
       report exactly which serialized HSD feature stopped the conversion. */
    uint32_t unsupported_kind;
    uint32_t unsupported_offset;
    uint32_t unsupported_value;
} MvNativeHsd;

enum {
    MV_NATIVE_UNSUPPORTED_NONE = 0,
    MV_NATIVE_UNSUPPORTED_MOBJ_RENDERDESC,
    MV_NATIVE_UNSUPPORTED_POBJ_TYPE,
    MV_NATIVE_UNSUPPORTED_POBJ_UNION,
    MV_NATIVE_UNSUPPORTED_JOBJ_FLAGS,
    MV_NATIVE_UNSUPPORTED_JOBJ_ROBJ,
};

const char *mv_hsd_native_unsupported_name(uint32_t kind);

enum {
    MV_NATIVE_STAT_JOINTS,
    MV_NATIVE_STAT_DOBJS,
    MV_NATIVE_STAT_MOBJS,
    MV_NATIVE_STAT_POBJS,
    MV_NATIVE_STAT_TOBJS,
    MV_NATIVE_STAT_VTXDESCS,
    MV_NATIVE_STAT_IMAGES,
    MV_NATIVE_STAT_TLUTS,
    MV_NATIVE_STAT_MATERIALS,
    MV_NATIVE_STAT_PEDESCS,
    MV_NATIVE_STAT_LODS,
    MV_NATIVE_STAT_TEVS,
    MV_NATIVE_STAT_UNSUPPORTED,
    MV_NATIVE_STAT_COUNT
};

/* Returns 0 on a complete supported graph, 1 when the selected root requires
   an explicitly unsupported HSD descriptor type, and -1 for malformed data or
   allocation failure. No necessary descriptor is silently replaced by NULL. */
int mv_hsd_native_build_at(const MvDat *dat, uint32_t root_offset, MvNativeHsd *out);
/* Validation graph for pre-relocation raw archives. It accepts HSD features
   whose serialized scalar layout is supported by the raw nativeizer (notably
   spline JObjs and the standard V/H/R billboard modes) even when the compact
   standalone native-proxy renderer does not model them. Never pass this graph
   to runtime loaders; callers must free it after validation/statistics. */
int mv_hsd_native_validate_raw_at(const MvDat *dat, uint32_t root_offset,
                                  MvNativeHsd *out);
int mv_hsd_native_build(const MvDat *dat, const char *root_name, MvNativeHsd *out);
void mv_hsd_native_free(MvNativeHsd *graph);
int mv_hsd_native_stats(const MvDat *dat, const char *root_name,
                        uint32_t stats[MV_NATIVE_STAT_COUNT]);
