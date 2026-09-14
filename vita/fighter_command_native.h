#pragma once

#include "hsd_data.h"

#include <stddef.h>
#include <stdint.h>

typedef struct MvFighterCommandRawResult {
    uint32_t root_count;
    uint32_t command_count;
    uint32_t histogram[59];
} MvFighterCommandRawResult;

/* Nativeize serialized fighter subaction command streams.  DAT relocation
 * words used by Subroutine/Goto remain untouched for HSD_ArchiveParse; command
 * bitfields are repacked from PPC MSB-first layout to ARM LSB-first layout. */
int mv_fighter_command_scripts_prepare_raw(
    void* bytes, size_t size, const uint32_t* script_roots,
    size_t script_root_count, MvFighterCommandRawResult* out,
    const char* label);
