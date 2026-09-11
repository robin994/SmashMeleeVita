#pragma once
#include "hsd_scene.h"
#include <stdio.h>

void mv_sss_vita_set_log(FILE *log);
int mv_sss_vita_prepare(int use_us, void **data_table);
void mv_sss_vita_release(void);
const MvCamera *mv_sss_vita_camera(void);
