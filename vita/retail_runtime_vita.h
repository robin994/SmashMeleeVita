#pragma once

#include <stdio.h>

int mv_retail_runtime_begin(FILE *log);
void mv_retail_runtime_end(void);
void mv_retail_runtime_start_render(int pass);
void mv_retail_runtime_present_frame(int pass);
