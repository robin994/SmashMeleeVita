#pragma once
#include <psp2/ctrl.h>
#include <dolphin/pad.h>
/* Shared by the hardware reader and deterministic input checks. */
void melee_vita_map_pad(const SceCtrlData* source, PADStatus* destination);
int melee_vita_pad_selftest(void);
