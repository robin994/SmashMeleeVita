#include "sss_assets_vita.h"
#include "asset_file.h"
#include "hsd_anim_native.h"
#include "hsd_data.h"
#include "hsd_matanim_native.h"
#include "hsd_native.h"

#include <melee/sc/types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/wobj.h>

typedef struct {
    HSD_CObjDesc *cam;
    void *light0;
    void *light1;
    void *fog;
} MvSssSceneModels;

typedef struct {
    MvSssSceneModels models;
    StaticModelDesc anim[11];
    HSD_Joint *confirm_joint;
    HSD_AnimJoint *confirm_anim;
    HSD_MatAnimJoint *confirm_matanim;
    HSD_ShapeAnimJoint *confirm_shapeanim;
} MvSssDataTable;

_Static_assert(sizeof(StaticModelDesc) == 16, "SSS StaticModelDesc ABI");
_Static_assert(sizeof(MvSssDataTable) == 0xD0, "MnSelectStageDataTable ABI");

static unsigned char *sss_bytes;
static size_t sss_size;
static MvDat sss_dat;
static int sss_dat_open;
static int sss_ready;
static int sss_use_us;
static MvNativeHsd sss_models[12];
static MvNativeAnim sss_anims[12];
static MvNativeMatAnim sss_matanims[12];
static MvCamera sss_camera;
static HSD_WObjDesc sss_eye, sss_interest;
static Vec3 sss_up;
static HSD_CameraDescPerspective sss_camera_desc;
static MvSssDataTable sss_table;
static FILE *sss_log;

void mv_sss_vita_set_log(FILE *log) { sss_log = log; }

static int public_offset(const char *wanted, uint32_t *out)
{
    for (uint32_t i = 0; i < sss_dat.public_count; ++i) {
        const char *name;
        uint32_t offset;
        if (mv_dat_public(&sss_dat, i, &name, &offset)) return -1;
        if (!strcmp(name, wanted)) { *out = offset; return 0; }
    }
    return -1;
}

static int table_pointer(uint32_t field, uint32_t *target)
{
    int r = mv_dat_pointer(&sss_dat, field, target);
    return r < 0 ? -1 : r;
}

static int build_camera(uint32_t offset)
{
    if (mv_camera_read_at(&sss_dat, offset, &sss_camera)) return -1;
    if (sss_camera.projection_type != PROJ_PERSPECTIVE) return -2;
    memset(&sss_eye, 0, sizeof(sss_eye));
    memset(&sss_interest, 0, sizeof(sss_interest));
    memset(&sss_camera_desc, 0, sizeof(sss_camera_desc));
    sss_eye.pos.x = sss_camera.eye[0];
    sss_eye.pos.y = sss_camera.eye[1];
    sss_eye.pos.z = sss_camera.eye[2];
    sss_interest.pos.x = sss_camera.interest[0];
    sss_interest.pos.y = sss_camera.interest[1];
    sss_interest.pos.z = sss_camera.interest[2];
    sss_up.x = sss_camera.up[0];
    sss_up.y = sss_camera.up[1];
    sss_up.z = sss_camera.up[2];
    sss_camera_desc.class_name = NULL;
    sss_camera_desc.flags = sss_camera.flags;
    sss_camera_desc.projection_type = sss_camera.projection_type;
    sss_camera_desc.viewport.xmin = sss_camera.viewport_xmin;
    sss_camera_desc.viewport.xmax = sss_camera.viewport_xmax;
    sss_camera_desc.viewport.ymin = sss_camera.viewport_ymin;
    sss_camera_desc.viewport.ymax = sss_camera.viewport_ymax;
    sss_camera_desc.scissor.left = sss_camera.scissor_left;
    sss_camera_desc.scissor.right = sss_camera.scissor_right;
    sss_camera_desc.scissor.top = sss_camera.scissor_top;
    sss_camera_desc.scissor.bottom = sss_camera.scissor_bottom;
    sss_camera_desc.eyepos = &sss_eye;
    sss_camera_desc.interest = &sss_interest;
    sss_camera_desc.roll = 0.0f;
    sss_camera_desc.up_vector = &sss_up;
    sss_camera_desc.nnear = sss_camera.near_z;
    sss_camera_desc.ffar = sss_camera.far_z;
    sss_camera_desc.fov = sss_camera.fov_y;
    sss_camera_desc.aspect = sss_camera.aspect;
    sss_table.models.cam = (HSD_CObjDesc *)&sss_camera_desc;
    return 0;
}

