#pragma once
#include <melee/gm/types.h>

typedef struct {
    int mode;
    const char *name;
    void (*init)(void);
    void (*load)(void);

    u8 css_state_id;
    void *css_data;
    void (*css_enter)(GameModeState *);
    void (*css_exit)(GameModeState *);

    u8 sss_state_id;
    void *sss_data;
    void (*sss_enter)(GameModeState *);
    void (*sss_exit)(GameModeState *);

    u8 vs_state_id;
    u8 vs_scene_kind;
    void *vs_enter_data;
    void *vs_exit_data;
    void (*vs_enter)(GameModeState *);
    void (*vs_exit)(GameModeState *);
} MvModeRoute;

const MvModeRoute *mv_mode_vita_route(int mode);
