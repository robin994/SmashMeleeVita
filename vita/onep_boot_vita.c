#include "onep_boot_vita.h"
#include "mode_route_vita.h"
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/gmvs.h>
#include "css_assets_vita.h"
#include "sss_assets_vita.h"
#include "frame_telemetry_vita.h"
#include "gc_runtime_vita.h"
#include "gx_capture_vita.h"
#include "gx_replay_vita.h"
#include "hsd_runtime_probe.h"
#include "render_vita.h"
#include "retail_runtime_vita.h"
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
#include <melee/mn/mnstagesel.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/sislib.h>
#include <psp2/ctrl.h>
#include <stdint.h>
#include <string.h>

extern CSSData gmClassic_80470708;
extern StartMeleeData gmClassic_80472AF8;
extern MatchExitInfo gmClassic_8047086C;
extern GameModeState gm_Mode_Classic_States[];

static int capture_live_sss(MvGxReplay *replay, MvGxCaptureStats *stats,
                            const MvCamera *camera, FILE *log, int initialize)
{
    unsigned roots = 0;
    memset(stats, 0, sizeof(*stats));
    for (unsigned p = 0; p <= HSD_GObjLibInitData.p_link_max; ++p) {
        HSD_GObj *gobj = HSD_GObjPLinkHead[p];
        for (; gobj; gobj = gobj->next) {
            if (gobj->obj_kind != HSD_GObj_JObjKind || !gobj->hsd_obj) continue;
            int r = mv_hsd_gx_capture_runtime((HSD_JObj *)gobj->hsd_obj,
                                              roots == 0, initialize ? 0 : 1, stats);
            if (r) {
                if (log) {
                    fprintf(log,
                            "GAME_SSS_CAPTURE_ROOT_FAIL plink=%u gxlink=%u root=%u code=%d\n",
                            p, gobj->gx_link, roots, r);
                    fflush(log);
                }
                return -10 - r;
            }
            ++roots;
        }
    }
    if (!roots) return -2;
    int sis_count = HSD_SisLib_VitaCaptureAll();
    if (mv_gx_capture_stats(stats)) return -4;
    if (initialize) {
        int r = mv_gx_replay_init_relaxed_from(replay, camera, log, 0);
        if (r) return -3;
            if (log) {
            fprintf(log,
                    "GAME_SSS_CAPTURE_INIT_PASS roots=%u sis=%d commands=%u triangles=%u "
                    "capture_sig=%08x renderer=%s\n",
                    roots, sis_count, stats->commands, stats->triangles,
                    (unsigned)mv_gx_capture_frame_signature(), MV_RENDER_NAME);
            fflush(log);
        }
    } else {
        replay->relaxed_from_command = 0;
    }
    return 0;
}

