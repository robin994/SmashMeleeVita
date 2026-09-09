#include "hsd_runtime_probe.h"

#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/id.h>
#include <sysdolphin/baselib/initialize.h>
#include <dolphin/os.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/tobj.h>
#include "gx_capture_vita.h"

#include <string.h>
#include <stddef.h>
#include <math.h>

_Static_assert(sizeof(HSD_TObjDesc) == 92, "upstream HSD_TObjDesc ARM32 size");
_Static_assert(offsetof(HSD_TObjDesc, next) == 4, "HSD_TObjDesc.next ABI");
_Static_assert(offsetof(HSD_TObjDesc, scale) == 28, "HSD_TObjDesc.scale ABI");
_Static_assert(offsetof(HSD_TObjDesc, blend_flags) == 64, "HSD_TObjDesc.blend_flags ABI");
_Static_assert(offsetof(HSD_TObjDesc, blending) == 68, "HSD_TObjDesc.blending ABI");
_Static_assert(offsetof(HSD_TObjDesc, imagedesc) == 76, "HSD_TObjDesc.imagedesc ABI");
_Static_assert(offsetof(HSD_TObjDesc, tlutdesc) == 80, "HSD_TObjDesc.tlutdesc ABI");
_Static_assert(offsetof(HSD_TObjDesc, lod) == 84, "HSD_TObjDesc.lod ABI");
_Static_assert(offsetof(HSD_TObjDesc, tev) == 88, "HSD_TObjDesc.tev ABI");

static void count_dobjs(HSD_DObj *dobj, MvHsdRuntimeStats *stats)
{
    for (; dobj; dobj = dobj->next) {
        ++stats->dobjs;
        if (dobj->mobj) {
            ++stats->mobjs;
            for (HSD_TObj *tobj = dobj->mobj->tobj; tobj; tobj = tobj->next)
                ++stats->tobjs;
        }
        for (HSD_PObj *pobj = dobj->pobj; pobj; pobj = pobj->next)
            ++stats->pobjs;
    }
}

static void count_joints(HSD_JObj *jobj, MvHsdRuntimeStats *stats)
{
    for (; jobj; jobj = jobj->next) {
        ++stats->joints;
        if (union_type_dobj(jobj)) count_dobjs(jobj->u.dobj, stats);
        if (!(jobj->flags & JOBJ_INSTANCE)) count_joints(jobj->child, stats);
    }
}

static void capture_identity(float m[12])
{
    memset(m, 0, sizeof(float) * 12u);
    m[0] = m[5] = m[10] = 1.0f;
}

static void capture_concat(const float a[12], const float b[12], float out[12])
{
    float result[12];
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 3; ++col) {
            result[row * 4 + col] = a[row * 4] * b[col] +
                                    a[row * 4 + 1] * b[4 + col] +
                                    a[row * 4 + 2] * b[8 + col];
        }
        result[row * 4 + 3] = a[row * 4] * b[3] +
                              a[row * 4 + 1] * b[7] +
                              a[row * 4 + 2] * b[11] + a[row * 4 + 3];
    }
    memcpy(out, result, sizeof(result));
}

/* Rigid Euler subset of upstream HSD_MtxSRT. The native graph builder rejects
 * quaternion, IK, billboards, user matrices and RObj before this is reachable. */
