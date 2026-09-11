#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/types.h>
#include <melee/ft/forward.h>
#include <melee/gr/forward.h>
#include <melee/lb/lbarchive.h>
#include <melee/ty/toy.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/sislib_font.h>
#include <sysdolphin/baselib/hsd_3A94.h>
#include <sysdolphin/baselib/hsd_3B27.h>

#include <string.h>

/* The generated default SIS glyph atlas is absent from this checkout. Retail
 * SIS archives provide their own texture tables; retain neutral fallback
 * storage only for paths that explicitly select the default font. */
TextGlyphTexture HSD_SisLib_FontAtlas[287] __attribute__((aligned(32)));


void hsd_803AAA48(void)
{
    /* The Vita CARD adapter is synchronous; there is no HSD command queue to
     * pump until read/write support is implemented. */
}

void hsd_803B24E4(s32 *ctx, int channel, int file_no, void *work_buf)
{
    CardState *state = (CardState *)ctx;
    memset(state, 0, sizeof(*state));
    state->x20 = -1;
    state->x4 = channel;
    state->x8 = file_no;
    state->x0 = work_buf;
}

int hsd_803B2550(s32 *ctx, const char *name, void (*callback)(int, int))
{
    (void)callback;
    CardState *state = (CardState *)ctx;
    int result = CARDOpen(state->x4, (char *)name, &state->file_info);
    if (result < 0) return result;
    CARDClose(&state->file_info);
    return CARD_RESULT_IOERROR;
}

s32 hsd_803B2674(CardState *state)
{
    (void)state;
    return CARD_RESULT_IOERROR;
}

void hsd_803AC3E0(CardState *state, int file_idx, int file_size,
                  int file_flags, u8 *data)
{
    if (!state || file_idx < 0 || file_idx >= 9) return;
    state->x4C[file_idx] = file_size;
    state->x70[file_idx].ptr = data;
    state->x28[file_idx] = file_flags;
}

int hsd_803B27F4(const s32 *ctx, const char *name, int a, int b,
                 void (*callback)(int, int))
{
    (void)ctx; (void)name; (void)a; (void)b; (void)callback;
    return CARD_RESULT_IOERROR;
}

int hsd_803B286C(const s32 *ctx, UNK_T arg1, const char *name, int a, int b,
                 void (*callback)(int, int))
{
    (void)ctx; (void)arg1; (void)name; (void)a; (void)b; (void)callback;
    return CARD_RESULT_IOERROR;
}

int hsd_803B2928(const s32 *ctx, const char *name, int a, int b,
                 void (*callback)(int, int))
{
    (void)ctx; (void)name; (void)a; (void)b; (void)callback;
    return CARD_RESULT_IOERROR;
}

int hsd_803B29D8(const s32 *ctx, int channel, const u8 *data, UNK_T callback)
{
    (void)ctx; (void)channel; (void)data; (void)callback;
    return CARD_RESULT_IOERROR;
}

int hsd_803B2A4C(const s32 *ctx, int channel, const u8 *data,
                 void (*callback)(int, int))
{
    (void)ctx; (void)channel; (void)data; (void)callback;
    return CARD_RESULT_IOERROR;
}

int hsd_803B2ADC(s32 *ctx, UNK_T data)
{
    (void)ctx; (void)data;
    return CARD_RESULT_IOERROR;
}

#ifndef MELEE_VITA_FULL_GAMEPLAY_SCENE
void gm_801B23F0(void)
{
    /* Camera-mode-only preload; Regular Match never consumes it. */
}
#endif

#ifndef MELEE_VITA_FULL_GAMEPLAY_SCENE
/* Hardware checkpoint profile: mode-state preparation is kept original while
 * live stage/fighter construction remains behind the full-gameplay link. */
void Ground_801C06B8(GrKind kind)
{
    (void)kind;
}

void Ground_801C5A28(void)
{
    Toy_803124BC();
    Toy_8031234C(0);
    Toy_80305918(0, 0, 1);
}
#endif

/* Menu-graph support: exact BSS storage referenced by nametag maintenance.
 * The Homerun gameplay island itself remains outside this target. */
#ifndef MELEE_VITA_FULL_GAMEPLAY_SCENE
VsModeData gmHomeRun_VsModeData;
#endif



/* These callbacks are reached only when the same UI is hosted by the
 * Tournament game mode. The main-menu graph runs under GM_MENU; retain safe
 * entry points without pulling the Tournament gameplay/UI scene. */
#ifndef MELEE_VITA_FULL_GAMEPLAY_SCENE
void gm_80190EA4(void) {}
void gm_80190FE4(int arg0) { (void)arg0; }
#endif

/* Out-of-line form used by menu-only builds. The full gameplay profile
 * links the original definition from ft_0C31.c. */
#ifndef MELEE_VITA_FULL_GAMEPLAY_SCENE
#ifdef HSD_JObjSetMtxDirty
#undef HSD_JObjSetMtxDirty
#endif
void HSD_JObjSetMtxDirty(HSD_JObj *jobj)
{
    if (jobj != NULL && !HSD_JObjMtxIsDirty(jobj))
        HSD_JObjSetMtxDirtySub(jobj);
}
#endif

/* Exact prize-text index mapping used by the Data/Special Records menu. */
#ifndef MELEE_VITA_FULL_GAMEPLAY_SCENE
void un_802FE3F8(int a, int b, s16 *c, s16 *d)
{
    int offset = a;
    if (a == 62) offset = 63;
    else if (a == 63) offset = 67;
    else if (a == 64) offset = 68;
    else if (a == 65) offset = 62;
    if (a < 0 || a >= 66) return;
    if (c) *c = (s16)(b + offset);
    if (a == 62 && d) *d = (s16)(b + offset + 1);
}
#endif

/* hsd_3B5C snapshot JPEG decoder scratch state. In the original executable
 * these five globals live in hsd_3A94.c together with the full CARD driver;
 * keeping only the decoder state avoids retaining the unrelated CARD island. */
u8* hsd_804D79B8;
u8* hsd_804D79BC;
s32 hsd_804D79C0;
s32 hsd_804D79C4;
u8 hsd_804D79C8;
