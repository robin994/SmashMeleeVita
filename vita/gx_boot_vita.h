#pragma once
#include <stdint.h>

typedef struct {
    uint32_t color;
    float a[3];
    float k[3];
    float position[3];
    float direction[3];
    uint8_t valid;
} MvGxLoadedLight;

/* Boot-only VI/GX subset. No GX primitive renderer is supplied by this module. */
void mv_vi_tick(void);
int mv_vi_boot_validate(uint32_t out[6]);
void mv_vi_boot_copy(const void *status, void *buffer, int pass);
int mv_gx_misc_validate(uint32_t out[2]);
void mv_runtime_set_log(void *file);
int mv_gx_loaded_light(unsigned index, MvGxLoadedLight *out);