void mv_sss_vita_release(void)
{
    for (unsigned i = 0; i < 12; ++i) {
        mv_native_matanim_free(&sss_matanims[i]);
        mv_native_anim_free(&sss_anims[i]);
        mv_hsd_native_free(&sss_models[i]);
    }
    if (sss_dat_open) { mv_dat_close(&sss_dat); sss_dat_open = 0; }
    free(sss_bytes);
    sss_bytes = NULL;
    sss_size = 0;
    sss_ready = 0;
    memset(&sss_table, 0, sizeof(sss_table));
}

static int build_set(unsigned index, uint32_t base, StaticModelDesc *out)
{
    uint32_t target = 0;
    int r = table_pointer(base + 0, &target);
    if (r < 0) return -1;
    if (r == 1) {
        int br = mv_hsd_native_build_at(&sss_dat, target, &sss_models[index]);
        if (br) {
            if (sss_log) {
                fprintf(sss_log,
                        "GAME_SSS_NATIVE_FAIL set=%u code=%d unsupported=%s offset=%08x value=%08x\n",
                        index, br, mv_hsd_native_unsupported_name(sss_models[index].unsupported_kind),
                        sss_models[index].unsupported_offset, sss_models[index].unsupported_value);
                fflush(sss_log);
            }
            return -2;
        }
        out->joint = sss_models[index].root;
    }
    r = table_pointer(base + 4, &target);
    if (r < 0) return -3;
    if (r == 1) {
        if (mv_native_anim_build_at(&sss_dat, target, &sss_anims[index])) return -4;
        out->animjoint = sss_anims[index].root;
    }
    r = table_pointer(base + 8, &target);
    if (r < 0) return -5;
    if (r == 1) {
        if (mv_native_matanim_build_at(&sss_dat, target, &sss_matanims[index])) return -6;
        out->matanim_joint = sss_matanims[index].root;
    }
    r = table_pointer(base + 12, &target);
    if (r != 0) return -7;
    out->shapeanim_joint = NULL;
    return 0;
}

int mv_sss_vita_prepare(int use_us, void **data_table)
{
    if (!data_table) return -1;
    use_us = !!use_us;
    if (sss_ready && sss_use_us != use_us) mv_sss_vita_release();
    if (!sss_ready) {
        const char *path = use_us ?
            "ux0:data/SmashMeleeVita/files/MnSlMap.usd" :
            "ux0:data/SmashMeleeVita/files/MnSlMap.dat";
        int r = mv_read_file(path, &sss_bytes, &sss_size);
        if (r) return -10;
        if (mv_dat_open(&sss_dat, sss_bytes, sss_size)) { mv_sss_vita_release(); return -11; }
        sss_dat_open = 1;
        uint32_t table = 0, target = 0;
        if (public_offset("MnSelectStageDataTable", &table)) { mv_sss_vita_release(); return -12; }
        r = table_pointer(table + 0, &target);
        if (r != 1 || build_camera(target)) { mv_sss_vita_release(); return -13; }
        sss_table.models.light0 = NULL;
        sss_table.models.light1 = NULL;
        sss_table.models.fog = NULL;
        for (unsigned i = 0; i < 11; ++i) {
            int br = build_set(i, table + 0x10 + i * 0x10, &sss_table.anim[i]);
            if (br) { mv_sss_vita_release(); return -20 - (int)i; }
        }
        StaticModelDesc confirm = {0};
        int br = build_set(11, table + 0xC0, &confirm);
        if (br) { mv_sss_vita_release(); return -40; }
        sss_table.confirm_joint = confirm.joint;
        sss_table.confirm_anim = confirm.animjoint;
        sss_table.confirm_matanim = confirm.matanim_joint;
        sss_table.confirm_shapeanim = confirm.shapeanim_joint;
        sss_ready = 1;
        sss_use_us = use_us;
        if (sss_log) {
            fprintf(sss_log,
                    "GAME_SSS_NATIVE_PREPARE_PASS source=%s sets=12 camera=table+0 renderer=vitaGL shapeanim=none\n",
                    use_us ? "MnSlMap.usd" : "MnSlMap.dat");
            fflush(sss_log);
        }
    }
    *data_table = &sss_table;
    return 0;
}

const MvCamera *mv_sss_vita_camera(void) { return sss_ready ? &sss_camera : NULL; }
