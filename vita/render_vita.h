#pragma once
#include <vitaGL.h>
#define MV_RENDER_NAME "vitaGL"
int mv_render_init(void);
void mv_render_fini(void);
void mv_render_begin(void);
void mv_render_present(void);
