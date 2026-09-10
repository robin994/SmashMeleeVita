#include "onep_boot_vita.h"
#include "mode_route_vita.h"
#include <melee/gm/gmvsmelee.h>
#include "css_assets_vita.h"
#include "frame_telemetry_vita.h"
#include "gx_capture_vita.h"
#include "gx_replay_vita.h"
#include "hsd_runtime_probe.h"
#include "render_vita.h"
#include "title_boot_vita.h"

#include <melee/gm/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_1B03.h>
#include <melee/gm/gmclassic.h>
#include <melee/gm/gmadventure.h>
#include <melee/gm/types.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/lb_013B.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mn/mncharsel.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <psp2/ctrl.h>
#include <stdint.h>
#include <string.h>

extern CSSData gmClassic_80470708;
extern StartMeleeData gmClassic_80472AF8;
extern MatchExitInfo gmClassic_8047086C;
extern GameModeState gm_Mode_Classic_States[];

static int capture_live_css(MvGxReplay *replay, MvGxCaptureStats *stats,
                            const MvCamera *camera, FILE *log, int initialize)
{
    unsigned roots = 0;
    memset(stats, 0, sizeof(*stats));
    for (unsigned p = 0; p <= HSD_GObjLibInitData.p_link_max; ++p) {
        HSD_GObj *gobj = ((HSD_GObj **)HSD_GObj_Entities)[p];
        for (; gobj; gobj = gobj->next) {
            if (gobj->obj_kind != HSD_GObj_JObjKind || !gobj->hsd_obj) continue;
            int r = mv_hsd_gx_capture_runtime((HSD_JObj *)gobj->hsd_obj,
                                              roots == 0, initialize ? 0 : 1, stats);
            if (r) {
                if (log) { fprintf(log, "GAME_CSS_CAPTURE_ROOT_FAIL plink=%u gxlink=%u root=%u code=%d\n", p, gobj->gx_link, roots, r); fflush(log); }
                return -10 - r;
            }
            ++roots;
        }
    }
    if (!roots) return -2;
    if (initialize) {
        int r = mv_gx_replay_init_relaxed_from(replay, camera, log, 0);
        if (r) return -3;
        replay->relaxed_from_command = 0;
        if (log) { fprintf(log, "GAME_CSS_CAPTURE_INIT_PASS roots=%u commands=%u triangles=%u renderer=%s\n", roots, stats->commands, stats->triangles, MV_RENDER_NAME); fflush(log); }
    } else replay->relaxed_from_command = 0;
    return 0;
}