static int capture_make_srt(float m[12], const HSD_JObj *jobj, const float *parent_scl)
{
    float sx0 = jobj->scale.x, sy0 = jobj->scale.y, sz0 = jobj->scale.z;
    float sx1 = jobj->scale.x, sy1 = jobj->scale.y, sz1 = jobj->scale.z;
    float sx2 = jobj->scale.x, sy2 = jobj->scale.y, sz2 = jobj->scale.z;
    if (!isfinite(sx0) || !isfinite(sy0) || !isfinite(sz0) ||
        !isfinite(jobj->rotate.x) || !isfinite(jobj->rotate.y) ||
        !isfinite(jobj->rotate.z) || !isfinite(jobj->translate.x) ||
        !isfinite(jobj->translate.y) || !isfinite(jobj->translate.z)) return -1;
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
    const float sin_x = sinf(jobj->rotate.x), cos_x = cosf(jobj->rotate.x);
    const float sin_y = sinf(jobj->rotate.y), cos_y = cosf(jobj->rotate.y);
    const float sin_z = sinf(jobj->rotate.z), cos_z = cosf(jobj->rotate.z);
    m[0] = cos_z * (sx2 * cos_y);
    m[4] = sin_z * (sx1 * cos_y);
    m[8] = -sx0 * sin_y;
    m[1] = sy2 * ((cos_z * (sin_x * sin_y)) - (cos_x * sin_z));
    m[5] = sy1 * ((sin_z * (sin_x * sin_y)) + (cos_x * cos_z));
    m[9] = cos_y * (sy0 * sin_x);
    m[2] = sz2 * ((cos_z * (cos_x * sin_y)) + (sin_x * sin_z));
    m[6] = sz1 * ((sin_z * (cos_x * sin_y)) - (sin_x * cos_z));
    m[10] = cos_y * (sz0 * cos_x);
    m[3] = jobj->translate.x;
    m[7] = jobj->translate.y;
    m[11] = jobj->translate.z;
    return 0;
}

static uint32_t capture_material_rgba(const HSD_Material *mat)
{
    if (!mat) return 0xffffffffu;
    float alpha = mat->alpha;
    if (!isfinite(alpha)) alpha = 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    unsigned a = (unsigned)((float)mat->diffuse.a * alpha + 0.5f);
    return (uint32_t)mat->diffuse.r << 24 | (uint32_t)mat->diffuse.g << 16 |
           (uint32_t)mat->diffuse.b << 8 | a;
}

static void capture_rotation_mtx(float m[12], float rx, float ry, float rz)
{
    const float sx = sinf(rx), cx = cosf(rx);
    const float sy = sinf(ry), cy = cosf(ry);
    const float sz = sinf(rz), cz = cosf(rz);
    const float t1 = sx * sy, t2 = cx * sy;
    m[0] = cy * cz; m[4] = cy * sz; m[8] = -sy;
    m[1] = cz * t1 - cx * sz; m[5] = sz * t1 + cx * cz; m[9] = sx * cy;
    m[2] = cz * t2 + sx * sz; m[6] = sz * t2 - sx * cz; m[10] = cx * cy;
    m[3] = m[7] = m[11] = 0.0f;
}

static int capture_uv_mtx(const HSD_TObj *tobj, float out[2][3])
{
    if (!tobj || tobj_coord(tobj) != TEX_COORD_UV || !tobj->repeat_s || !tobj->repeat_t ||
        !isfinite(tobj->scale.x) || !isfinite(tobj->scale.y) ||
        fabsf(tobj->scale.x) < 1.0e-12f || fabsf(tobj->scale.y) < 1.0e-12f)
        return -1;
    float trans_y_extra = 0.0f;
    if (tobj->wrap_t == GX_MIRROR) {
        float repeat_over_scale = (float)tobj->repeat_t / tobj->scale.y;
        if (fabsf(repeat_over_scale) < 1.0e-12f) return -1;
        trans_y_extra = 1.0f / repeat_over_scale;
    }
    float translation[12] = {
        1, 0, 0, -tobj->translate.x,
        0, 1, 0, -(tobj->translate.y + trans_y_extra),
        0, 0, 1, tobj->translate.z
    };
    float rotation[12], rt[12], scale[12] = {0}, m[12];
    capture_rotation_mtx(rotation, tobj->rotate.x, tobj->rotate.y, -tobj->rotate.z);
    capture_concat(rotation, translation, rt);
    scale[0] = (float)tobj->repeat_s / tobj->scale.x;
    scale[5] = (float)tobj->repeat_t / tobj->scale.y;
    scale[10] = tobj->scale.z;
    capture_concat(scale, rt, m);
    out[0][0] = m[0]; out[0][1] = m[1]; out[0][2] = m[3];
    out[1][0] = m[4]; out[1][1] = m[5]; out[1][2] = m[7];
    return 0;
}

