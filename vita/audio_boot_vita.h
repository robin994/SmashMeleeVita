#pragma once

#include <stdint.h>

enum {
    MV_AUDIO_AR_BASE = 0,
    MV_AUDIO_AR_SIZE,
    MV_AUDIO_ARQ_CHUNK,
    MV_AUDIO_AI_RATE,
    MV_AUDIO_BANK_BASE,
    MV_AUDIO_BANK_COMMON,
    MV_AUDIO_BANK_PRIORITY,
    MV_AUDIO_BANK_TOTAL,
    MV_AUDIO_BOOT_STAT_COUNT,
};

/* Called from the exact lbAudioAx_8002838C bring-up point immediately before
 * AXDriver/HSD Synth initialization. */
void mv_audio_boot_record(uint32_t bank_base, uint32_t bank_common,
                          uint32_t bank_priority, uint32_t bank_total);
int mv_audio_boot_validate(uint32_t out[MV_AUDIO_BOOT_STAT_COUNT]);
int mv_aram_clear(uint32_t dest, uint32_t size);
int mv_ax_boot_validate(uint32_t out[6]);

