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
} MvNativeHsd;

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
int mv_hsd_native_build(const MvDat *dat, const char *root_name, MvNativeHsd *out);
void mv_hsd_native_free(MvNativeHsd *graph);
int mv_hsd_native_stats(const MvDat *dat, const char *root_name,
                        uint32_t stats[MV_NATIVE_STAT_COUNT]);