static void capture_material_state(const HSD_DObj *dobj, MvGxMaterialState *out)
{
    memset(out, 0, sizeof(*out));
    out->material_rgba = 0xffffffffu;
    if (!dobj || !dobj->mobj) return;

    const HSD_MObj *mobj = dobj->mobj;
    out->material_rgba = capture_material_rgba(mobj->mat);
    out->rendermode = mobj->rendermode;
    if (mobj->pe) {
        const HSD_PEDesc *pe = mobj->pe;
        out->pe_color_update = !!(pe->flags & 0x01);
        out->pe_alpha_update = !!(pe->flags & 0x02);
        out->pe_dst_alpha_enable = !!(pe->flags & 0x04);
        out->pe_dst_alpha = pe->dst_alpha;
        out->pe_blend_type = pe->type;
        out->pe_src_factor = pe->src_factor;
        out->pe_dst_factor = pe->dst_factor;
        out->pe_logic_op = pe->logic_op;
        out->pe_z_enable = !!(pe->flags & 0x10);
        out->pe_z_func = pe->z_comp;
        out->pe_z_update = !!(pe->flags & 0x20);
        out->pe_z_comp_loc = !!(pe->flags & 0x08);
        out->pe_alpha_comp0 = pe->alpha_comp0;
        out->pe_alpha_ref0 = pe->ref0;
        out->pe_alpha_op = pe->alpha_op;
        out->pe_alpha_comp1 = pe->alpha_comp1;
        out->pe_alpha_ref1 = pe->ref1;
        out->pe_dither = !!(pe->flags & 0x40);
        out->pe_custom = 1;
    } else {
        /* Exact default branch of HSD_SetupPEMode(). */
        out->pe_color_update = 1;
        out->pe_alpha_update = 0;
        out->pe_dst_alpha_enable = 0;
        out->pe_dst_alpha = 0;
        out->pe_blend_type = (mobj->rendermode & 0x40000000u) ? GX_BM_BLEND : GX_BM_NONE;
        out->pe_src_factor = GX_BL_SRCALPHA;
        out->pe_dst_factor = GX_BL_INVSRCALPHA;
        out->pe_logic_op = GX_LO_SET;
        out->pe_z_enable = 1;
        out->pe_z_func = (mobj->rendermode & 0x08000000u) ? GX_ALWAYS : GX_LEQUAL;
        out->pe_z_update = (mobj->rendermode & 0x20000000u) ? 0 : 1;
        if (out->pe_z_update && out->pe_blend_type == GX_BM_BLEND) {
            out->pe_z_comp_loc = 0;
            out->pe_alpha_comp0 = GX_GEQUAL;
            out->pe_alpha_ref0 = 0;
            out->pe_alpha_op = GX_AOP_AND;
            out->pe_alpha_comp1 = GX_GEQUAL;
            out->pe_alpha_ref1 = 0;
        } else {
            out->pe_z_comp_loc = 1;
            out->pe_alpha_comp0 = GX_ALWAYS;
            out->pe_alpha_ref0 = 0;
            out->pe_alpha_op = GX_AOP_AND;
            out->pe_alpha_comp1 = GX_ALWAYS;
            out->pe_alpha_ref1 = 0;
        }
        out->pe_dither = 0;
        out->pe_custom = 0;
    }
    const HSD_TObj *first = mobj->tobj;
    for (const HSD_TObj *tobj = first; tobj; tobj = tobj->next) {
        if (out->texture_count != UINT8_MAX) ++out->texture_count;
    }
    if (!first) return;
    if (out->texture_count != 1) out->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_MULTITEX;

    out->tobj_flags = first->flags & ~TEX_MTX_DIRTY;
    out->blending = first->blending;
    out->wrap_s = (uint8_t)first->wrap_s;
    out->wrap_t = (uint8_t)first->wrap_t;
    out->mag_filter = (uint8_t)first->magFilt;
    if (tobj_coord(first) != TEX_COORD_UV)
        out->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_TEXCOORD;
    if (tobj_bump(first)) out->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_BUMP;
    if (first->tev) {
        out->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV;
        out->tev_valid = 1;
        memcpy(out->tev_op, &first->tev->color_op, sizeof(out->tev_op));
        memcpy(out->tev_konst, &first->tev->konst, sizeof(out->tev_konst));
        memcpy(out->tev0, &first->tev->tev0, sizeof(out->tev0));
        memcpy(out->tev1, &first->tev->tev1, sizeof(out->tev1));
        out->tev_active = first->tev->active;
    }

    const uint32_t colormap = tobj_colormap(first);
    if (colormap != TEX_COLORMAP_MODULATE && colormap != TEX_COLORMAP_REPLACE)
        out->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_COLORMAP;
    const uint32_t alphamap = tobj_alphamap(first);
    if (alphamap != TEX_ALPHAMAP_NONE && alphamap != TEX_ALPHAMAP_MODULATE &&
        alphamap != TEX_ALPHAMAP_REPLACE)
        out->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_ALPHAMAP;

    if (first->imagedesc) {
        out->image = first->imagedesc->image_ptr;
        out->width = first->imagedesc->width;
        out->height = first->imagedesc->height;
        out->format = (uint8_t)first->imagedesc->format;
    }
    if (first->tlut) {
        out->palette = first->tlut->lut;
        out->palette_entries = first->tlut->n_entries;
        out->palette_format = (uint8_t)first->tlut->fmt;
    }
    if (!(out->unsupported & MV_GX_MATERIAL_UNSUPPORTED_TEXCOORD) &&
        !capture_uv_mtx(first, out->uv_mtx))
        out->uv_mtx_valid = 1;

    const HSD_TObj *second = first->next;
    if (second) {
        out->tobj1_flags = second->flags & ~TEX_MTX_DIRTY;
        out->blending1 = second->blending;
        out->wrap_s1 = (uint8_t)second->wrap_s;
        out->wrap_t1 = (uint8_t)second->wrap_t;
        out->mag_filter1 = (uint8_t)second->magFilt;
        if (second->imagedesc) {
            out->image1 = second->imagedesc->image_ptr;
            out->width1 = second->imagedesc->width;
            out->height1 = second->imagedesc->height;
            out->format1 = (uint8_t)second->imagedesc->format;
        }
        if (second->tlut) {
            out->palette1 = second->tlut->lut;
            out->palette_entries1 = second->tlut->n_entries;
            out->palette_format1 = (uint8_t)second->tlut->fmt;
        }
        if (tobj_coord(second) == TEX_COORD_UV &&
            !capture_uv_mtx(second, out->uv_mtx1))
            out->uv_mtx1_valid = 1;
        if (second->tev) {
            out->tev1_valid = 1;
            memcpy(out->tev1_op, &second->tev->color_op, sizeof(out->tev1_op));
            memcpy(out->tev1_konst, &second->tev->konst, sizeof(out->tev1_konst));
            memcpy(out->tev1_reg0, &second->tev->tev0, sizeof(out->tev1_reg0));
            memcpy(out->tev1_reg1, &second->tev->tev1, sizeof(out->tev1_reg1));
            out->tev1_active = second->tev->active;
        }
    }
}

