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
/* Read-only ARAM view for the Vita AX software mixer. Addresses remain in
 * GameCube byte-addressed ARAM space; AX voice sample addresses are nibbles. */
const uint8_t *mv_aram_data(void);
uint32_t mv_aram_capacity(void);
int mv_ax_boot_validate(uint32_t out[6]);
/* The host copy may complete immediately, but GameCube ARQ completion is
 * observable only when the runtime pump dispatches the queued callback. */
void mv_arq_async_pump(void);
unsigned mv_arq_async_pending(void);
/* Enable the actual Vita audio device only from the real application main;
 * ARM/Unicorn tests exercise AX without executing Vita syscall imports. */
void mv_ax_audio_enable_hardware(void);
/* Drive the 200 Hz AX control callback from the 60 Hz main VI thread. */
void mv_ax_vblank_pump(void);

