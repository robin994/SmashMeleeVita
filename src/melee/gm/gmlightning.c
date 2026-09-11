#include "gmlightning.h"

#include "gm_unsplit.h"
#include "gmmain_lib.h"
#include "gmmovieend.h"
#include "gmvsmelee.h"
#include "gmvsmode.h"
#include "types.h"
#include <melee/if/if_2FD9.h>

GameModeState gm_Mode_LightningVs_States[] = {
    {
        0,
        3,
        0,
        gm_801BA704,
        gm_801BA730,
        {
            GS_CSS,
            &gmVsMelee_CssData,
            &gmVsMelee_CssData,
        },
    },
    {
        1,
        3,
        0,
        gm_801BA758,
        gm_801BA780,
        {
            GS_SSS,
            &gmVsMelee_SssData,
            &gmVsMelee_SssData,
        },
    },
    {
        2,
        3,
        0,
        gm_801BA7B8,
        gm_801BA7EC,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        3,
        3,
        0,
        gm_801BA814,
        gm_801BA848,
        {
            GS_SUDDEN_DEATH,
            &gmVsMelee_StartData,
            &gmVsMelee_SuddenDeathExitInfo,
        },
    },
    {
        4,
        3,
        0,
        gm_801BA868,
        gm_801BA888,
        {
            GS_RESULTS,
            &gmVsMelee_ResultsEnterData,
            NULL,
        },
    },
    {
        0x80,
        2,
        0,
        gm_ModeState_Approach_OnEnter,
        NULL,
        {
            GS_APPROACH,
            &gmVsMelee_ApproachData,
            &gmVsMelee_ApproachData,
        },
    },
    {
        0x81,
        2,
        0,
        gm_ModeState_ApproachVs_OnEnter,
        gm_ModeState_ApproachVs_OnExit,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        0xC0,
        2,
        0,
        gm_ModeState_Prize_OnEnter,
        gm_ModeState_Prize_OnExit,
        {
            GS_PRIZE_INTERFACE,
            &if_Scene_Prize_EnterData,
            NULL,
        },
    },
    { -1 },
};

void gm_801BA704(GameModeState* scene)
{
    gmVsMelee_EnterCss(scene, &gmMainLib_804D3EE0->modes.vs_lightning, 9);
}

void gm_801BA730(GameModeState* scene)
{
    gmVsMelee_ExitCss(scene, &gmMainLib_804D3EE0->modes.vs_lightning);
}

void gm_801BA758(GameModeState* scene)
{
    gmVsMelee_EnterSss(scene, &gmMainLib_804D3EE0->modes.vs_lightning);
}

void gm_801BA780(GameModeState* scene)
{
    gmVsMelee_ExitSss(scene, &gmMainLib_804D3EE0->modes.vs_lightning,
                      gmVsMode_State_Css);
}

/// Sets game speed to 1.25F for lightning melee
static void fn_801BA7AC(StartMeleeData* start, UNUSED StartMeleeData* vs)
{
    start->rules.game_speed = 1.25F;
}

void gm_801BA7B8(GameModeState* scene)
{
    VsModeData* data = &gmMainLib_804D3EE0->modes.vs_lightning;
    gmVsMelee_EnterVs(scene, data, fn_801BA7AC, NULL);
}

void gm_801BA7EC(GameModeState* scene)
{
    gmVsMelee_ExitVs(scene, 4, 3);
}

void gm_801BA814(GameModeState* scene)
{
    VsModeData* data = &gmMainLib_804D3EE0->modes.vs_lightning;
    gmVsMelee_EnterSuddenDeath(scene, data, fn_801BA7AC, NULL);
}

void gm_801BA848(GameModeState* scene)
{
    gmVsMelee_ExitSuddenDeath(scene);
}

void gm_801BA868(GameModeState* scene)
{
    gmVsMelee_EnterResults(scene);
}

void gm_801BA888(GameModeState* scene)
{
    gmVsMelee_ExitResults(scene, &gmMainLib_804D3EE0->modes.vs_lightning, 0);
}

void gm_Mode_LightningVs_OnInit(void)
{
    gm_InitVsMode(&gmMainLib_804D3EE0->modes.vs_lightning);
}

void gm_Mode_LightningVs_OnLoad(void)
{
    gmVsMelee_ResetKOCounts();
}

#ifdef MELEE_VITA_PLATFORM
#include "mode_route_vita.h"
/* Bind the original callbacks without retaining unrelated results states. */
const MvModeRoute mv_route_gmlightning = {
    .mode = GM_LIGHTNING_VS, .name = "GM_LIGHTNING_VS",
    .init = gm_Mode_LightningVs_OnInit, .load = gm_Mode_LightningVs_OnLoad,
    .css_state_id = 0, .css_data = &gmVsMelee_CssData,
    .css_enter = gm_801BA704, .css_exit = gm_801BA730,
    .sss_state_id = 1, .sss_data = &gmVsMelee_SssData,
    .sss_enter = gm_801BA758, .sss_exit = gm_801BA780,
    .vs_state_id = 2, .vs_scene_kind = GS_VS,
    .vs_enter_data = &gmVsMelee_StartData, .vs_exit_data = &gmVsMelee_VsExitInfo,
    .vs_enter = gm_801BA7B8, .vs_exit = gm_801BA7EC,
};
#endif