static int capture_joints(HSD_JObj *jobj, const float parent[12],
                          const float parent_scl[3], int parent_has_scl)
{
    for (; jobj; jobj = jobj->next) {
        float local[12], world[12], current_scl[3] = {0};
        if (capture_make_srt(local, jobj, parent_has_scl ? parent_scl : NULL)) return -1;
        capture_concat(parent, local, world);
        int current_has_scl;
        if (jobj->flags & JOBJ_CLASSICAL_SCALE) {
            current_has_scl = parent_has_scl;
            if (parent_has_scl) memcpy(current_scl, parent_scl, sizeof(current_scl));
        } else {
            current_has_scl = 1;
            current_scl[0] = jobj->scale.x * (parent_has_scl ? parent_scl[0] : 1.0f);
            current_scl[1] = jobj->scale.y * (parent_has_scl ? parent_scl[1] : 1.0f);
            current_scl[2] = jobj->scale.z * (parent_has_scl ? parent_scl[2] : 1.0f);
        }
        if (union_type_dobj(jobj)) {
            for (HSD_DObj *dobj = jobj->u.dobj; dobj; dobj = dobj->next) {
                MvGxMaterialState material;
                capture_material_state(dobj, &material);
                mv_gx_capture_set_material(&material);
                for (HSD_PObj *pobj = dobj->pobj; pobj; pobj = pobj->next) {
                    if (HSD_PObjCaptureRigid(pobj, (MtxPtr)world))
                        return -1;
                }
            }
        }
        if (!(jobj->flags & JOBJ_INSTANCE) &&
            capture_joints(jobj->child, world, current_scl, current_has_scl))
            return -1;
    }
    return 0;
}

