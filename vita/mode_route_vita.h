#pragma once
#include <melee/gm/types.h>

typedef struct {
    int mode;
    const char *name;
    void (*init)(void);
    void (*load)(void);
    void (*css_enter)(GameModeState *);
    void (*css_exit)(GameModeState *);
    void (*sss_enter)(GameModeState *);
    void (*sss_exit)(GameModeState *);
} MvModeRoute;
const MvModeRoute *mv_mode_vita_route(int mode);
