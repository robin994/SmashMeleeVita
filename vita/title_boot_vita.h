#pragma once

#include <stdio.h>

typedef struct StaticModelDesc StaticModelDesc;
typedef struct HSD_CameraDescPerspective HSD_CameraDescPerspective;

int mv_title_vita_prepare(StaticModelDesc *moji, StaticModelDesc *background,
                          HSD_CameraDescPerspective **camera);
int mv_title_boot_run(FILE *log);
void mv_scene_vita_reset(void);
void mv_scene_vita_begin_menu(void);
int mv_scene_vita_done(void);
int mv_scene_vita_pending_mode(void);

void mv_scene_vita_objects_init(void);
void mv_scene_vita_objects_close(void);
void mv_scene_vita_sis_init(unsigned size);
void mv_gm_vita_enter_mode(int mode);