static int run_route_sss(FILE *log, const MvModeRoute *route, int mode)
{
    if (!route || !route->sss_enter || !route->sss_exit || !route->sss_data)
        return -60;
    if (mv_render_init() < 0) return -61;

    lbDvd_80018CF4(lbDvdPreload_3);
    mv_scene_vita_objects_init();
    gm_801A3E88();
    mv_scene_vita_reset();
    mv_sss_vita_set_log(log);

    GameModeState sss_state = {0};
    sss_state.id = route->sss_state_id;
    sss_state.info.scene_kind = GS_SSS;
    sss_state.info.enter_data = route->sss_data;
    sss_state.info.exit_data = route->sss_data;
    gm_SetGameModeStateId(sss_state.id);
    route->sss_enter(&sss_state);

    if (log) {
        fprintf(log,
                "GAME_SSS_ENTER mode=%s(%d) state=%u source=original_mode_callback+mnStageSel\n",
                route->name, mode, (unsigned)sss_state.id);
        fflush(log);
    }
    mnStageSel_Scene_OnEnter(route->sss_data);
    const MvCamera *camera = mv_sss_vita_camera();
    if (!camera) {
        mv_scene_vita_objects_close();
        mv_render_fini();
        mv_sss_vita_release();
        mv_sss_vita_set_log(NULL);
        return -62;
    }

    MvGxReplay replay;
    memset(&replay, 0, sizeof(replay));
    MvGxCaptureStats capture = {0};
    int result = capture_live_sss(&replay, &capture, camera, log, 1);
    if (result) {
        mv_scene_vita_objects_close();
        mv_render_fini();
        mv_sss_vita_release();
        mv_sss_vita_set_log(NULL);
        return -63 + result;
    }

    MvFrameTelemetry timing;
    mv_frame_telemetry_init(&timing, log, "STAGE_SELECT");
    unsigned frames = 0;
    while (!mv_scene_vita_done()) {
        uint64_t frame0 = mv_frame_time_us();
        HSD_PadRenewStatus();
        gm_EvaluateAllControllerInputs();
        mnStageSel_Scene_OnFrame();
        HSD_GObj_RunProcs();
        if (mv_scene_vita_done()) break;

        uint64_t cap0 = mv_frame_time_us();
        result = capture_live_sss(&replay, &capture, camera, log, 0);
        uint64_t cap1 = mv_frame_time_us();
        if (result) break;
        uint64_t replay0 = mv_frame_time_us();
        mv_render_begin();
        mv_gx_replay_draw(&replay, camera);
        uint64_t replay1 = mv_frame_time_us();
        mv_render_present();
        uint64_t present1 = mv_frame_time_us();
        ++frames;
        mv_frame_telemetry_record(&timing, present1 - frame0, cap1 - cap0,
                                  replay1 - replay0, present1 - replay1);
        mv_frame_telemetry_flush(&timing, 0);
        SceCtrlData pad = {0};
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
            (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
                (SCE_CTRL_SELECT | SCE_CTRL_START)) {
            result = -99;
            break;
        }
    }

    mnStageSel_Scene_OnExit(NULL);
    route->sss_exit(&sss_state);
    SSSData *sss = (SSSData *)route->sss_data;
    if (log) {
        fprintf(log,
                "GAME_SSS_EXIT mode=%s(%d) frames=%u start_game=%u stkind=%u result=%d state_exit=original\n",
                route->name, mode, frames, (unsigned)sss->start_game,
                (unsigned)sss->vs.start.rules.stkind, result);
        fflush(log);
    }
    mv_frame_telemetry_flush(&timing, 1);
    mv_gx_replay_close(&replay);
    mv_render_fini();
    mv_scene_vita_objects_close();
    mv_sss_vita_release();
    mv_sss_vita_set_log(NULL);

    if (result) return result;
    /* The original SSS state-exit callback encodes both directions in the
     * GameMode state machine: start_game advances to the match; Back sets the
     * next state to CSS. In both cases the state machine must keep running. */
    return 0;
}

