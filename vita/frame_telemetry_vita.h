#pragma once
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *log;
    const char *scene;
    uint64_t total_frame_us, total_capture_us, total_replay_us, total_present_us;
    uint64_t max_frame_us, max_capture_us, max_replay_us, max_present_us;
    unsigned frames;
} MvFrameTelemetry;

uint64_t mv_frame_time_us(void);
void mv_frame_telemetry_init(MvFrameTelemetry *t, FILE *log, const char *scene);
void mv_frame_telemetry_record(MvFrameTelemetry *t, uint64_t frame_us,
                               uint64_t capture_us, uint64_t replay_us,
                               uint64_t present_us);
void mv_frame_telemetry_flush(MvFrameTelemetry *t, int force);
