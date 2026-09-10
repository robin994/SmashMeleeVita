#include "title_boot_vita.h"

#include "asset_file.h"
#include "gx_capture_vita.h"
#include "gx_replay_vita.h"
#include "hsd_data.h"
#include "hsd_native.h"
#include "hsd_anim_native.h"
#include "hsd_matanim_native.h"
#include "hsd_runtime_probe.h"
#include "hsd_scene.h"

#include <dolphin/pad.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gm_1A45.h>
#include <melee/gm/gmtitle.h>
#include <melee/gm/types.h>
#include <melee/sc/types.h>
#include <psp2/ctrl.h>
#include <stdlib.h>
#include <string.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/sobjlib.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/wobj.h>
#include "render_vita.h"

static unsigned char *title_bytes;
static size_t title_size;
static MvDat title_dat;
static int title_dat_open;
static MvNativeAnim title_moji_anim, title_background_anim;
static MvNativeMatAnim title_moji_matanim, title_background_matanim;
static HSD_JObj *live_moji, *live_background;
static MvNativeHsd title_moji;
static MvNativeHsd title_background;
static MvCamera title_camera;
static HSD_WObjDesc title_eye;
static HSD_WObjDesc title_interest;
static Vec3 title_up;
static HSD_CameraDescPerspective title_camera_desc;
static int title_assets_ready;
static int title_exit_buttons;
static int title_scene_done;
static MenuExitData menu_exit_data;
static int menu_scene_active;

/* gmtitle.c normally gets these from gmopening.c. The direct-title Vita boot
 * starts at GM_TITLE, so the opening-movie counters are intentionally zero. */
HSD_SObjDesc *gm_804D67F0;
u32 gm_804D67EC;

static void title_release_assets(void)
{
    mv_native_anim_free(&title_moji_anim);
    mv_native_anim_free(&title_background_anim);
    mv_native_matanim_free(&title_moji_matanim);
    mv_native_matanim_free(&title_background_matanim);
    mv_hsd_native_free(&title_moji);
    mv_hsd_native_free(&title_background);
    if (title_dat_open) {
        mv_dat_close(&title_dat);
        title_dat_open = 0;
    }
    free(title_bytes);
    title_bytes = NULL;
    title_size = 0;
    title_assets_ready = 0;
}

static int title_camera_from_dat(void)
{
    if (mv_camera_read(&title_dat, "ScTitle_cam_int1_camera", &title_camera))
        return -1;
    if (title_camera.projection_type != 1)
        return -2;

    memset(&title_eye, 0, sizeof(title_eye));
    memset(&title_interest, 0, sizeof(title_interest));
    memset(&title_camera_desc, 0, sizeof(title_camera_desc));

    title_eye.pos.x = title_camera.eye[0];
    title_eye.pos.y = title_camera.eye[1];
    title_eye.pos.z = title_camera.eye[2];
    title_interest.pos.x = title_camera.interest[0];
    title_interest.pos.y = title_camera.interest[1];
    title_interest.pos.z = title_camera.interest[2];
    title_up.x = title_camera.up[0];
    title_up.y = title_camera.up[1];
    title_up.z = title_camera.up[2];

    title_camera_desc.class_name = NULL;
    title_camera_desc.flags = title_camera.flags;
    title_camera_desc.projection_type = title_camera.projection_type;
    title_camera_desc.viewport.xmin = title_camera.viewport_xmin;
    title_camera_desc.viewport.xmax = title_camera.viewport_xmax;
    title_camera_desc.viewport.ymin = title_camera.viewport_ymin;
    title_camera_desc.viewport.ymax = title_camera.viewport_ymax;
    title_camera_desc.scissor.left = title_camera.scissor_left;
    title_camera_desc.scissor.right = title_camera.scissor_right;
    title_camera_desc.scissor.top = title_camera.scissor_top;
    title_camera_desc.scissor.bottom = title_camera.scissor_bottom;
    title_camera_desc.eyepos = &title_eye;
    title_camera_desc.interest = &title_interest;
    title_camera_desc.roll = 0.0f;
    title_camera_desc.up_vector = &title_up;
    title_camera_desc.nnear = title_camera.near_z;
    title_camera_desc.ffar = title_camera.far_z;
    title_camera_desc.fov = title_camera.fov_y;
    title_camera_desc.aspect = title_camera.aspect;
    return 0;
}

