#pragma once

#include "hsd_native.h"
#include "gx_capture_vita.h"
#include <stdint.h>

typedef struct {
    uint32_t joints;
    uint32_t dobjs;
    uint32_t mobjs;
    uint32_t pobjs;
    uint32_t tobjs;
} MvHsdRuntimeStats;

typedef struct {
    uint32_t file_size;
    uint32_t data_size;
    uint32_t relocations;
    uint32_t publics;
    uint32_t externs;
    uint32_t public_offset;
} MvHsdArchiveStats;

/* Bootstrap through the upstream HSD_JObjLoadJoint implementation. */
int mv_hsd_runtime_probe(const MvNativeHsd *native, MvHsdRuntimeStats *stats);

/* Construct a fresh upstream object graph, then execute the original rigid
 * PObj GX display path into the bounded Vita command capture backend.  This is
 * deliberately geometry/state capture only: material/TEV submission is a
 * later renderer stage. */
int mv_hsd_gx_capture_probe(const MvNativeHsd *native, MvGxCaptureStats *stats);
/* Append one native HSD model graph to the Vita GX capture queue. */
int mv_hsd_gx_capture_append(const MvNativeHsd *native, int reset, MvGxCaptureStats *stats);

/* Parse a writable copy of an original HSD archive with the upstream archive
 * API adapted for the GameCube big-endian file format on ARM.  Pointer fields
 * are relocated in place just like the original parser; non-pointer descriptor
 * scalars intentionally remain big-endian and still require typed conversion
 * before feeding an HSD object constructor. */
int mv_hsd_archive_probe(void *bytes, size_t size, const char *public_name,
                         MvHsdArchiveStats *stats);

struct HSD_JObj;
int mv_hsd_gx_capture_runtime(struct HSD_JObj *root, int reset, int visibility, MvGxCaptureStats *capture);
