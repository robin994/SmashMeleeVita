#pragma once
#include <stdint.h>
/* Boot-only VI/GX subset. No GX primitive renderer is supplied by this module. */
void mv_vi_tick(void);
int mv_vi_boot_validate(uint32_t out[6]);
void mv_vi_boot_copy(const void *status, void *buffer, int pass);
int mv_gx_misc_validate(uint32_t out[2]);
void mv_runtime_set_log(void *file);
