#pragma once
#include "hsd_scene.h"
#include <stdio.h>

void mv_css_vita_set_log(FILE *log);
int mv_css_vita_prepare(void **data_table);
void mv_css_vita_release(void);
const MvCamera *mv_css_vita_camera(void);