int mv_onep_mode_run(FILE *log, int mode)
{
    const MvModeRoute *route = mv_mode_vita_route(mode);
    if (mode != GM_CLASSIC && mode != GM_ADVENTURE && !route) return -1;
    if (mv_render_init() < 0) return -2;
    mv_gm_vita_enter_mode(mode);

    if (log) { fprintf(log, "GAME_1P_ROUTE_BEGIN mode=%s(%d) target=CSS->STAGE_MATCH\n", mode == GM_CLASSIC ? "CLASSIC" : "ADVENTURE", mode); fflush(log); }

    lbDvd_80018CF4(lbDvdPreload_3);
    /* gm_801A4BD4 loads LbRb.dat after the scene heap has been rebuilt.
     * Do the same here: heap 0 is invalid before lbDvd_80018CF4(). */
    lb_80014534();
    if (log) {
        fprintf(log, "RUMBLE_DATA_INIT_PASS scene=1P source=lb_013B.c asset=LbRb.dat symbol=lbRumbleData\n");
        fflush(log);
    }
    mv_scene_vita_sis_init(0x2400);
    mv_scene_vita_objects_init();
    gm_801A3E88();
    mv_scene_vita_reset();
    mv_css_vita_set_log(log);

    CSSData *css = route ? &gmVsMelee_CssData : &gmClassic_80470708;
    GameModeState css_state;
    memset(&css_state, 0, sizeof(css_state));
    css_state.id = 0x70;
    css_state.info.scene_kind = GS_CSS;
    css_state.info.enter_data = css;
    css_state.info.exit_data = css;

    lbAudioAx_VitaSfxStateTrace("1P_BEGIN");

    if (route) {
        static unsigned char initialized[GM_COUNT];
        if (!initialized[mode]) { route->init(); initialized[mode] = 1; }
        route->load();
        route->css_enter(&css_state);
    } else if (mode == GM_CLASSIC) {
        gm_Mode_Classic_OnInit();
        gm_Mode_Classic_OnLoad();
        gmClassic_801B3DD8(&css_state);
    } else {
        gm_Mode_Adventure_OnInit();
        gm_Mode_Adventure_OnLoad();
        gm_801B42E8(&css_state);
    }
    lbAudioAx_VitaSfxStateTrace("1P_AFTER_MODE_ONLOAD");

    const u8 match_type = css->match_type;
    if (log) { fprintf(log, "GAME_1P_CSS_ENTER mode=%d match_type=%u port=%u source=original_mode_onload+css_state_onenter\n", mode, match_type, (unsigned)gm_801677F0()); fflush(log); }
    mnCharSel_Scene_OnEnter(css);
    const MvCamera *camera = mv_css_vita_camera();
    if (!camera) { mv_render_fini(); mv_css_vita_release(); return -3; }

    MvGxReplay replay; memset(&replay, 0, sizeof(replay));
    MvGxCaptureStats capture = {0};
    int result = capture_live_css(&replay, &capture, camera, log, 1);
    if (result) { mv_render_fini(); mv_css_vita_release(); return -20 + result; }

    MvFrameTelemetry timing; mv_frame_telemetry_init(&timing, log, mode == GM_CLASSIC ? "CLASSIC_CSS" : "ADVENTURE_CSS");
    unsigned frames = 0;
    while (!mv_scene_vita_done()) {
        uint64_t frame0 = mv_frame_time_us();
        HSD_PadRenewStatus();
        gm_EvaluateAllControllerInputs();
        mnCharSel_Scene_OnFrame();
        HSD_GObj_80390CFC();
        if (mv_scene_vita_done()) break;

        uint64_t cap0 = mv_frame_time_us();
        result = capture_live_css(&replay, &capture, camera, log, 0);
        uint64_t cap1 = mv_frame_time_us();
        if (result) break;
        uint64_t replay0 = mv_frame_time_us();
        mv_render_begin();
        mv_gx_replay_draw(&replay, camera);
        uint64_t replay1 = mv_frame_time_us();
        mv_render_present();
        uint64_t present1 = mv_frame_time_us();
        ++frames;
        mv_frame_telemetry_record(&timing, present1 - frame0, cap1 - cap0, replay1 - replay0, present1 - replay1);
        mv_frame_telemetry_flush(&timing, 0);
        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 && (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) == (SCE_CTRL_SELECT | SCE_CTRL_START)) { result = -99; break; }
    }

    mnCharSel_Scene_OnExit(NULL);
    if (route)
        route->css_exit(&css_state);
    else if (mode == GM_CLASSIC)
        gmClassic_801B3E44(&css_state);
    else
        gm_801B4350(&css_state);

    s8 ckind = ChKind_None; u8 stocks = 0, color = 0, nametag = GM_NAMETAG_NONE, cpu_level = 0;
    gm_801B0730(css, &ckind, &stocks, &color, &nametag, &cpu_level);
    if (log) { fprintf(log, "GAME_1P_CSS_EXIT mode=%d frames=%u pending=%u ckind=%d stocks=%u color=%u level=%u result=%d state_exit=original\n", mode, frames, css->pending_scene_change, ckind, stocks, color, cpu_level, result); fflush(log); }
    mv_frame_telemetry_flush(&timing, 1);
    mv_gx_replay_close(&replay);
    mv_render_fini();
    mv_scene_vita_objects_close();
    mv_css_vita_release();
    mv_css_vita_set_log(NULL);

    if (result) return result;
    if (css->pending_scene_change == CSSPendingSceneChange_2) return 1;
    if (route) {
        GameModeState sss_state = {0};
        sss_state.info.scene_kind = GS_SSS;
        sss_state.info.enter_data = &gmVsMelee_SssData;
        sss_state.info.exit_data = &gmVsMelee_SssData;
        route->sss_enter(&sss_state);
        if (log) { fprintf(log, "GAME_SSS_PREPARED mode=%d source=original_mode_callback\n", mode); fflush(log); }
        return -80; /* Replaced by the native stage scene once its assets load. */
    }
    if (ckind == ChKind_None) return -4;

    GameModeState prep_state = mode == GM_CLASSIC ? gm_Mode_Classic_States[0]
                                                   : gm_Mode_Adventure_States[0];
    gm_SetGameModeStateId(prep_state.id);
    if (prep_state.on_enter)
        prep_state.on_enter(&prep_state);
    if (log) {
        fprintf(log,
                "GAME_1P_PREP_STATE_READY mode=%d state=%u scene=%u source=original_state_table\n",
                mode, (unsigned)prep_state.id, (unsigned)prep_state.info.scene_kind);
        fflush(log);
    }

    GameModeState match_state = mode == GM_CLASSIC ? gm_Mode_Classic_States[1]
                                                    : gm_Mode_Adventure_States[1];
    gm_SetGameModeStateId(match_state.id);
    if (!match_state.on_enter)
        return -5;
    match_state.on_enter(&match_state);

    if (log) {
        fprintf(log,
                "GAME_1P_STAGE_MATCH_READY mode=%d scene=GS_VS state=%u stkind=%u "
                "p0_kind=%d p0_slot=%u p0_color=%u p0_stocks=%u "
                "p1_kind=%d p1_slot=%u p1_color=%u p1_stocks=%u "
                "source=original_match_state_onenter\n",
                mode, (unsigned)match_state.id,
                (unsigned)gmClassic_80472AF8.rules.stkind,
                gmClassic_80472AF8.players[0].ckind,
                (unsigned)gmClassic_80472AF8.players[0].slot_type,
                (unsigned)gmClassic_80472AF8.players[0].color,
                (unsigned)gmClassic_80472AF8.players[0].stocks,
                gmClassic_80472AF8.players[1].ckind,
                (unsigned)gmClassic_80472AF8.players[1].slot_type,
                (unsigned)gmClassic_80472AF8.players[1].color,
                (unsigned)gmClassic_80472AF8.players[1].stocks);
        fflush(log);
    }
    return 0;
}