int mv_hsd_runtime_probe(const MvNativeHsd *native, MvHsdRuntimeStats *stats)
{
    if (!native || !native->root || !stats || native->unsupported_count) return -1;
    memset(stats, 0, sizeof(*stats));
    HSD_IDInitAllocData();
    HSD_IDSetup();
    long heap_before = OSCheckHeap(HSD_GetHeap());
    if (heap_before <= 0) return -1;
    HSD_JObj *root = HSD_JObjLoadJoint(native->root);
    long heap_after = OSCheckHeap(HSD_GetHeap());
    if (heap_after < 0 || heap_after >= heap_before) return -1;
    OSReport("HSD_RUNTIME_HEAP_PASS backend=upstream_memory_objalloc before=%ld after=%ld used=%ld\n",
             heap_before, heap_after, heap_before - heap_after);
    if (!root) return -1;
    count_joints(root, stats);
    if (stats->joints != native->joint_count || stats->dobjs != native->dobj_count ||
        stats->mobjs != native->mobj_count || stats->pobjs != native->pobj_count ||
        stats->tobjs != native->tobj_count)
        return -1;
    return 0;
}

int mv_hsd_gx_capture_probe(const MvNativeHsd *native, MvGxCaptureStats *capture)
{
    if (!native || !native->root || !capture || native->unsupported_count) return -1;
    HSD_IDInitAllocData();
    HSD_IDSetup();
    HSD_JObj *root = HSD_JObjLoadJoint(native->root);
    if (!root) return -2;
    mv_gx_capture_reset();
    HSD_ClearVtxDesc();
    float identity[12];
    capture_identity(identity);
    if (capture_joints(root, identity, NULL, 0)) return -3;
    if (mv_gx_capture_stats(capture)) return -3;
    if (capture->display_lists != native->pobj_count || !capture->commands ||
        !capture->vertices || !capture->triangles) return -4;
    return 0;
}

int mv_hsd_archive_probe(void *bytes, size_t size, const char *public_name,
                         MvHsdArchiveStats *stats)
{
    if (!bytes || !public_name || !stats) return -1;
    memset(stats, 0, sizeof(*stats));
    HSD_Archive archive;
    if (HSD_ArchiveParse(&archive, bytes, size) != 0) return -1;
    void *address = HSD_ArchiveGetPublicAddress(&archive, public_name);
    if (!address || address < (void *)archive.data) return -1;
    uintptr_t offset = (uintptr_t)address - (uintptr_t)archive.data;
    if (offset > archive.header.data_size) return -1;
    stats->file_size = archive.header.file_size;
    stats->data_size = archive.header.data_size;
    stats->relocations = archive.header.nb_reloc;
    stats->publics = archive.header.nb_public;
    stats->externs = archive.header.nb_extern;
    stats->public_offset = (uint32_t)offset;
    return 0;
}
