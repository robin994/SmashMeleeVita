#include "frame_telemetry_vita.h"
#include <psp2/kernel/processmgr.h>

uint64_t mv_frame_time_us(void) { return sceKernelGetProcessTimeWide(); }

void mv_frame_telemetry_init(MvFrameTelemetry *t, FILE *log, const char *scene)
{
    *t = (MvFrameTelemetry){0}; t->log = log; t->scene = scene;
}

static uint64_t max64(uint64_t a, uint64_t b) { return a > b ? a : b; }

void mv_frame_telemetry_record(MvFrameTelemetry *t, uint64_t frame_us,
                               uint64_t capture_us, uint64_t replay_us,
                               uint64_t present_us)
{
    if (!t) return;
    ++t->frames;
    t->total_frame_us += frame_us; t->total_capture_us += capture_us;
    t->total_replay_us += replay_us; t->total_present_us += present_us;
    t->max_frame_us = max64(t->max_frame_us, frame_us);
    t->max_capture_us = max64(t->max_capture_us, capture_us);
    t->max_replay_us = max64(t->max_replay_us, replay_us);
    t->max_present_us = max64(t->max_present_us, present_us);
}

void mv_frame_telemetry_flush(MvFrameTelemetry *t, int force)
{
    if (!t || !t->log || !t->frames || (!force && t->frames % 60u)) return;
    fprintf(t->log,
            "FRAME_TIMING scene=%s frames=%u frame_us_avg=%llu frame_us_max=%llu "
            "capture_us_avg=%llu capture_us_max=%llu replay_us_avg=%llu replay_us_max=%llu "
            "present_us_avg=%llu present_us_max=%llu\n",
            t->scene ? t->scene : "?", t->frames,
            (unsigned long long)(t->total_frame_us / t->frames),
            (unsigned long long)t->max_frame_us,
            (unsigned long long)(t->total_capture_us / t->frames),
            (unsigned long long)t->max_capture_us,
            (unsigned long long)(t->total_replay_us / t->frames),
            (unsigned long long)t->max_replay_us,
            (unsigned long long)(t->total_present_us / t->frames),
            (unsigned long long)t->max_present_us);
    fflush(t->log);
}
