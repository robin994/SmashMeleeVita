#include "menu_boot_vita.h"
#include "title_boot_vita.h"

#include "asset_file.h"
#include "gx_capture_vita.h"
#include "gx_replay_vita.h"
#include "hsd_anim_native.h"
#include "hsd_data.h"
#include "hsd_matanim_native.h"
#include "hsd_native.h"
#include "hsd_runtime_probe.h"
#include "hsd_scene.h"
#include "render_vita.h"
#include "frame_telemetry_vita.h"

#include <dolphin/pad.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/types.h>
#include <melee/lb/lbcardgame.h>
#include <melee/lb/lbcardnew.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbheap.h>
#include <melee/mn/mnmain.h>
#include <melee/mn/forward.h>
#include <psp2/ctrl.h>
#include <stdlib.h>
#include <string.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/wobj.h>

static unsigned char *menu_bytes;
static size_t menu_size;
static MvDat menu_dat;
static int menu_dat_open;
static MvNativeHsd menu_models[4];
static MvNativeAnim menu_anims[4];
static MvNativeMatAnim menu_matanims[4];
static MvCamera menu_camera;
static HSD_WObjDesc menu_eye;
static HSD_WObjDesc menu_interest;
static FILE *menu_log;
static Vec3 menu_up;

typedef enum {
    MENU_PROXY_NONE = 0,
    MENU_PROXY_JOINT,
    MENU_PROXY_ANIM,
    MENU_PROXY_MATANIM,
    MENU_PROXY_RAW_LIST,
    MENU_PROXY_OPTIONAL_NULL,
} MvMenuProxyKind;

typedef struct {
    char name[64];
    MvMenuProxyKind kind;
    void *ptr;
    void *owned;
    MvNativeHsd hsd;
    MvNativeAnim anim;
    MvNativeMatAnim matanim;
} MvMenuProxyEntry;

#define MV_MENU_PROXY_MAX 256u
static MvMenuProxyEntry menu_proxy_entries[MV_MENU_PROXY_MAX];
static size_t menu_proxy_count;
static HSD_Archive menu_archive_proxy_token;

static int menu_public_offset(const char *name, uint32_t *offset)
{
    if (!name || !offset || !menu_dat_open) return -1;
    for (uint32_t i = 0; i < menu_dat.public_count; ++i) {
        const char *symbol = NULL;
        uint32_t target = 0;
        if (mv_dat_public(&menu_dat, i, &symbol, &target)) return -1;
        if (symbol && strcmp(symbol, name) == 0) {
            *offset = target;
            return 0;
        }
    }
    return -1;
}

static int menu_name_ends_with(const char *name, const char *suffix)
{
    size_t a = strlen(name), b = strlen(suffix);
    return a >= b && strcmp(name + a - b, suffix) == 0;
}

static void menu_proxy_release(void)
{
    for (size_t i = 0; i < menu_proxy_count; ++i) {
        MvMenuProxyEntry *e = &menu_proxy_entries[i];
        if (e->kind == MENU_PROXY_JOINT) mv_hsd_native_free(&e->hsd);
        else if (e->kind == MENU_PROXY_ANIM) mv_native_anim_free(&e->anim);
        else if (e->kind == MENU_PROXY_MATANIM) mv_native_matanim_free(&e->matanim);
        free(e->owned);
    }
    memset(menu_proxy_entries, 0, sizeof(menu_proxy_entries));
    menu_proxy_count = 0;
}

HSD_Archive *mv_menu_vita_archive_proxy(void)
{
    return &menu_archive_proxy_token;
}

int mv_menu_vita_archive_is_proxy(const HSD_Archive *archive)
{
    return archive == &menu_archive_proxy_token;
}

static void *menu_proxy_build_raw_list(MvMenuProxyEntry *e, uint32_t offset)
{
    char **list = calloc(257u, sizeof(*list));
    if (!list) return NULL;
    for (unsigned i = 0; i < 256u; ++i) {
        uint32_t target = 0;
        int r = mv_dat_pointer(&menu_dat, offset + i * 4u, &target);
        if (r < 0) { free(list); return NULL; }
        if (!r) { list[i] = NULL; break; }
        const uint8_t *p = mv_dat_span(&menu_dat, target, 1);
        if (!p) { free(list); return NULL; }
        list[i] = (char *)p;
        if (*p == 0) break;
    }
    e->owned = list;
    return list;
}

