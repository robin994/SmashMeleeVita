#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/types.h>
#include <melee/ft/forward.h>
#include <melee/gr/forward.h>
#include <melee/lb/lbarchive.h>
#include <melee/ty/toy.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/hsd_3A94.h>
#include <sysdolphin/baselib/hsd_3B27.h>

#include <string.h>

/* Direct C equivalents of two tiny game-state helpers.  Keeping them here
 * avoids dragging entire later scene translation units into the bounded boot
 * target before those scenes are actually ready to run on Vita. */
bool gm_IsCurrently1PMode(void)
{
    return gm_Is1PMode(gm_GetCurrentGameMode());
}


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

/* The CSS runs its original HSD/SIS state machine, but rendering is owned by
 * the Vita GX-capture/replay backend. These adapters deliberately suppress
 * only legacy GX text submission and preload-only GameCube callbacks. */
void Player_80031CB0(CharacterKind kind, u8 color)
{
    (void)kind;
    (void)color;
}

void Player_80031D2C(CharacterKind kind, u8 color)
{
    (void)kind;
    (void)color;
}

void gm_801B23F0(void)
{
    /* Camera-mode-only preload; Regular Match never consumes it. */
}

void Ground_801C06B8(GrKind kind)
{
    (void)kind;
    /* Stage asset preload is deferred until the actual VS stage island. */
}


void Ground_801C5A28(void)
{
    Toy_803124BC();
    Toy_8031234C(0);
    Toy_80305918(0, 0, 1);
}

/* Menu-graph support: exact BSS storage referenced by nametag maintenance.
 * The Homerun gameplay island itself remains outside this target. */
VsModeData gmHomeRun_VsModeData;

int gm_8016F120(int arg0)
{
    return gmMainLib_8015DADC(arg0);
}

void gm_80174238(void)
{
    for (int i = 0; i < 0x12C; ++i) gmMainLib_8015DA68(i);
}

/* These callbacks are reached only when the same UI is hosted by the
 * Tournament game mode. The main-menu graph runs under GM_MENU; retain safe
 * entry points without pulling the Tournament gameplay/UI scene. */
void gm_80190EA4(void) {}
void gm_80190FE4(int arg0) { (void)arg0; }

/* Out-of-line form used by a few menu translation units. */
#ifdef HSD_JObjSetMtxDirty
#undef HSD_JObjSetMtxDirty
#endif
void HSD_JObjSetMtxDirty(HSD_JObj *jobj)
{
    if (jobj != NULL && !HSD_JObjMtxIsDirty(jobj))
        HSD_JObjSetMtxDirtySub(jobj);
}

/* Exact prize-text index mapping used by the Data/Special Records menu. */
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

/* hsd_3B5C snapshot JPEG decoder scratch state. In the original executable
 * these five globals live in hsd_3A94.c together with the full CARD driver;
 * keeping only the decoder state avoids retaining the unrelated CARD island. */
u8* hsd_804D79B8;
u8* hsd_804D79BC;
s32 hsd_804D79C0;
s32 hsd_804D79C4;
u8 hsd_804D79C8;