int mv_title_vita_prepare(StaticModelDesc *moji, StaticModelDesc *background,
                          HSD_CameraDescPerspective **camera)
{
    if (!moji || !background || !camera)
        return -1;
    if (!title_assets_ready) {
        int result = mv_read_file("ux0:data/SmashMeleeVita/files/GmTtAll.usd",
                                  &title_bytes, &title_size);
        if (result)
            return -10;
        if (mv_dat_open(&title_dat, title_bytes, title_size)) {
            title_release_assets();
            return -11;
        }
        title_dat_open = 1;
        if (mv_hsd_native_build(&title_dat, "TtlMoji_Top_joint", &title_moji)) {
            title_release_assets();
            return -12;
        }
        if (mv_hsd_native_build(&title_dat, "TtlBg_Top_joint", &title_background)) {
            title_release_assets();
            return -13;
        }
        if (mv_native_anim_build(&title_dat,"TtlMoji_Top_animjoint",&title_moji_anim) ||
            mv_native_anim_build(&title_dat,"TtlBg_Top_animjoint",&title_background_anim)) {
            title_release_assets(); return -17;
        }
        result = mv_native_matanim_build(&title_dat,
                                         "TtlMoji_Top_matanim_joint",
                                         &title_moji_matanim);
        if (result) {
            title_release_assets();
            return -18;
        }
        result = mv_native_matanim_build(&title_dat,
                                         "TtlBg_Top_matanim_joint",
                                         &title_background_matanim);
        if (result) {
            title_release_assets();
            return -19;
        }
        result = title_camera_from_dat();
        if (result) {
            title_release_assets();
            return -14 + result;
        }
        title_assets_ready = 1;
    }

    memset(moji, 0, sizeof(*moji));
    memset(background, 0, sizeof(*background));
    moji->animjoint = title_moji_anim.root;
    background->animjoint = title_background_anim.root;
    moji->matanim_joint = title_moji_matanim.root;
    background->matanim_joint = title_background_matanim.root;
    moji->joint = title_moji.root;
    background->joint = title_background.root;
    *camera = &title_camera_desc;
    return 0;
}

/* Reuse the GObj allocator/list storage between native scenes. Re-running
 * HSD_GObj initialization loses its pool free lists at every Back transition. */
static int scene_objects_initialized;
static int scene_sis_owned;
void mv_scene_vita_objects_init(void)
{
    if (scene_objects_initialized) return;
    HSD_GObjLibInitDataType init = {0};
    HSD_GObj_803912E0(&init);
    init.gproc_pri_max = 0x18;
    HSD_GObj_80391304(&init);
    scene_objects_initialized = 1;
}

void mv_scene_vita_sis_init(unsigned size)
{
    if (scene_sis_owned) HSD_SisLib_803A5FBC();
    HSD_SisLib_803A6048(size);
    scene_sis_owned = 1;
}

void mv_scene_vita_objects_close(void)
{
    if (scene_sis_owned) {
        HSD_SisLib_803A5FBC();
        scene_sis_owned = 0;
    }
    if (!scene_objects_initialized) return;
    for (unsigned p = 0; p <= HSD_GObjLibInitData.p_link_max; ++p) {
        HSD_GObj **heads = (HSD_GObj **)HSD_GObj_Entities;
        while (heads[p]) HSD_GObjFree(heads[p]);
    }
}

/* The original callbacks require scene-specific, ARM-native exit storage. */
void *gm_GetCurrentSceneExitData(void)
{
    return menu_scene_active ? (void *)&menu_exit_data : (void *)&title_exit_buttons;
}

void gm_801A4B60(void)
{
    title_scene_done = 1;
}

void mv_scene_vita_reset(void)
{
    title_exit_buttons = 0;
    title_scene_done = 0;
    menu_scene_active = 0;
    memset(&menu_exit_data, 0, sizeof(menu_exit_data));
    menu_exit_data.pending_mode = GM_COUNT;
}

void mv_scene_vita_begin_menu(void)
{
    mv_scene_vita_reset();
    menu_scene_active = 1;
}

int mv_scene_vita_done(void)
{
    return title_scene_done;
}

int mv_scene_vita_pending_mode(void)
{
    return menu_scene_active ? menu_exit_data.pending_mode : GM_COUNT;
}