void *mv_menu_vita_archive_lookup(const char *name)
{
    if (!name || !menu_dat_open) return NULL;
    for (size_t i = 0; i < menu_proxy_count; ++i)
        if (strcmp(menu_proxy_entries[i].name, name) == 0)
            return menu_proxy_entries[i].ptr;
    if (menu_proxy_count >= MV_MENU_PROXY_MAX) return NULL;

    uint32_t offset = 0;
    if (menu_public_offset(name, &offset)) {
        if (menu_log) { fprintf(menu_log, "GAME_MENU_PROXY_MISS symbol=%s\n", name); fflush(menu_log); }
        return NULL;
    }

    MvMenuProxyEntry *e = &menu_proxy_entries[menu_proxy_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    int result = 0;
    if (menu_name_ends_with(name, "_shapeanim_joint")) {
        e->kind = MENU_PROXY_OPTIONAL_NULL;
        e->ptr = NULL;
    } else if (menu_name_ends_with(name, "_matanim_joint")) {
        e->kind = MENU_PROXY_MATANIM;
        result = mv_native_matanim_build_at(&menu_dat, offset, &e->matanim);
        e->ptr = result ? NULL : e->matanim.root;
    } else if (menu_name_ends_with(name, "_animjoint")) {
        e->kind = MENU_PROXY_ANIM;
        result = mv_native_anim_build_at(&menu_dat, offset, &e->anim);
        e->ptr = result ? NULL : e->anim.root;
    } else if (menu_name_ends_with(name, "_joint")) {
        e->kind = MENU_PROXY_JOINT;
        result = mv_hsd_native_build_at(&menu_dat, offset, &e->hsd);
        e->ptr = result ? NULL : e->hsd.root;
    } else if (strncmp(name, "mnName", 6) == 0) {
        e->kind = MENU_PROXY_RAW_LIST;
        e->ptr = menu_proxy_build_raw_list(e, offset);
        result = e->ptr ? 0 : -1;
    } else {
        e->kind = MENU_PROXY_NONE;
        result = -1;
    }

    if (menu_log) {
        fprintf(menu_log, "GAME_MENU_PROXY_SYMBOL symbol=%s offset=%08x owner=%u kind=%d result=%d ptr=%p\n",
                name, offset, (unsigned)mn_804A04F0.cur_menu, (int)e->kind, result, e->ptr);
        fflush(menu_log);
    }
    if (result) return NULL;
    return e->ptr;
}
void mv_menu_vita_request_action(int menu_kind, int selection)
{
    mn_804A04F0.entering_menu = 0;
    if (menu_log) {
        fprintf(menu_log, "GAME_MENU_ACTION_DEFERRED menu=%d selection=%d renderer=vitaGL\n", menu_kind, selection);
        fflush(menu_log);
    }
}

static HSD_CameraDescPerspective menu_camera_desc;
static int menu_assets_ready;
static unsigned return_menu = MENU_KIND_MAIN;
static unsigned return_selection = SEL_MAIN_1P;
void mv_menu_vita_reset_return(void)
{
    return_menu = MENU_KIND_MAIN;
    return_selection = SEL_MAIN_1P;
}


static const char *const menu_joint_names[4] = {
    "MenMainBack_Top_joint",
    "MenMainPanel_Top_joint",
    "MenMainConTop_Top_joint",
    "MenMainCursor_Top_joint",
};

static const char *const menu_anim_names[4] = {
    "MenMainBack_Top_animjoint",
    "MenMainPanel_Top_animjoint",
    "MenMainConTop_Top_animjoint",
    "MenMainCursor_Top_animjoint",
};

static const char *const menu_matanim_names[4] = {
    "MenMainBack_Top_matanim_joint",
    "MenMainPanel_Top_matanim_joint",
    "MenMainConTop_Top_matanim_joint",
    "MenMainCursor_Top_matanim_joint",
};

static void menu_release_assets(void)
{
    menu_proxy_release();
    for (unsigned i = 0; i < 4; ++i) {
        mv_native_matanim_free(&menu_matanims[i]);
        mv_native_anim_free(&menu_anims[i]);
        mv_hsd_native_free(&menu_models[i]);
    }
    if (menu_dat_open) {
        mv_dat_close(&menu_dat);
        menu_dat_open = 0;
    }
    free(menu_bytes);
    menu_bytes = NULL;
    menu_size = 0;
    menu_assets_ready = 0;
}

static int menu_camera_from_dat(void)
{
    if (mv_camera_read(&menu_dat, "ScMenMain_cam_int1_camera", &menu_camera))
        return -1;
    if (menu_camera.projection_type != PROJ_PERSPECTIVE)
        return -2;

    memset(&menu_eye, 0, sizeof(menu_eye));
    memset(&menu_interest, 0, sizeof(menu_interest));
    memset(&menu_camera_desc, 0, sizeof(menu_camera_desc));

    menu_eye.pos.x = menu_camera.eye[0];
    menu_eye.pos.y = menu_camera.eye[1];
    menu_eye.pos.z = menu_camera.eye[2];
    menu_interest.pos.x = menu_camera.interest[0];
    menu_interest.pos.y = menu_camera.interest[1];
    menu_interest.pos.z = menu_camera.interest[2];
    menu_up.x = menu_camera.up[0];
    menu_up.y = menu_camera.up[1];
    menu_up.z = menu_camera.up[2];

    menu_camera_desc.class_name = NULL;
    menu_camera_desc.flags = menu_camera.flags;
    menu_camera_desc.projection_type = menu_camera.projection_type;
    menu_camera_desc.viewport.xmin = menu_camera.viewport_xmin;
    menu_camera_desc.viewport.xmax = menu_camera.viewport_xmax;
    menu_camera_desc.viewport.ymin = menu_camera.viewport_ymin;
    menu_camera_desc.viewport.ymax = menu_camera.viewport_ymax;
    menu_camera_desc.scissor.left = menu_camera.scissor_left;
    menu_camera_desc.scissor.right = menu_camera.scissor_right;
    menu_camera_desc.scissor.top = menu_camera.scissor_top;
    menu_camera_desc.scissor.bottom = menu_camera.scissor_bottom;
    menu_camera_desc.eyepos = &menu_eye;
    menu_camera_desc.interest = &menu_interest;
    menu_camera_desc.roll = 0.0f;
    menu_camera_desc.up_vector = &menu_up;
    menu_camera_desc.nnear = menu_camera.near_z;
    menu_camera_desc.ffar = menu_camera.far_z;
    menu_camera_desc.fov = menu_camera.fov_y;
    menu_camera_desc.aspect = menu_camera.aspect;
    return 0;
}

int mv_menu_vita_prepare(StaticModelDesc *back, StaticModelDesc *panel,
                         StaticModelDesc *content, StaticModelDesc *cursor,
                         HSD_CObjDesc **camera)
{
    StaticModelDesc *out[4] = { back, panel, content, cursor };
    if (!back || !panel || !content || !cursor || !camera)
        return -1;

    if (!menu_assets_ready) {
        int result = mv_read_file("ux0:data/SmashMeleeVita/files/MnMaAll.usd",
                                  &menu_bytes, &menu_size);
        if (result)
            return -10;
        if (mv_dat_open(&menu_dat, menu_bytes, menu_size)) {
            menu_release_assets();
            return -11;
        }
        menu_dat_open = 1;

        for (unsigned i = 0; i < 4; ++i) {
            result = mv_hsd_native_build(&menu_dat, menu_joint_names[i],
                                         &menu_models[i]);
            if (result) {
                if (menu_log) {
                    fprintf(menu_log,
                            "GAME_MENU_NATIVE_FAIL root=%s code=%d unsupported=%s "
                            "offset=%08x value=%08x count=%u\n",
                            menu_joint_names[i], result,
                            mv_hsd_native_unsupported_name(menu_models[i].unsupported_kind),
                            menu_models[i].unsupported_offset,
                            menu_models[i].unsupported_value,
                            (unsigned)menu_models[i].unsupported_count);
                    fflush(menu_log);
                }
                menu_release_assets();
                return -20 - (int)i;
            }
            result = mv_native_anim_build(&menu_dat, menu_anim_names[i],
                                          &menu_anims[i]);
            if (result) {
                if (menu_log) {
                    fprintf(menu_log,
                            "GAME_MENU_ANIM_FAIL root=%s code=%d\n",
                            menu_anim_names[i], result);
                    fflush(menu_log);
                }
                menu_release_assets();
                return -30 - (int)i;
            }
            result = mv_native_matanim_build(&menu_dat, menu_matanim_names[i],
                                             &menu_matanims[i]);
            if (result) {
                if (menu_log) {
                    fprintf(menu_log,
                            "GAME_MENU_MATANIM_FAIL root=%s code=%d\n",
                            menu_matanim_names[i], result);
                    fflush(menu_log);
                }
                menu_release_assets();
                return -40 - (int)i;
            }
        }
        result = menu_camera_from_dat();
        if (result) {
            menu_release_assets();
            return -50 + result;
        }
        menu_assets_ready = 1;
    }

    for (unsigned i = 0; i < 4; ++i) {
        memset(out[i], 0, sizeof(*out[i]));
        out[i]->joint = menu_models[i].root;
        out[i]->animjoint = menu_anims[i].root;
        out[i]->matanim_joint = menu_matanims[i].root;
    }
    *camera = (HSD_CObjDesc *)&menu_camera_desc;

    if (menu_log) {
        fprintf(menu_log,
                "GAME_MENU_NATIVE_PREPARE_PASS source=MnMaAll.usd "
                "roots=Back+Panel+ConTop+Cursor camera=ScMenMain "
                "billboard=JOBJ_native envelope=HSD_palette_capture "
                "pnmtxidx=per_vertex vita_renderer=" MV_RENDER_NAME " "
                "matanim=native shapeanim=unused ") ;
        for (unsigned i = 0; i < 4; ++i) {
            fprintf(menu_log,
                    "%s%s=%u/%u/%u/%u/%u",
                    i ? " " : "",
                    i == 0 ? "back" : i == 1 ? "panel" :
                    i == 2 ? "content" : "cursor",
                    (unsigned)menu_matanims[i].joint_count,
                    (unsigned)menu_matanims[i].matanim_count,
                    (unsigned)menu_matanims[i].texanim_count,
                    (unsigned)menu_matanims[i].aobj_count,
                    (unsigned)menu_matanims[i].fobj_count);
        }
        fprintf(menu_log, "\n");
        fflush(menu_log);
    }
    return 0;
}

static HSD_GObj *find_menu_gobj(unsigned p_link, unsigned gx_link)
{
    HSD_GObj *gobj = HSD_GObjPLinkHead[p_link];
    for (; gobj != NULL; gobj = gobj->next) {
        if (gobj->gx_link == gx_link && gobj->obj_kind == HSD_GObj_JObjKind &&
            gobj->hsd_obj != NULL)
            return gobj;
    }
    return NULL;
}

#define MV_MENU_CAPTURE_ROOTS 512u
static int capture_menu_jobj(HSD_GObj *gobj, int recapture,
                             MvGxCaptureStats *stats, unsigned *roots,
                             void **seen)
{
    if (!gobj || gobj->obj_kind != HSD_GObj_JObjKind || !gobj->hsd_obj)
        return 0;
    for (unsigned i = 0; i < *roots; ++i)
        if (seen[i] == gobj->hsd_obj) return 0;
    if (*roots == MV_MENU_CAPTURE_ROOTS) return -90;
    int r = mv_hsd_gx_capture_runtime((HSD_JObj *)gobj->hsd_obj, *roots == 0,
                                      recapture, stats);
    if (!r) seen[(*roots)++] = gobj->hsd_obj;
    return r;
}

static int capture_live_menu(MvGxReplay *replay, MvGxCaptureStats *stats,
                             int initialize)
{
    /* Preserve the base layer order that is already hardware-validated, then
     * append every other live JObj in actual GX-link order. External Melee
     * menus create/destroy their own GObjs inside the same scene; discovering
     * them every frame makes the Vita renderer follow that graph without a
     * screen-specific renderer or hard-coded root list. */
    HSD_GObj *background = find_menu_gobj(5, 2);
    HSD_GObj *panel = find_menu_gobj(6, 3);
    HSD_GObj *content = find_menu_gobj(7, 4);
    if (initialize && (!background || !panel || !content)) {
        if (menu_log) {
            fprintf(menu_log,
                    "GAME_MENU_GOBJ_LOOKUP_FAIL background=%p panel=%p "
                    "content=%p expected_plinks=5,6,7 gxlinks=2,3,4\n",
                    (void *)background, (void *)panel, (void *)content);
            fflush(menu_log);
        }
        return -1;
    }

    if (initialize && menu_log) {
        fprintf(menu_log,
                "GAME_MENU_GOBJ_LOOKUP_PASS background=%p panel=%p content=%p "
                "plinks=5,6,7 gxlinks=2,3,4\n",
                (void *)background->hsd_obj, (void *)panel->hsd_obj,
                (void *)content->hsd_obj);
        fflush(menu_log);
    }

    memset(stats, 0, sizeof(*stats));
    unsigned roots = 0, dynamic_roots = 0;
    void *seen[MV_MENU_CAPTURE_ROOTS];
    const int recapture = initialize ? 0 : 1;
    if (capture_menu_jobj(background, recapture, stats, &roots, seen)) return -2;
    const uint32_t background_commands = stats->commands;
    if (capture_menu_jobj(panel, recapture, stats, &roots, seen)) return -3;
    if (capture_menu_jobj(content, recapture, stats, &roots, seen)) return -4;

    const unsigned max_link = (unsigned)HSD_GObjLibInitData.gx_link_max + 1u;
    for (unsigned link = 0; link <= max_link; ++link) {
        for (HSD_GObj *gobj = HSD_GObjGXLinkHead[link]; gobj;
             gobj = gobj->next_gx) {
            if (gobj == background || gobj == panel || gobj == content)
                continue;
            if (gobj->obj_kind != HSD_GObj_JObjKind || !gobj->hsd_obj)
                continue;
            unsigned before = roots;
            int r = capture_menu_jobj(gobj, recapture, stats, &roots, seen);
            if (r) {
                if (menu_log) {
                    fprintf(menu_log,
                            "GAME_MENU_DYNAMIC_CAPTURE_FAIL gxlink=%u plink=%u "
                            "classifier=%u code=%d roots=%u\n",
                            link, (unsigned)gobj->p_link,
                            (unsigned)gobj->classifier, r, roots);
                    fflush(menu_log);
                }
                return -10 - r;
            }
            dynamic_roots += roots - before;
        }
    }

    if (!roots) return -91;
    int sis_count = HSD_SisLib_VitaCaptureAll();
    if (mv_gx_capture_stats(stats)) return -92;
    if (initialize) {
        if (mv_gx_replay_init_relaxed_from(replay, &menu_camera, menu_log,
                                           background_commands))
            return -5;
        replay->relaxed_from_command = background_commands;
        if (menu_log) {
            fprintf(menu_log,
                    "GAME_MENU_DYNAMIC_CAPTURE_PASS roots=%u dynamic=%u sis=%d "
                    "commands=%u triangles=%u\n",
                    roots, dynamic_roots, sis_count, stats->commands, stats->triangles);
            fflush(menu_log);
        }
    } else {
        replay->relaxed_from_command = background_commands;
    }
    return 0;
}

int mv_main_menu_run(FILE *log)
{
    int result = 0;
    menu_log = log;
    mv_gm_vita_enter_mode(GM_MENU);
    if (mv_render_init() < 0)
        return -50;

    /* The direct Title -> MAIN milestone bypasses gm_801A4014(), whose
     * preloadState() normally rebuilds heap 0 before a new game-mode scene.
     * Without this transition lbArchive_LoadSymbols("LbAd.dat") receives a
     * NULL heap allocation and DevCom correctly cancels the request.  Recreate
     * the exact GM_MENU preload boundary before allocating any new GObjs. */
    if (log) {
        fprintf(log,
                "GAME_MENU_PRELOAD_BEGIN preload=lbDvdPreload_2 "
                "heap0=%d heap2=%d heap3=%d\n",
                (int)lbHeap_80015BB8(0), (int)lbHeap_80015BB8(2),
                (int)lbHeap_80015BB8(3));
        fflush(log);
    }
    lbDvd_80018CF4(lbDvdPreload_2);
    mv_scene_vita_sis_init(0x4800);
    if (log) {
        fprintf(log,
                "GAME_MENU_PRELOAD_PASS preload=lbDvdPreload_2 "
                "heap0=%d heap2=%d heap3=%d sislib=0x4800\n",
                (int)lbHeap_80015BB8(0), (int)lbHeap_80015BB8(2),
                (int)lbHeap_80015BB8(3));
        fflush(log);
    }
    HSD_ASSERTREPORT(0x310, lbHeap_80015BB8(0) == LbHeapStatus_Create,
                     "Vita GM_MENU preload did not recreate heap 0\n");

    mv_scene_vita_objects_init();
    gm_801A3E88();
    mv_scene_vita_begin_menu();

    /* gmmenumode.c:onEnter() performs these before the menu scene is entered.
     * The direct Vita Title -> MAIN path must preserve that game-mode boundary
     * or lbCardGame_UpdatePowerTime() in mnMain_Scene_OnEnter() asserts. */
    lbCardNew_AllocWorkArea();
    lbCardGame_LoadArchive(0);
    if (log) {
        fprintf(log,
                "GAME_MENU_CARDGAME_PASS source=gmmenumode_onEnter "
                "workarea=allocated archives=LbMcGame+NtMemAc\n");
        fflush(log);
    }

    MenuEnterData enter = {0};
    enter.menu_kind = return_menu;
    enter.hovered_selection = return_selection;
    enter.load_assets = 1;

    if (log) {
        fprintf(log,
                "GAME_MENU_ORIGINAL_ON_ENTER_BEGIN source=mnmain.c "
                "menu=MENU_KIND_MAIN renderer=" MV_RENDER_NAME "\n");
        fflush(log);
    }
    mnMain_Scene_OnEnter(&enter);

    MvGxReplay replay;
    memset(&replay, 0, sizeof(replay));
    MvGxCaptureStats capture = {0};
    result = capture_live_menu(&replay, &capture, 1);
    if (result) {
        if (log) {
            fprintf(log, "GAME_MENU_ORIGINAL_CAPTURE_FAIL code=%d\n", result);
            fflush(log);
        }
        mv_render_fini();
        menu_release_assets();
        menu_log = NULL;
        return -60 + result;
    }

    if (log) {
        fprintf(log,
                "GAME_MENU_ORIGINAL_ENTER_PASS commands=%u triangles=%u "
                "selection=%u source=mnMain_Scene_OnEnter\n",
                capture.commands, capture.triangles,
                (unsigned)mn_804A04F0.hovered_selection);
        fflush(log);
    }

    MvFrameTelemetry timing;
    mv_frame_telemetry_init(&timing, log, "MENU");
    unsigned frames = 0;
    unsigned last_selection = mn_804A04F0.hovered_selection;
    unsigned last_menu = mn_804A04F0.cur_menu;
    if (log) {
        fprintf(log, "GAME_MENU_NODE_ENTER menu=%u selection=%u\n", last_menu, last_selection);
        fflush(log);
    }
    for (;;) {
        uint64_t frame_start = mv_frame_time_us();
        HSD_PadRenewStatus();
        gm_EvaluateAllControllerInputs();
        HSD_GObj_RunProcs();
        if (mn_804A04F0.cur_menu != last_menu) {
            if (log) {
                fprintf(log, "GAME_MENU_NODE_EXIT menu=%u selection=%u\n",
                        last_menu, last_selection);
                fprintf(log, "GAME_MENU_EDGE from=%u selection=%u to=%u\n",
                        last_menu, last_selection, (unsigned)mn_804A04F0.cur_menu);
                fprintf(log, "GAME_MENU_NODE_ENTER menu=%u selection=%u prev=%u entering=%u\n",
                        (unsigned)mn_804A04F0.cur_menu,
                        (unsigned)mn_804A04F0.hovered_selection,
                        (unsigned)mn_804A04F0.prev_menu,
                        (unsigned)mn_804A04F0.entering_menu);
                fflush(log);
            }
            last_menu = mn_804A04F0.cur_menu;
        }
        if (mv_scene_vita_done()) {
            if (log) {
                fprintf(log, "GAME_MENU_ROUTE frame=%u menu=%u selection=%u pending_mode=%d\n", frames, (unsigned)mn_804A04F0.cur_menu, (unsigned)mn_804A04F0.hovered_selection, mv_scene_vita_pending_mode());
                fflush(log);
            }
            int mode = mv_scene_vita_pending_mode();
            if (log) {
                fprintf(log, "GAME_MENU_GM_EXIT mode=%d menu=%u selection=%u port=%u\n",
                        mode, last_menu, last_selection, (unsigned)gm_801677F0());
                fflush(log);
            }
            return_menu = mn_804A04F0.cur_menu;
            return_selection = mn_804A04F0.hovered_selection;
            if (mode < 0 || mode >= GM_COUNT) result = -70;
            break;
        }

        uint64_t capture_start = mv_frame_time_us();
        result = capture_live_menu(&replay, &capture, 0);
        if (result) {
            if (log) {
                fprintf(log,
                        "GAME_MENU_ORIGINAL_LIVE_CAPTURE_FAIL frame=%u code=%d\n",
                        frames, result);
                fflush(log);
            }
            break;
        }

        uint64_t replay_start = mv_frame_time_us();
        mv_render_begin();
        mv_gx_replay_draw(&replay, &menu_camera);
        uint64_t present_start = mv_frame_time_us();
        mv_render_present();
        uint64_t frame_end = mv_frame_time_us();
        mv_frame_telemetry_record(&timing, frame_end - frame_start,
                                 replay_start - capture_start,
                                 present_start - replay_start,
                                 frame_end - present_start);
        mv_frame_telemetry_flush(&timing, 0);
        ++frames;

        if (log && (frames == 1 || frames == 60 || frames == 180)) {
            fprintf(log,
                    "GAME_MENU_LIVE_PASS frame=%u commands=%u triangles=%u "
                    "selection=%u matanim=native renderer=" MV_RENDER_NAME "\n",
                    frames, capture.commands, capture.triangles,
                    (unsigned)mn_804A04F0.hovered_selection);
            fflush(log);
        }

        if (mn_804A04F0.hovered_selection != last_selection) {
            last_selection = mn_804A04F0.hovered_selection;
            if (log) {
                fprintf(log,
                        "GAME_MENU_SELECTION frame=%u selection=%u "
                        "source=mn_8022DB10\n",
                        frames, last_selection);
                fflush(log);
            }
        }

        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
            (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
                (SCE_CTRL_SELECT | SCE_CTRL_START))
            break;
    }

    mv_frame_telemetry_flush(&timing, 1);
    lbAudioAx_VitaSfxStateTrace("MENU_EXIT");

    if (log) {
        fprintf(log,
                "GAME_MENU_ORIGINAL_EXIT frames=%u selection=%u result=%d\n",
                frames, (unsigned)mn_804A04F0.hovered_selection, result);
        fflush(log);
    }
    mv_gx_replay_close(&replay);
    mv_render_fini();
    mv_scene_vita_objects_close();
    menu_release_assets();
    menu_log = NULL;
    return result;
}
