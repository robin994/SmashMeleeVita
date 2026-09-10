#pragma once

#include <stdio.h>

#include <melee/sc/forward.h>
#include <sysdolphin/baselib/forward.h>

int mv_menu_vita_prepare(StaticModelDesc *back, StaticModelDesc *panel,
                         StaticModelDesc *content, StaticModelDesc *cursor,
                         HSD_CObjDesc **camera);
int mv_main_menu_run(FILE *log);
void mv_menu_vita_request_action(int menu_kind, int selection);
HSD_Archive *mv_menu_vita_archive_proxy(void);
int mv_menu_vita_archive_is_proxy(const HSD_Archive *archive);
void *mv_menu_vita_archive_lookup(const char *name);

void mv_menu_vita_reset_return(void);
