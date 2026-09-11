#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdint.h>
#include "pad_vita.h"
#include "render_vita.h"
#include "gc_runtime_vita.h"
#include "gx_boot_vita.h"
#include "audio_boot_vita.h"
#include "title_boot_vita.h"
#include "mth_player_vita.h"
#include "menu_boot_vita.h"
#include "onep_boot_vita.h"
#include <melee/gm/forward.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/initialize.h>

_Static_assert(sizeof(void*) == 4, "GameCube data needs 32-bit pointers");
_Static_assert(sizeof(u32) == 4 && sizeof(s32) == 4, "Dolphin ABI");
_Static_assert(sizeof(f32) == 4, "Dolphin float ABI");

/* Partial scene integration; original assets do not imply a complete game loop. */
int main(void)
{
    sceIoMkdir("ux0:data/SmashMeleeVita", 0777);
    FILE* log = fopen("ux0:data/SmashMeleeVita/runtime.log", "w");
    if (log) { fprintf(log, "MELEE_VITA_GAME_BOOT v3.30\n"); fflush(log); }
    if (log) { fprintf(log, "MELEE_VITA_UPSTREAM_BASE 480b04454\n"); fflush(log); }
    if(log) { fprintf(log,"RENDER_BACKEND name=" MV_RENDER_NAME " scene_loop=partial menu=mnMain_native\n"); fflush(log); }
    mv_runtime_set_log(log);

    /* vitaGL owns the one GXM context for movie -> title -> menu.  Initialize
     * it before HSD/ARAM/audio reserve most of the process memory; all later
     * mv_render_init() calls are intentionally idempotent. */
    SceIoStat shacccg_stat = {0};
    int shacccg_result = sceIoGetstat("ur0:data/libshacccg.suprx", &shacccg_stat);
    if (log) {
        fprintf(log,
                "VITAGL_INIT_BEGIN shader_compiler=ur0:data/libshacccg.suprx "
                "shader_stat=%08x legacy_pool=%u ram_threshold=%u\n",
                (unsigned) shacccg_result, 4U * 1024U * 1024U,
                16U * 1024U * 1024U);
        fflush(log);
    }
    int render_result = mv_render_init();
    if (render_result < 0) {
        if (log) {
            fprintf(log, "VITAGL_INIT_FAIL code=%d shader_stat=%08x\n",
                    render_result, (unsigned) shacccg_result);
            fflush(log);
        }
        mv_runtime_set_log(NULL);
        if (log)
            fclose(log);
        return sceKernelExitProcess(2);
    }
    if (log) {
        fprintf(log, "VITAGL_INIT_PASS context=global owner=movie+title+menu\n");
        fflush(log);
    }
    /* Independent known-answer vectors for upstream HSD_Rand, seed = 1. */
    const u32 expected[] = {41, 51235, 6334, 59268, 51937};
    int passed = 1;
    *seed_ptr = 1;
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        u32 value = (u32)HSD_Rand();
        if (value != expected[i]) passed = 0;
        if (log) fprintf(log, "HSD_RAND[%u]=%lu expected=%lu\n", i, value, expected[i]);
    }
    if (log) { fprintf(log, "HSD_RAND_%s\n", passed ? "PASS" : "FAIL"); fflush(log); }
    int pad_passed = melee_vita_pad_selftest();
    if (log) { fprintf(log, "PAD_MAP_%s\n", pad_passed ? "PASS" : "FAIL"); fflush(log); }
    uint32_t gm_boot[MV_GM_BOOT_STAT_COUNT] = {0};
    int gm_boot_result = gmMain_VitaBootProbe(gm_boot);
    if (log) {
        if (!gm_boot_result) {
            fprintf(log,
                "GMMAIN_BOOT_PREFIX_PASS source=gmmain.c pad=%08lx db_level=%lu develop=%lu "
                "arena=%lu seed=%lu sim_mem=%lu stop=before_HSD_GX_init\n",
                (unsigned long)gm_boot[MV_GM_BOOT_LAUNCH_PAD],
                (unsigned long)gm_boot[MV_GM_BOOT_DB_LEVEL],
                (unsigned long)gm_boot[MV_GM_BOOT_DEVELOP_PRESENT],
                (unsigned long)gm_boot[MV_GM_BOOT_ARENA_BYTES],
                (unsigned long)gm_boot[MV_GM_BOOT_SEED_TICK],
                (unsigned long)gm_boot[MV_GM_BOOT_SIM_MEM_BYTES]);
        } else {
            fprintf(log, "GMMAIN_BOOT_PREFIX_FAIL code=%d\n", gm_boot_result);
            PADInit();
        }
        fflush(log);
    }
    u32 hsd_init_stats[6] = {0};
    int hsd_init_result = gm_boot_result ? -100 : HSD_InitComponentVitaProbe(hsd_init_stats);
    if (log) {
        if (!hsd_init_result) {
            fprintf(log,
                "HSD_COMPONENT_INIT_PASS source=initialize.c main_heap=%lu main_free=%lu "
                "audio_free=%lu next_arena=%lu physical=%lu stages=%08lx "
                "stop=before_gmmain_GXSetMisc\n",
                (unsigned long)hsd_init_stats[0],
                (unsigned long)hsd_init_stats[1],
                (unsigned long)hsd_init_stats[2],
                (unsigned long)hsd_init_stats[3],
                (unsigned long)hsd_init_stats[4],
                (unsigned long)hsd_init_stats[5]);
        } else {
            fprintf(log, "HSD_COMPONENT_INIT_FAIL code=%d stop=before_gmmain_GXSetMisc\n",
                    hsd_init_result);
        }
        fflush(log);
    }
    uint32_t gm_post[MV_GM_POST_STAT_COUNT] = {0};
    int gm_post_result = hsd_init_result ? -100 : gmMain_VitaPostHsdProbe(gm_post);
    if (log) {
        if (!gm_post_result) {
            fprintf(log,
                "GMMAIN_POST_HSD_PASS source=gmmain.c gx=GXSetMisc_XF_FLUSH seed=%lu "
                "audio=lbAudioAx_8002838C_AXDriver_HSD_Synth stop=before_audio_aux_banks\n",
                (unsigned long)gm_post[MV_GM_POST_SEED_TICK]);
        } else {
            fprintf(log, "GMMAIN_POST_HSD_FAIL code=%d stage=AXDriver_HSD_Synth\n",
                    gm_post_result);
        }
        fflush(log);
    }
    uint32_t gx_misc_stats[2] = {0};
    int gx_misc_result = gm_post_result ? -100 : mv_gx_misc_validate(gx_misc_stats);
    if (log) {
        if (!gx_misc_result) {
            fprintf(log, "GX_MISC_PASS xf_flush=%lu dl_save_context=%lu backend=VitaState\n",
                    (unsigned long)gx_misc_stats[0], (unsigned long)gx_misc_stats[1]);
        } else {
            fprintf(log, "GX_MISC_FAIL code=%d\n", gx_misc_result);
        }
        fflush(log);
    }
    uint32_t audio_stats[MV_AUDIO_BOOT_STAT_COUNT] = {0};
    int audio_result = gm_post_result ? -100 : mv_audio_boot_validate(audio_stats);
    if (log) {
        if (!audio_result) {
            fprintf(log,
                "AUDIO_PREFIX_PASS source=lbaudio_ax.c ar_base=%08lx ar_size=%lu "
                "arq_chunk=%lu ai_rate_code=%lu bank_base=%lu bank_common=%lu "
                "bank_priority=%lu bank_total=%lu aram_dma=roundtrip_pass "
                "output=not_started stop=before_audio_aux_banks\n",
                (unsigned long)audio_stats[MV_AUDIO_AR_BASE],
                (unsigned long)audio_stats[MV_AUDIO_AR_SIZE],
                (unsigned long)audio_stats[MV_AUDIO_ARQ_CHUNK],
                (unsigned long)audio_stats[MV_AUDIO_AI_RATE],
                (unsigned long)audio_stats[MV_AUDIO_BANK_BASE],
                (unsigned long)audio_stats[MV_AUDIO_BANK_COMMON],
                (unsigned long)audio_stats[MV_AUDIO_BANK_PRIORITY],
                (unsigned long)audio_stats[MV_AUDIO_BANK_TOTAL]);
        } else {
            fprintf(log, "AUDIO_PREFIX_FAIL code=%d stage=AXDriver_HSD_Synth\n",
                    audio_result);
        }
        fflush(log);
    }
    uint32_t ax_stats[6] = {0};
    int ax_result = audio_result ? -100 : mv_ax_boot_validate(ax_stats);
    if (log) {
        if (!ax_result) {
            fprintf(log,
                    "AX_SYNTH_INIT_PASS source=axdriver.c/synth.c voices=%lu allocated=%lu "
                    "aux_a=%lu aux_b=%lu mode=%lu max_dsp_cycles=%lu "
                    "output=not_started stop=before_audio_aux_banks\n",
                    (unsigned long)ax_stats[0], (unsigned long)ax_stats[1],
                    (unsigned long)ax_stats[2], (unsigned long)ax_stats[3],
                    (unsigned long)ax_stats[4], (unsigned long)ax_stats[5]);
        } else {
            fprintf(log, "AX_SYNTH_INIT_FAIL code=%d\n", ax_result);
        }
        fflush(log);
    }
    uint32_t video_stats[6] = {0};
    int video_result = (hsd_init_result || gm_post_result || gx_misc_result || audio_result || ax_result) ?
                           -100 : mv_vi_boot_validate(video_stats);
    if (log) {
        fprintf(log, "VI_GX_BOOT_%s code=%d width=%lu height=%lu retraces=%lu copies=%lu flushes=%lu lights=%02lx copy=black_boot_only\n",
                video_result ? "FAIL" : "PASS", video_result,
                (unsigned long)video_stats[0], (unsigned long)video_stats[1],
                (unsigned long)video_stats[2], (unsigned long)video_stats[3],
                (unsigned long)video_stats[4], (unsigned long)video_stats[5]);
        fflush(log);
    }
    uint32_t post_audio[1] = {0};
    int post_audio_result = video_result ? -100 : gmMain_VitaPostAudioProbe(post_audio);
    if (log) {
        if (!post_audio_result) {
            fprintf(log,
                    "GMMAIN_POST_AUDIO_PASS source=gmmain.c stages=%08lx "
                    "controller=1 retrace=1 vi_visible=1 memory=1 heap=1 dvd=1 arq=1 "
                    "stop=before_card_snapshot_gmMainLib\n",
                    (unsigned long)post_audio[0]);
        } else {
            fprintf(log, "GMMAIN_POST_AUDIO_FAIL code=%d stages=%08lx\n",
                    post_audio_result, (unsigned long)post_audio[0]);
        }
        fflush(log);
    }
    uint32_t services[1] = {0};
    int services_result = post_audio_result ? -100 : gmMain_VitaServicesProbe(services);
    if (log) {
        if (!services_result) {
            fprintf(log,
                    "GMMAIN_SERVICES_PASS source=gmmain.c stages=%08lx "
                    "cardnew=1 cardgame=1 snapshot=1 mainlib=1 mthp=1 sislib=1 "
                    "stop=before_gmMainLib_final_audio_bank_init\n",
                    (unsigned long)services[0]);
        } else {
            fprintf(log, "GMMAIN_SERVICES_FAIL code=%d stages=%08lx\n",
                    services_result, (unsigned long)services[0]);
        }
        fflush(log);
    }
    uint32_t final_init[1] = {0};
    int final_init_result = services_result ? -100 : gmMain_VitaFinalInitProbe(final_init);
    if (log) {
        if (!final_init_result) {
            fprintf(log,
                    "GMMAIN_FINAL_INIT_PASS source=gmmain.c mainlib_reset=%lu "
                    "next=GM_TITLE_original_scene\n",
                    (unsigned long)final_init[0]);
        } else {
            fprintf(log, "GMMAIN_FINAL_INIT_FAIL code=%d stage=gmMainLib_8015FBA4\n",
                    final_init_result);
        }
        fflush(log);
    }

    if (hsd_init_result || gm_post_result || gx_misc_result || audio_result || ax_result ||
        video_result || post_audio_result || services_result || final_init_result) {
        if (log) {
            fprintf(log, "GAME_BOOT_STOP initialization_failed\n");
            fflush(log);
        }
        mv_runtime_set_log(NULL);
        if (log)
            fclose(log);
        return sceKernelExitProcess(1);
    }


    if (log) {
        fprintf(log,
                "GAME_BOOT_ENTER mode=GM_OPENING_MV scene=GS_MOVIE_OPENING "
                "asset=MvOpen.mth diagnostic_ui=disabled\n");
        fflush(log);
    }
    int opening_result = mv_opening_movie_run(log);
    if (opening_result == 2) {
        mv_runtime_set_log(NULL);
        if (log) fclose(log);
        return sceKernelExitProcess(0);
    }
    if (opening_result < 0 && log) {
        fprintf(log, "GAME_OPENING_FALLBACK_TO_TITLE code=%d\n", opening_result);
        fflush(log);
    }

    if (log) {
        fprintf(log,
                "GAME_BOOT_ENTER mode=GM_TITLE scene=GS_TITLE source=gmtitle.c "
                "visible=original_GmTtAll diagnostic_ui=disabled\n");
        fflush(log);
    }
    int title_result = 0, menu_result = 0, onep_result = 0;
    int pending_mode = GM_TITLE;
    for (;;) {
        if (pending_mode == GM_TITLE) {
            title_result = mv_title_boot_run(log);
            if (title_result != 1) break;
            mv_menu_vita_reset_return();
            pending_mode = GM_MENU;
        } else if (pending_mode == GM_MENU) {
            menu_result = mv_main_menu_run(log);
            if (menu_result != 0 || !mv_scene_vita_done()) break;
            pending_mode = mv_scene_vita_pending_mode();
            if (pending_mode < 0 || pending_mode >= GM_COUNT) break;
        } else {
            int completed_mode = pending_mode;
            onep_result = mv_onep_mode_run(log, completed_mode);
            if (onep_result == -99) break;
            if (onep_result < 0) {
                if (log) {
                    fprintf(log, "GAME_MODE_RETURN mode=%d result=%d destination=ERROR\n",
                            completed_mode, onep_result);
                    fflush(log);
                }
                break;
            }
#ifdef MELEE_VITA_FULL_GAMEPLAY_SCENE
            if (completed_mode == GM_CLASSIC || completed_mode == GM_ADVENTURE)
                pending_mode = onep_result;
            else
                pending_mode = GM_MENU;
#else
            pending_mode = GM_MENU;
#endif
            if (log) {
                fprintf(log, "GAME_MODE_RETURN mode=%d result=%d destination=%d\n",
                        completed_mode, onep_result, pending_mode);
                fflush(log);
            }
        }
    }
    if (log) {
        fprintf(log,
                "GAME_BOOT_RETURN opening=%d title=%d menu=%d pending_mode=%d onep=%d\n",
                opening_result, title_result, menu_result, pending_mode, onep_result);
        fflush(log);
    }
    mv_runtime_set_log(NULL);
    if (log) fclose(log);
    return sceKernelExitProcess((title_result < 0 || menu_result < 0 || onep_result < 0) ? 1 : 0);
}
