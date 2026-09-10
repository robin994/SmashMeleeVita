#include "mode_route_vita.h"
extern const MvModeRoute mv_route_gmvsmode;
extern const MvModeRoute mv_route_gmstamina;
extern const MvModeRoute mv_route_gmsupersudden;
extern const MvModeRoute mv_route_gmgiant;
extern const MvModeRoute mv_route_gmtiny;
extern const MvModeRoute mv_route_gminvisible;
extern const MvModeRoute mv_route_gmfixedcamera;
extern const MvModeRoute mv_route_gmsinglebutton;
extern const MvModeRoute mv_route_gmlightning;
extern const MvModeRoute mv_route_gmslomo;
extern const MvModeRoute mv_route_gmtrainingmode;
const MvModeRoute *mv_mode_vita_route(int mode)
{
    switch (mode) {
    case GM_VS: return &mv_route_gmvsmode;
    case GM_STAMINA_VS: return &mv_route_gmstamina;
    case GM_SUPER_SUDDEN_DEATH_VS: return &mv_route_gmsupersudden;
    case GM_GIANT_VS: return &mv_route_gmgiant;
    case GM_TINY_VS: return &mv_route_gmtiny;
    case GM_INVISIBLE_VS: return &mv_route_gminvisible;
    case GM_CAMERA_VS: return &mv_route_gmfixedcamera;
    case GM_SINGLE_BUTTON_VS: return &mv_route_gmsinglebutton;
    case GM_LIGHTNING_VS: return &mv_route_gmlightning;
    case GM_SLOMO_VS: return &mv_route_gmslomo;
    case GM_TRAINING: return &mv_route_gmtrainingmode;
    default: return NULL;
    }
}
