#pragma once

#include <stdint.h>

enum {
    MV_GX_CHANNEL_EVAL_ACTIVE = 1u << 0,
    MV_GX_CHANNEL_EVAL_LIT = 1u << 1,
    MV_GX_CHANNEL_EVAL_NORMAL = 1u << 2,
};

/* Evaluate a GX raster channel in eye space. HSD's TEV graph consumes
 * RASC/RASA after the XF lighting stage, not the raw vertex color. */
uint32_t mv_gx_channel_eval(unsigned channel, uint32_t vertex_rgba,
                            const float position[3],
                            const float normal[3],
                            uint32_t *flags);
uint32_t mv_gx_channel0_eval(uint32_t vertex_rgba,
                             const float position[3],
                             const float normal[3],
                             uint32_t *flags);
uint32_t mv_gx_channel1_eval(uint32_t vertex_rgba,
                             const float position[3],
                             const float normal[3],
                             uint32_t *flags);