static int capture_live_css(MvGxReplay *replay, MvGxCaptureStats *stats,
                            const MvCamera *camera, FILE *log, int initialize)
{
    unsigned roots = 0;
    memset(stats, 0, sizeof(*stats));
    for (unsigned p = 0; p <= HSD_GObjLibInitData.p_link_max; ++p) {
        HSD_GObj *gobj = HSD_GObjPLinkHead[p];
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
    int sis_count = HSD_SisLib_VitaCaptureAll();
    if (mv_gx_capture_stats(stats)) return -4;
    if (initialize) {
        int r = mv_gx_replay_init_relaxed_from(replay, camera, log, 0);
        if (r) return -3;
        replay->relaxed_from_command = 0;
        if (log) { fprintf(log, "GAME_CSS_CAPTURE_INIT_PASS roots=%u sis=%d commands=%u triangles=%u capture_sig=%08x renderer=%s\n", roots, sis_count, stats->commands, stats->triangles, (unsigned)mv_gx_capture_frame_signature(), MV_RENDER_NAME); fflush(log); }
    } else replay->relaxed_from_command = 0;
    return 0;
}

int mv_onep_mode_run(FILE *log, int mode)
{
    const MvModeRoute *route = mv_mode_vita_route(mode);
    if (mv_render_init() < 0) return -2;
    mv_gm_vita_enter_mode(mode);

    const char *mode_name = route ? route->name :
                            mode == GM_CLASSIC ? "GM_CLASSIC" :
                            mode == GM_ADVENTURE ? "GM_ADVENTURE" :
                            "GM_RETAIL";
    if (log) {
        fprintf(log, "GAME_MODE_ROUTE_BEGIN mode=%s(%d) target=%s\n",
                mode_name, mode,
                (mode == GM_CLASSIC || mode == GM_ADVENTURE || route) ?
                    "CSS->STAGE_MATCH" : "original_GameMode_table");
        fflush(log);
    }

    /* Finish already-issued DVD/ARAM completions from the outgoing menu after
     * its HPS stream has been stopped. */
    unsigned transition_pumps = 0;
    while (mv_gc_async_pending() && transition_pumps < 256u) {
        mv_gc_async_pump();
        ++transition_pumps;
    }

#ifdef MELEE_VITA_FULL_GAMEPLAY_SCENE
    if (mode != GM_CLASSIC && mode != GM_ADVENTURE && !route) {
        if (log) {
            fprintf(log,
                    "GAME_MODE_RETAIL_DISPATCH mode=%d source=gm_GetAllGameModes+runGameMode\n",
                    mode);
            fflush(log);
        }
        if (mv_retail_runtime_begin(log) != 0)
            return -7;
        int next_mode = mv_gm_vita_run_mode(mode);
        mv_retail_runtime_end();
        if (next_mode < 0 || next_mode >= GM_COUNT)
            return -6;
        return next_mode;
    }
#endif
    if (log) {
        fprintf(log, "GAME_1P_PRELOAD_BEGIN pending_async=%u drained=%u preload=%d\n",
                mv_gc_async_pending(), transition_pumps, lbDvdPreload_3);
        fflush(log);
    }
    lbDvd_80018CF4(lbDvdPreload_3);
    if (log) {
        fprintf(log, "GAME_1P_PRELOAD_PASS pending_async=%u preload=%d\n",
                mv_gc_async_pending(), lbDvdPreload_3);
        fflush(log);
    }
    if (log) { fprintf(log, "GAME_1P_BOOTSTRAP stage=sis_begin\n"); fflush(log); }
    mv_scene_vita_sis_init(0x2400);
    if (log) { fprintf(log, "GAME_1P_BOOTSTRAP stage=objects_begin\n"); fflush(log); }
    mv_scene_vita_objects_init();
    if (log) { fprintf(log, "GAME_1P_BOOTSTRAP stage=controller_begin\n"); fflush(log); }
    gm_801A3E88();
    if (log) { fprintf(log, "GAME_1P_BOOTSTRAP stage=audio_scene_begin\n"); fflush(log); }
    lbAudioAx_8002835C();
    if (log) { fprintf(log, "GAME_1P_BOOTSTRAP stage=rumble_begin\n"); fflush(log); }
    lb_80014534();
    if (log) { fprintf(log, "RUMBLE_DATA_INIT_PASS scene=1P source=lb_013B.c asset=LbRb.dat symbol=lbRumbleData\n"); fflush(log); }
    mv_scene_vita_reset();
    mv_css_vita_set_log(log);

    CSSData *css = route ? (CSSData *)route->css_data : &gmClassic_80470708;
    GameModeState css_state;
    memset(&css_state, 0, sizeof(css_state));
    css_state.id = route ? route->css_state_id : 0x70;
    css_state.info.scene_kind = GS_CSS;
    css_state.info.enter_data = css;
    css_state.info.exit_data = css;

    lbAudioAx_VitaSfxStateTrace("1P_BEGIN");

    if (route) {
#ifndef MELEE_VITA_FULL_GAMEPLAY_SCENE
        static unsigned char initialized[GM_COUNT];
        if (!initialized[mode]) {
            if (log) { fprintf(log, "GAME_1P_MODE_INIT_BEGIN mode=%d\n", mode); fflush(log); }
            route->init(); initialized[mode] = 1;
            if (log) { fprintf(log, "GAME_1P_MODE_INIT_PASS mode=%d\n", mode); fflush(log); }
        }
#endif
        if (log) { fprintf(log, "GAME_1P_MODE_LOAD_BEGIN mode=%d\n", mode); fflush(log); }
        route->load();
        if (log) { fprintf(log, "GAME_1P_MODE_LOAD_PASS mode=%d\n", mode); fflush(log); }
        if (log) { fprintf(log, "GAME_1P_CSS_STATE_ENTER_BEGIN mode=%d\n", mode); fflush(log); }
        route->css_enter(&css_state);
        if (log) { fprintf(log, "GAME_1P_CSS_STATE_ENTER_PASS mode=%d\n", mode); fflush(log); }
    } else if (mode == GM_CLASSIC) {
        if (log) { fprintf(log, "GAME_1P_MODE_LOAD_BEGIN mode=%d\n", mode); fflush(log); }
        gm_Mode_Classic_OnLoad();
        if (log) { fprintf(log, "GAME_1P_MODE_LOAD_PASS mode=%d\n", mode); fflush(log); }
        if (log) { fprintf(log, "GAME_1P_CSS_STATE_ENTER_BEGIN mode=%d\n", mode); fflush(log); }
        gmClassic_801B3DD8(&css_state);
        if (log) { fprintf(log, "GAME_1P_CSS_STATE_ENTER_PASS mode=%d\n", mode); fflush(log); }
    } else {
        gm_Mode_Adventure_OnLoad(); gm_801B42E8(&css_state);
    }
    lbAudioAx_VitaSfxStateTrace("1P_AFTER_MODE_ONLOAD");

    const u8 match_type = css->match_type;
    if (log) {
        HSD_GObj **heads = HSD_GObjPLinkHead;
        fprintf(log, "GAME_1P_CSS_ENTER mode=%d match_type=%u port=%u source=original_mode_onload+css_state_onenter\n",
                mode, match_type, (unsigned)gm_801677F0());
        fprintf(log, "GAME_CSS_GOBJ_HEAP_SYNC generation=%u entities=%p p3=%p low3=%p\n",
                (unsigned)HSD_GetHeapGeneration(), (void *)HSD_GObjPLinkHead,
                heads ? (void *)heads[3] : NULL,
                plinklow_gobjs ? (void *)plinklow_gobjs[3] : NULL);
        fflush(log);
    }
    if (log) { fprintf(log, "GAME_CSS_SCENE_ONENTER_BEGIN mode=%d\n", mode); fflush(log); }
    mnCharSel_Scene_OnEnter(css);
    if (log) { fprintf(log, "GAME_CSS_SCENE_ONENTER_PASS mode=%d\n", mode); fflush(log); }
    const MvCamera *camera = mv_css_vita_camera();
    if (!camera) { mv_render_fini(); mv_css_vita_release(); return -3; }

    MvGxReplay replay; memset(&replay, 0, sizeof(replay));
    MvGxCaptureStats capture = {0};
    int result = capture_live_css(&replay, &capture, camera, log, 1);
    if (result) { mv_render_fini(); mv_css_vita_release(); return -20 + result; }

    MvFrameTelemetry timing;
    mv_frame_telemetry_init(&timing, log,
                            mode == GM_CLASSIC ? "CLASSIC_CSS" :
                            mode == GM_ADVENTURE ? "ADVENTURE_CSS" :
                            route ? route->name : "RETAIL_CSS");
    unsigned frames = 0;
    unsigned css_raw_prev = 0;
    while (!mv_scene_vita_done()) {
        uint64_t frame0 = mv_frame_time_us();
        HSD_PadRenewStatus();
        gm_EvaluateAllControllerInputs();
        mnCharSel_Scene_OnFrame();
        HSD_GObj_RunProcs();
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
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
            unsigned raw_pressed = pad.buttons & ~css_raw_prev;
            css_raw_prev = pad.buttons;
            if ((pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
                (SCE_CTRL_SELECT | SCE_CTRL_START))
            {
                result = -99;
                break;
            }
            if ((raw_pressed & SCE_CTRL_START) &&
                !(pad.buttons & SCE_CTRL_SELECT))
            {
                u32 before = mnCharSel_VitaDebugState();
                u32 hsd_button = HSD_PadCopyStatus[0].button;
                u32 hsd_trigger = HSD_PadCopyStatus[0].trigger;
                int accepted = mnCharSel_VitaTryStart();
                u32 after = mnCharSel_VitaDebugState();
                if (log) {
                    fprintf(log,
                            "GAME_CSS_VITA_START raw=%08x hsd_button=%08x hsd_trigger=%08x state_before=%08x state_after=%08x accepted=%d\n",
                            (unsigned)pad.buttons, (unsigned)hsd_button,
                            (unsigned)hsd_trigger, (unsigned)before,
                            (unsigned)after, accepted);
                    fflush(log);
                }
            }
        }
        if (log && (frames % 120u) == 0u) {
            u32 state = mnCharSel_VitaDebugState();
            fprintf(log,
                    "GAME_CSS_STATE frame=%u state=%08x hsd_button=%08x hsd_trigger=%08x "
                    "pending=%u ckind=%d capture_sig=%08x\n",
                    frames, (unsigned)state,
                    (unsigned)HSD_PadCopyStatus[0].button,
                    (unsigned)HSD_PadCopyStatus[0].trigger,
                    (unsigned)css->pending_scene_change,
                    (int)css->vs.start.players[gm_801677F0()].ckind,
                    (unsigned)mv_gx_capture_frame_signature());
            fflush(log);
        }
    }

    if (log) { fprintf(log, "GAME_CSS_ONEXIT_BEGIN mode=%d\n", mode); fflush(log); }
    mnCharSel_Scene_OnExit(NULL);
    if (log) { fprintf(log, "GAME_CSS_ONEXIT_PASS mode=%d pending=%u\n", mode, (unsigned)css->pending_scene_change); fflush(log); }
    if (log) { fprintf(log, "GAME_1P_CSS_STATE_EXIT_BEGIN mode=%d state=%u\n", mode, (unsigned)gm_GetCurrentSceneIndex()); fflush(log); }
    if (route)
        route->css_exit(&css_state);
    else if (mode == GM_CLASSIC)
        gmClassic_801B3E44(&css_state);
    else
        gm_801B4350(&css_state);
    if (log) { fprintf(log, "GAME_1P_CSS_STATE_EXIT_PASS mode=%d state=%u\n", mode, (unsigned)gm_GetCurrentSceneIndex()); fflush(log); }

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
    if (css->pending_scene_change == CSSPendingSceneChange_2) {
        if (log) {
            fprintf(log,
                    "GAME_CSS_BACK_RETURN mode=%d destination=GM_MENU source=mncharsel_original\n",
                    mode);
            fflush(log);
        }
        return GM_MENU;
    }
#ifdef MELEE_VITA_FULL_GAMEPLAY_SCENE
    if (!route && (mode == GM_CLASSIC || mode == GM_ADVENTURE)) {
        if (log) {
            fprintf(log,
                    "GAME_1P_RETAIL_CONTINUE_BEGIN mode=%s(%d) css_pending=%u state=%u source=gm_1A3F.c\n",
                    mode == GM_CLASSIC ? "CLASSIC" : "ADVENTURE", mode,
                    (unsigned)css->pending_scene_change,
                    (unsigned)gm_GetCurrentSceneIndex());
            fflush(log);
        }
        if (mv_retail_runtime_begin(log) != 0) {
            if (log) {
                fprintf(log, "VITA_RETAIL_RUNTIME_BEGIN_FAIL mode=%d\n", mode);
                fflush(log);
            }
            return -7;
        }
        int next_mode = mv_gm_vita_continue_mode(mode);
        mv_retail_runtime_end();
        if (log) {
            fprintf(log,
                    "GAME_1P_RETAIL_CONTINUE_RETURN mode=%s(%d) next_mode=%d source=gm_1A3F.c\n",
                    mode == GM_CLASSIC ? "CLASSIC" : "ADVENTURE", mode,
                    next_mode);
            fflush(log);
        }
        if (next_mode < 0 || next_mode >= GM_COUNT) return -6;
        return next_mode;
    }
#endif
    if (route) {
        int sss_result = run_route_sss(log, route, mode);
        if (sss_result != 0) return sss_result;
#ifdef MELEE_VITA_FULL_GAMEPLAY_SCENE
        if (log) {
            fprintf(log,
                    "GAME_MODE_RETAIL_CONTINUE_BEGIN mode=%s(%d) state=%u source=original_GameMode_state_machine\n",
                    route->name, mode, (unsigned)gm_GetCurrentSceneIndex());
            fflush(log);
        }
        if (mv_retail_runtime_begin(log) != 0)
            return -7;
        int next_mode = mv_gm_vita_continue_mode(mode);
        mv_retail_runtime_end();
        if (log) {
            fprintf(log,
                    "GAME_MODE_RETAIL_CONTINUE_RETURN mode=%s(%d) next_mode=%d\n",
                    route->name, mode, next_mode);
            fflush(log);
        }
        if (next_mode < 0 || next_mode >= GM_COUNT)
            return -6;
        return next_mode;
#endif
        return 0;
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
