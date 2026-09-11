#include "css_assets_vita.h"
#include "asset_file.h"
#include "hsd_anim_native.h"
#include "hsd_data.h"
#include "hsd_matanim_native.h"
#include "hsd_native.h"

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
} MvCssSceneModels;

typedef struct {
    HSD_Joint *joint;
    HSD_AnimJoint *anim;
    HSD_MatAnimJoint *matanim;
    void *shapeanim;
} MvCssAnimSet;

typedef struct {
    MvCssSceneModels models;
    MvCssAnimSet anim[9];
} MvCssDataTable;

static unsigned char *css_bytes;
static size_t css_size;
static MvDat css_dat;
static int css_dat_open;
static int css_ready;
static MvNativeHsd css_models[9];
static MvNativeAnim css_anims[9];
static MvNativeMatAnim css_matanims[9];
static MvCamera css_camera;
static HSD_WObjDesc css_eye, css_interest;
static Vec3 css_up;
static HSD_CameraDescPerspective css_camera_desc;
static MvCssDataTable css_table;
static FILE *css_log;

void mv_css_vita_set_log(FILE *log) { css_log = log; }
void mv_css_vita_trace(const char *marker)
{
    if (!css_log || !marker) return;
    fprintf(css_log, "%s\n", marker);
    fflush(css_log);
}

static int public_offset(const char *wanted, uint32_t *out)
{
    for (uint32_t i = 0; i < css_dat.public_count; ++i) {
        const char *name; uint32_t offset;
        if (mv_dat_public(&css_dat, i, &name, &offset)) return -1;
        if (!strcmp(name, wanted)) { *out = offset; return 0; }
    }
    return -1;
}

static int table_pointer(uint32_t field, uint32_t *target)
{
    int r = mv_dat_pointer(&css_dat, field, target);
    return r < 0 ? -1 : r;
}

static int build_camera(uint32_t offset)
{
    if (mv_camera_read_at(&css_dat, offset, &css_camera)) return -1;
    if (css_camera.projection_type != PROJ_PERSPECTIVE) return -2;
    memset(&css_eye, 0, sizeof(css_eye));
    memset(&css_interest, 0, sizeof(css_interest));
    memset(&css_camera_desc, 0, sizeof(css_camera_desc));
    css_eye.pos.x = css_camera.eye[0]; css_eye.pos.y = css_camera.eye[1]; css_eye.pos.z = css_camera.eye[2];
    css_interest.pos.x = css_camera.interest[0]; css_interest.pos.y = css_camera.interest[1]; css_interest.pos.z = css_camera.interest[2];
    css_up.x = css_camera.up[0]; css_up.y = css_camera.up[1]; css_up.z = css_camera.up[2];
    css_camera_desc.class_name = NULL;
    css_camera_desc.flags = css_camera.flags;
    css_camera_desc.projection_type = css_camera.projection_type;
    css_camera_desc.viewport.xmin = css_camera.viewport_xmin; css_camera_desc.viewport.xmax = css_camera.viewport_xmax;
    css_camera_desc.viewport.ymin = css_camera.viewport_ymin; css_camera_desc.viewport.ymax = css_camera.viewport_ymax;
    css_camera_desc.scissor.left = css_camera.scissor_left; css_camera_desc.scissor.right = css_camera.scissor_right;
    css_camera_desc.scissor.top = css_camera.scissor_top; css_camera_desc.scissor.bottom = css_camera.scissor_bottom;
    css_camera_desc.eyepos = &css_eye; css_camera_desc.interest = &css_interest;
    css_camera_desc.roll = 0.0f; css_camera_desc.up_vector = &css_up;
    css_camera_desc.nnear = css_camera.near_z; css_camera_desc.ffar = css_camera.far_z;
    css_camera_desc.fov = css_camera.fov_y; css_camera_desc.aspect = css_camera.aspect;
    css_table.models.cam = (HSD_CObjDesc *)&css_camera_desc;
    return 0;
}

void mv_css_vita_release(void)
{
    for (unsigned i = 0; i < 9; ++i) {
        mv_native_matanim_free(&css_matanims[i]);
        mv_native_anim_free(&css_anims[i]);
        mv_hsd_native_free(&css_models[i]);
    }
    if (css_dat_open) { mv_dat_close(&css_dat); css_dat_open = 0; }
    free(css_bytes); css_bytes = NULL; css_size = 0; css_ready = 0;
    memset(&css_table, 0, sizeof(css_table));
}

int mv_css_vita_prepare(void **data_table)
{
    if (!data_table) return -1;
    if (!css_ready) {
        int r = mv_read_file("ux0:data/SmashMeleeVita/files/MnSlChr.usd", &css_bytes, &css_size);
        if (r) return -10;
        if (mv_dat_open(&css_dat, css_bytes, css_size)) { mv_css_vita_release(); return -11; }
        css_dat_open = 1;
        uint32_t table = 0, target = 0;
        if (public_offset("MnSelectChrDataTable", &table)) { mv_css_vita_release(); return -12; }
        r = table_pointer(table + 0, &target);
        if (r != 1 || build_camera(target)) { mv_css_vita_release(); return -13; }
        css_table.models.light0 = NULL; css_table.models.light1 = NULL; css_table.models.fog = NULL;
        for (unsigned i = 0; i < 9; ++i) {
            uint32_t base = table + 16 + i * 16;
            r = table_pointer(base + 0, &target);
            if (r < 0) { mv_css_vita_release(); return -20 - (int)i; }
            if (r == 1) {
                int br = mv_hsd_native_build_at(&css_dat, target, &css_models[i]);
                if (br) {
                    if (css_log) fprintf(css_log, "GAME_CSS_NATIVE_FAIL set=%u code=%d unsupported=%s offset=%08x value=%08x\n", i, br, mv_hsd_native_unsupported_name(css_models[i].unsupported_kind), css_models[i].unsupported_offset, css_models[i].unsupported_value);
                    mv_css_vita_release(); return -30 - (int)i;
                }
                css_table.anim[i].joint = css_models[i].root;
            }
            r = table_pointer(base + 4, &target);
            if (r < 0) { mv_css_vita_release(); return -40 - (int)i; }
            if (r == 1) { if (mv_native_anim_build_at(&css_dat, target, &css_anims[i])) { mv_css_vita_release(); return -50 - (int)i; } css_table.anim[i].anim = css_anims[i].root; }
            r = table_pointer(base + 8, &target);
            if (r < 0) { mv_css_vita_release(); return -60 - (int)i; }
            if (r == 1) { if (mv_native_matanim_build_at(&css_dat, target, &css_matanims[i])) { mv_css_vita_release(); return -70 - (int)i; } css_table.anim[i].matanim = css_matanims[i].root; }
            r = table_pointer(base + 12, &target);
            if (r != 0) { mv_css_vita_release(); return -80 - (int)i; }
        }
        css_ready = 1;
        if (css_log) {
            fprintf(css_log, "GAME_CSS_NATIVE_PREPARE_PASS source=MnSlChr.usd sets=9 camera=table+0 renderer=vitaGL\n");
            fflush(css_log);
        }
    }
    *data_table = &css_table;
    return 0;
}

const MvCamera *mv_css_vita_camera(void) { return css_ready ? &css_camera : NULL; }
