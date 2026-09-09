#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "pad_vita.h"
#include "asset_viewer.h"
#include "asset_file.h"
#include "gc_runtime_vita.h"
#include "gx_boot_vita.h"
#include "audio_boot_vita.h"
#include "hsd_runtime_probe.h"
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/initialize.h>

_Static_assert(sizeof(void*) == 4, "GameCube data needs 32-bit pointers");
_Static_assert(sizeof(u32) == 4 && sizeof(s32) == 4, "Dolphin ABI");
_Static_assert(sizeof(f32) == 4, "Dolphin float ABI");

/* This is a platform probe, not the game entry point. */
int main(void)
{
    sceIoMkdir("ux0:data/SmashMeleeVita", 0777);
    FILE* log = fopen("ux0:data/SmashMeleeVita/runtime.log", "w");
    if (log) { fprintf(log, "MELEE_VITA_GS_MEMCARD v2.6\n"); fflush(log); }
    mv_runtime_set_log(log);
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
    uint32_t gm_mode_boot[MV_GM_BOOT_SMOKE_STAT_COUNT] = {0};
    int gm_mode_boot_result = services_result ? -100 : gm_VitaBootStateProbe(gm_mode_boot);
    if (log) {
        if (!gm_mode_boot_result) {
            fprintf(log,
                    "GM_BOOT_ENTER_PASS source=gmboot.c callback=bootOnLoad mode=%02lx "
                    "scene=%02lx next_mode=%02lx skip_intro=%lu "
                    "stop=before_GS_MEMCARD_on_enter\n",
                    (unsigned long)gm_mode_boot[MV_GM_BOOT_SMOKE_MODE],
                    (unsigned long)gm_mode_boot[MV_GM_BOOT_SMOKE_SCENE],
                    (unsigned long)gm_mode_boot[MV_GM_BOOT_SMOKE_NEXT_MODE],
                    (unsigned long)gm_mode_boot[MV_GM_BOOT_SMOKE_SKIP_INTRO]);
        } else {
            fprintf(log, "GM_BOOT_ENTER_FAIL code=%d\n", gm_mode_boot_result);
        }
        fflush(log);
    }
    uint32_t memcard_enter[8] = {0};
    int memcard_enter_result = gm_mode_boot_result ? -100 :
                               gm_VitaMemCardSceneEnterProbe(memcard_enter);
    if (log) {
        if (!memcard_enter_result) {
            fprintf(log,
                    "GS_MEMCARD_ENTER_PASS source=gm_1AED.c stages=%lu "
                    "assets=LbMcGame,NtMemAc,NtMsgWin,SdMsgBox "
                    "gobjs=%lu projection=%lu viewport=%lux%lu heap_free=%lu state=%lu next=%lu "
                    "stop=before_GS_MEMCARD_on_frame\n",
                    (unsigned long)memcard_enter[0], (unsigned long)memcard_enter[1],
                    (unsigned long)memcard_enter[2], (unsigned long)memcard_enter[3],
                    (unsigned long)memcard_enter[4], (unsigned long)memcard_enter[5],
                    (unsigned long)memcard_enter[6], (unsigned long)memcard_enter[7]);
        } else {
            fprintf(log, "GS_MEMCARD_ENTER_FAIL code=%d stage=%08lx\n",
                    memcard_enter_result, (unsigned long)memcard_enter[0]);
        }
        fflush(log);
    }
    if (hsd_init_result || gm_post_result || gx_misc_result || audio_result || ax_result ||
        video_result || post_audio_result || services_result || gm_mode_boot_result ||
        memcard_enter_result) {
        if (log) { fprintf(log, "BOOT_STOP initialization_failed\n"); fflush(log); }
        mv_runtime_set_log(NULL);
        if (log) fclose(log);
        return sceKernelExitProcess(1);
    }
    MvHsdArchiveStats archive_stats = {0};
    unsigned char *archive_copy = NULL;
    size_t archive_copy_size = 0;
    int archive_result = mv_read_file("ux0:data/SmashMeleeVita/files/MnMaAll.usd",
                                      &archive_copy, &archive_copy_size);
    if (!archive_result) {
        archive_result = mv_hsd_archive_probe(archive_copy, archive_copy_size,
                                              "MenMainBack_Top_joint", &archive_stats);
    }
    free(archive_copy);
    if (log) {
        if (!archive_result) {
            fprintf(log,
                "HSD_ARCHIVE_UPSTREAM_PASS parser=HSD_ArchiveParse file=%lu data=%lu relocations=%lu "
                "publics=%lu externs=%lu root=MenMainBack_Top_joint root_offset=%08lx\n",
                (unsigned long)archive_stats.file_size,
                (unsigned long)archive_stats.data_size,
                (unsigned long)archive_stats.relocations,
                (unsigned long)archive_stats.publics,
                (unsigned long)archive_stats.externs,
                (unsigned long)archive_stats.public_offset);
        } else {
            fprintf(log, "HSD_ARCHIVE_UPSTREAM_FAIL code=%d\n", archive_result);
        }
        fflush(log);
    }
    uint32_t lbfile_stats[MV_LBFILE_STAT_COUNT] = {0};
    int lbfile_result = mv_lbfile_boot_probe(lbfile_stats);
    if (log) {
        if (!lbfile_result) {
            fprintf(log,
                "LBFILE_DVD_PASS source=lbfile.c file=MnMaAll.usd size=%lu header=%lu fnv1a=%08lx "
                "backend=VitaSyncDVD\n",
                (unsigned long)lbfile_stats[MV_LBFILE_SIZE],
                (unsigned long)lbfile_stats[MV_LBFILE_HEADER_SIZE],
                (unsigned long)lbfile_stats[MV_LBFILE_CHECKSUM]);
        } else {
            fprintf(log, "LBFILE_DVD_FAIL code=%d file=MnMaAll.usd\n", lbfile_result);
        }
        fflush(log);
    }
    uint32_t lbarchive_stats[MV_LBARCHIVE_STAT_COUNT] = {0};
    int lbarchive_result = mv_lbarchive_boot_probe(lbarchive_stats);
    if (log) {
        if (!lbarchive_result) {
            fprintf(log,
                "LBARCHIVE_DAT_PASS source=lbarchive.c file=MnMaAll.usd size=%lu publics=%lu "
                "relocations=%lu externs=%lu root=MenMainBack_Top_joint root_offset=%08lx\n",
                (unsigned long)lbarchive_stats[MV_LBARCHIVE_FILE_SIZE],
                (unsigned long)lbarchive_stats[MV_LBARCHIVE_PUBLICS],
                (unsigned long)lbarchive_stats[MV_LBARCHIVE_RELOCATIONS],
                (unsigned long)lbarchive_stats[MV_LBARCHIVE_EXTERNS],
                (unsigned long)lbarchive_stats[MV_LBARCHIVE_ROOT_OFFSET]);
        } else {
            fprintf(log, "LBARCHIVE_DAT_FAIL code=%d file=MnMaAll.usd\n", lbarchive_result);
        }
        fflush(log);
    }
    if (vita2d_init() < 0) {
        if (log) { fprintf(log, "VIDEO_INIT_FAIL\n"); fclose(log); }
        return sceKernelExitProcess(1);
    }
    vita2d_pgf* font = vita2d_load_default_pgf();
    if (!font) {
        if (log) { fprintf(log, "FONT_INIT_FAIL\n"); fclose(log); }
        vita2d_fini();
        return sceKernelExitProcess(1);
    }
    vita2d_set_clear_color(RGBA8(16, 22, 32, 255));
    MvViewer viewer;
    mv_viewer_init(&viewer, log);
    int diagnostics = 0;
    unsigned previous = 0, frames = 0;
    for (;;) {
        SceCtrlData pad = {0};
        int count = sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = count > 0 ? pad.buttons & ~previous : 0;
        if (pressed & SCE_CTRL_TRIANGLE) diagnostics = !diagnostics;
        if (pressed & SCE_CTRL_SQUARE) mv_viewer_toggle_scene(&viewer);
        if (pressed & SCE_CTRL_RTRIGGER) mv_viewer_page(&viewer, 1);
        if (pressed & SCE_CTRL_LTRIGGER) mv_viewer_page(&viewer, -1);
        if (count > 0 && pad.buttons != previous) {
            if (log) { fprintf(log, "PAD buttons=%08lx lx=%u ly=%u rx=%u ry=%u\n",
                (unsigned long)pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry); fflush(log); }
            previous = pad.buttons;
        }
        /* PS+START is the Vita screenshot shortcut. Requiring SELECT as well
           keeps hardware screenshots possible while retaining an explicit exit. */
        if (count > 0 &&
            (pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
                (SCE_CTRL_SELECT | SCE_CTRL_START)) break;
        PADStatus gc[PAD_MAX_CONTROLLERS];
        PADRead(gc);
        PADClamp(gc);
        vita2d_start_drawing();
        vita2d_clear_screen();
        if (!diagnostics) {
            mv_viewer_draw(&viewer, font);
        } else {
        const unsigned white = RGBA8(235, 240, 245, 255);
        vita2d_pgf_draw_text(font, 40, 65, white, 1.5f, "Smash Melee Vita - bootstrap");
        vita2d_pgf_draw_text(font, 40, 120, white, 1.0f, "Experimental port of doldecomp/melee. No gameplay yet.");
        vita2d_pgf_draw_textf(font, 40, 175, white, 1.0f, "Upstream HSD_Rand: %s", passed ? "PASS" : "FAIL");
        vita2d_pgf_draw_textf(font, 40, 230, white, 1.0f, "Input: %08lx   Left: %u,%u   Right: %u,%u",
            (unsigned long)pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry);
        vita2d_pgf_draw_textf(font, 40, 285, white, 1.0f, "Frames: %u   Log: %s", frames, log ? "open" : "unavailable");
        vita2d_pgf_draw_text(font, 40, 340, white, 1.0f, "SELECT+START: exit (PS+START screenshot safe)");
        vita2d_pgf_draw_textf(font, 40, 395, white, 1.0f,
            "PAD bridge: %s   GC buttons: %04x   Stick: %d,%d   C: %d,%d",
            pad_passed ? "PASS" : "FAIL", gc[0].button, gc[0].stickX, gc[0].stickY,
            gc[0].substickX, gc[0].substickY);
        vita2d_pgf_draw_textf(font, 40, 450, white, 0.9f,
            "gmmain: %s   HSD: %s   Audio prefix: %s   Arena=%lu KiB",
            gm_boot_result == 0 ? "PASS" : "FAIL",
            hsd_init_result == 0 ? "PASS" : "FAIL",
            audio_result == 0 ? "PASS" : "FAIL",
            (unsigned long)(gm_boot[MV_GM_BOOT_ARENA_BYTES] / 1024u));
        vita2d_pgf_draw_textf(font, 40, 485, white, 0.8f,
            "Original lbFile DVD read: %s   %lu bytes",
            lbfile_result == 0 ? "PASS" : "FAIL",
            (unsigned long)lbfile_stats[MV_LBFILE_SIZE]);
        vita2d_pgf_draw_textf(font, 520, 485, white, 0.8f,
            "lbArchive: %s", lbarchive_result == 0 ? "PASS" : "FAIL");
        }
        vita2d_end_drawing();
        vita2d_swap_buffers();
        if (++frames == 120 && log) { fprintf(log, "PRESENT_120\n"); fflush(log); }
    }
    vita2d_wait_rendering_done();
    mv_viewer_close(&viewer);
    vita2d_free_pgf(font);
    vita2d_fini();
    if (log) { fprintf(log, "EXIT frames=%u\n", frames); mv_runtime_set_log(NULL); fclose(log); }
    return sceKernelExitProcess(0);
}