static int title_capture(FILE *log, MvGxReplay *replay)
{
    MvGxCaptureStats capture = {0};
    MvGxReplayClassStats classify = {0};
    if (mv_hsd_gx_capture_runtime(live_background, 1, 0, &capture))
        return -20;
    const uint32_t background_commands = capture.commands;
    if (mv_hsd_gx_capture_runtime(live_moji, 0, 0, &capture))
        return -21;
    if (mv_gx_capture_replay_classify(&classify))
        return -22;
    if (log) {
        fprintf(log,
                "GAME_TITLE_CAPTURE commands=%u vertices=%u triangles=%u supported=%u "
                "supported_triangles=%u skipped=%u multitex=%u tev=%u texcoord=%u "
                "mapmode=%u vertex_color=%u background_commands=%u source=TtlBg+TtlMoji\n",
                capture.commands, capture.vertices, capture.triangles,
                classify.supported_commands, classify.supported_triangles,
                classify.skipped_multitex + classify.skipped_custom_tev +
                    classify.skipped_texcoord + classify.skipped_mapmode +
                    classify.skipped_vertex_color, classify.skipped_multitex,
                classify.skipped_custom_tev, classify.skipped_texcoord,
                classify.skipped_mapmode, classify.skipped_vertex_color,
                background_commands);
        fflush(log);
    }
    return mv_gx_replay_init_relaxed_from(replay, &title_camera, log,
                                          background_commands) ? -23 : 0;
}
int mv_title_boot_run(FILE *log)
{
    if (mv_render_init() < 0)
        return -30;

    mv_gm_vita_enter_mode(GM_TITLE);
    mv_scene_vita_objects_init();
    gm_801A3E88();

    title_exit_buttons = 0;
    mv_scene_vita_reset();
    gm_804D67F0 = NULL;
    if (log) {
        fprintf(log, "GAME_TITLE_ON_ENTER_BEGIN source=gmtitle.c asset=GmTtAll.usd\n");
        fflush(log);
    }
    gm_Scene_Title_OnEnter(NULL);
    if (!title_assets_ready) {
        mv_render_fini();
        return -31;
    }
    if (log) {
        fprintf(log,
                "GAME_TITLE_MATANIM_NATIVE_PASS "
                "moji_joints=%u moji_matanim=%u moji_texanim=%u moji_aobj=%u moji_fobj=%u "
                "bg_joints=%u bg_matanim=%u bg_texanim=%u bg_aobj=%u bg_fobj=%u\n",
                (unsigned)title_moji_matanim.joint_count,
                (unsigned)title_moji_matanim.matanim_count,
                (unsigned)title_moji_matanim.texanim_count,
                (unsigned)title_moji_matanim.aobj_count,
                (unsigned)title_moji_matanim.fobj_count,
                (unsigned)title_background_matanim.joint_count,
                (unsigned)title_background_matanim.matanim_count,
                (unsigned)title_background_matanim.texanim_count,
                (unsigned)title_background_matanim.aobj_count,
                (unsigned)title_background_matanim.fobj_count);
        fflush(log);
    }

    live_moji = live_background = NULL;
    for (HSD_GObj *g=((HSD_GObj**)HSD_GObj_Entities)[15];g;g=g->next) {
        if (g->gx_link==3) live_background=g->hsd_obj;
        if (g->gx_link==9) live_moji=g->hsd_obj;
    }
    if (!live_moji || !live_background) { mv_render_fini(); return -32; }
    MvGxReplay replay;
    memset(&replay, 0, sizeof(replay));
    int result = title_capture(log, &replay);
    if (result) {
        if (log) {
            fprintf(log, "GAME_TITLE_CAPTURE_FAIL code=%d\n", result);
            fflush(log);
        }
        mv_render_fini();
        return result;
    }

    if (log) {
        fprintf(log, "GAME_TITLE_VISIBLE_LOOP_BEGIN source=gmtitle.c renderer=" MV_RENDER_NAME "\n");
        fflush(log);
    }
    unsigned frames = 0;
    int done_logged = 0;
    for (;;) {
        HSD_PadRenewStatus();
        gm_EvaluateAllControllerInputs();
        gm_Scene_Title_OnFrame();
        HSD_GObj_80390CFC();

        MvGxCaptureStats live_stats={0};
        if (mv_hsd_gx_capture_runtime(live_background,1,1,&live_stats)) { result=-33;break; }
        replay.relaxed_from_command=live_stats.commands;
        if (mv_hsd_gx_capture_runtime(live_moji,0,1,&live_stats)) { result=-34;break; }
        if (log && (frames==0 || frames==120)) {
            fprintf(log,"GAME_TITLE_LIVE_ANIM frame=%u commands=%u source=HSD_JObjAnimAll capture=persistent_objects\n",frames,live_stats.commands);
            fflush(log);
        }
        mv_render_begin();
        mv_gx_replay_draw(&replay, &title_camera);
        mv_render_present();
        if (log && (frames==0 || frames==120 || frames==300)) {
            fprintf(log,
                    "GAME_TITLE_TEXTURE_CACHE frame=%u textures=%u capacity=%u bytes=%u\n",
                    frames,replay.texture_count,replay.texture_capacity,
                    replay.texture_bytes);
            fflush(log);
        }
        ++frames;

        if (title_scene_done && !done_logged) {
            done_logged = 1;
            if (log) {
                fprintf(log,
                        "GAME_TITLE_START_TRIGGERED buttons=%08x frame=%u "
                        "next=GM_MENU\n",
                        title_exit_buttons, frames);
                fflush(log);
            }
            break;
        }

        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
            (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
                (SCE_CTRL_SELECT | SCE_CTRL_START))
            break;
    }

    mv_gx_replay_close(&replay);
    mv_render_fini();
    mv_scene_vita_objects_close();
    title_release_assets();
    if (log) {
        fprintf(log, "GAME_TITLE_EXIT frames=%u\n", frames);
        fflush(log);
    }
    return result ? result : title_scene_done ? 1 : 0;
}
