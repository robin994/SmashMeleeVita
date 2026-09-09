#pragma once

#include <stdint.h>

enum {
    MV_GM_BOOT_LAUNCH_PAD = 0,
    MV_GM_BOOT_DB_LEVEL,
    MV_GM_BOOT_DEVELOP_PRESENT,
    MV_GM_BOOT_ARENA_BYTES,
    MV_GM_BOOT_SEED_TICK,
    MV_GM_BOOT_SIM_MEM_BYTES,
    MV_GM_BOOT_STAT_COUNT,
};

/* Implemented in the original src/melee/gm/gmmain.c when the Vita boot
 * manifest is enabled.  This executes the original boot ordering through the
 * arena/debug-level stage and intentionally stops before HSD/GX init. */
int gmMain_VitaBootProbe(uint32_t out[MV_GM_BOOT_STAT_COUNT]);

enum {
    MV_GM_POST_SEED_TICK = 0,
    MV_GM_POST_STAT_COUNT,
};

/* Continues the original gmmain ordering after a successful HSD_InitComponent:
 * GXSetMisc -> seed assignment -> lbAudioAx_8002838C. The Vita path now runs
 * the upstream AXDriver/HSD_Synth core and stops before aux effects/bank loads. */
int gmMain_VitaPostHsdProbe(uint32_t out[MV_GM_POST_STAT_COUNT]);

enum {
    MV_GM_POST_AUDIO_CONTROLLER = 1u << 0,
    MV_GM_POST_AUDIO_RETRACE = 1u << 1,
    MV_GM_POST_AUDIO_VI_VISIBLE = 1u << 2,
    MV_GM_POST_AUDIO_MEMORY = 1u << 3,
    MV_GM_POST_AUDIO_HEAP = 1u << 4,
    MV_GM_POST_AUDIO_DVD = 1u << 5,
    MV_GM_POST_AUDIO_ARQ = 1u << 6,
};

/* Executes the next contiguous original gmmain block after audio. out[0] is a
 * stage mask so hardware logs identify the exact last completed subsystem. */
int gmMain_VitaPostAudioProbe(uint32_t out[1]);

enum {
    MV_GM_SERVICES_CARDNEW = 1u << 0,
    MV_GM_SERVICES_CARDGAME = 1u << 1,
    MV_GM_SERVICES_SNAPSHOT = 1u << 2,
    MV_GM_SERVICES_MAINLIB = 1u << 3,
    MV_GM_SERVICES_MTHP = 1u << 4,
    MV_GM_SERVICES_SISLIB = 1u << 5,
};

int gmMain_VitaServicesProbe(uint32_t out[1]);

int gmMain_VitaFinalInitProbe(uint32_t out[1]);

enum {
    MV_GM_BOOT_SMOKE_MODE = 0,
    MV_GM_BOOT_SMOKE_SCENE,
    MV_GM_BOOT_SMOKE_NEXT_MODE,
    MV_GM_BOOT_SMOKE_SKIP_INTRO,
    MV_GM_BOOT_SMOKE_STAT_COUNT,
};

/* Executes the actual gmboot.c bootOnLoad callback on a bounded state object
 * and stops before the GS_MEMCARD scene is entered. */
int gm_VitaBootStateProbe(uint32_t out[MV_GM_BOOT_SMOKE_STAT_COUNT]);
void* gm_VitaBootGetEnterData(void);

enum {
    MV_GM_MEMCARD_ENTERED = 7u,
};
int gm_VitaMemCardSceneEnterProbe(uint32_t out[8]);

enum {
    MV_LBFILE_SIZE = 0,
    MV_LBFILE_HEADER_SIZE,
    MV_LBFILE_CHECKSUM,
    MV_LBFILE_STAT_COUNT,
};

/* Exercises Melee's original lbFileGetSize/lbFile_8001668C on MnMaAll.usd
 * through the Vita DVD + DevCom adapters. */
int mv_lbfile_boot_probe(uint32_t out[MV_LBFILE_STAT_COUNT]);

enum {
    MV_LBARCHIVE_FILE_SIZE = 0,
    MV_LBARCHIVE_PUBLICS,
    MV_LBARCHIVE_RELOCATIONS,
    MV_LBARCHIVE_EXTERNS,
    MV_LBARCHIVE_ROOT_OFFSET,
    MV_LBARCHIVE_STAT_COUNT,
};

/* End-to-end original Melee asset path: lbFile -> lbArchive_InitializeDAT ->
 * HSD_ArchiveParse/GetPublicAddress. The raw root is deliberately not passed
 * to HSD_JObjLoadJoint because descriptor scalar conversion is still required. */
int mv_lbarchive_boot_probe(uint32_t out[MV_LBARCHIVE_STAT_COUNT]);

