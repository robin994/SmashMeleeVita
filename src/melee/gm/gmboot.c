#include "gmboot.h"

#include "gm_unsplit.h"
#include "gmmain_lib.h"
#include "types.h"
#include <melee/lb/lbcardgame.h>
#include <melee/lb/lbcardnew.h>
#include <melee/lb/lblanguage.h>
#include <melee/ty/toy.h>

/* 1BF948 */ static void bootOnLoad(GameModeState*);
/* 1BF9A8 */ static void bootOnLeave(GameModeState*);
/* 1BFA3C */ static void memcardOnLoad(GameModeState*);

/// @todo Move to toy header
enum {
    TROPHY_PIKMIN = 275,
};

struct loadData {
    u32 x0;
    u8 x4;
    u8 mode_id; ///< Copied to ::leaveData::mode_id to set next mode
};

struct leaveData {
    u32 x0;
    u8 mode_id;
};

static struct loadData load_data;
static struct leaveData leave_data;

#ifdef MELEE_VITA_BOOT_PROBE
int gm_VitaBootStateProbe(u32 out[4])
{
    GameModeState state = { 0 };
    if (out == NULL) {
        return -1;
    }

    state.id = 0;
    state.info.scene_kind = GS_MEMCARD;
    state.info.enter_data = &load_data;
    state.info.exit_data = &leave_data;
    bootOnLoad(&state);

    out[0] = GM_BOOT;
    out[1] = state.info.scene_kind;
    out[2] = load_data.mode_id;
    out[3] = gmMainLib_8046B0F0.skip_intro ? 1 : 0;
    return 0;
}

int gm_VitaMemCardStateProbe(u32 out[4])
{
    GameModeState state = { 0 };
    if (out == NULL) {
        return -1;
    }

    /* Keep the same backing data used by the real GM_BOOT/GM_MEMCARD tables.
     * This executes Melee's original memcardOnLoad callback but deliberately
     * stops before gm_Scene_MemCard_OnEnter(), whose archive/UI dependencies
     * are the next runtime frontier. */
    state.id = 0;
    state.info.scene_kind = GS_MEMCARD;
    state.info.enter_data = &load_data;
    state.info.exit_data = &leave_data;
    bootOnLoad(&state);
    memcardOnLoad(&state);

    out[0] = state.info.scene_kind;
    out[1] = load_data.x0;
    out[2] = load_data.x4;
    out[3] = load_data.mode_id;
    return 0;
}
#endif

GameModeState gm_Mode_Boot_States[] = {
    {
        0,
        1,
        0,
        bootOnLoad,
        bootOnLeave,
        GS_MEMCARD,
        &load_data,
        &leave_data,
    },
    {
        -1,
    },
};

void bootOnLoad(GameModeState* scene)
{
    struct loadData* scene_data = gm_GetGameModeStateEnterData(scene);
    scene_data->x4 = 0;
    scene_data->x0 = 0;
    if (gmMainLib_8046B0F0.skip_intro == true) {
        scene_data->mode_id = GM_TITLE;
    } else {
        gm_801BF708(0);
        scene_data->mode_id = GM_OPENING_MV;
    }
}

void bootOnLeave(GameModeState* data)
{
    struct leaveData* scene_data = gm_GetGameModeStateExitData(data);

    if (!Toy_803048C0(TROPHY_PIKMIN)) {
        if (!lb_8001C2D8(0, "01",
                         lbLang_GetLanguageSetting() == LANG_JP ? "GPIJ"
                                                                : "GPIE",
                         "Pikmin dataFile"))
        {
            Toy_803124BC();
            Toy_SetUnlockState(TROPHY_PIKMIN, true);
        }
    }

    gm_SetGameModeOverride(lbCardGame_DecideGameMode);

    // Enter mode
    // Gekko "boot to CSS" code changes scene_id to a hardcoded 2 (::GM_VS)
    gm_ChangeGameModeAfterCurrentScene(scene_data->mode_id);
}

GameModeState gm_Mode_MemCard_States[] = {
    {
        0,
        3,
        0,
        memcardOnLoad,
        NULL,
        {
            GS_MEMCARD,
            &load_data,
            &leave_data,
        },
    },
    { -1 },
};

void memcardOnLoad(GameModeState* scene)
{
    struct loadData* temp_r3 = gm_GetGameModeStateEnterData(scene);
    temp_r3->x4 = 0;
    temp_r3->x0 = 1;
}
