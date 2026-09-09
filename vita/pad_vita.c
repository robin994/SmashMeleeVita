#include "pad_vita.h"
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(PADStatus) == 12, "GameCube PADStatus ABI");
_Static_assert(offsetof(PADStatus, err) == 10, "GameCube PADStatus layout");

static u32 pad_spec = PAD_SPEC_5;
static unsigned long pad_sampling_rate = 11;

/* Raw ranges precede upstream PADClamp, which supplies GameCube deadzones. */
static s8 axis(unsigned value, int inverted)
{
    int centered = (int)value - 128;
    if (centered < -127) centered = -127;
    return (s8)(inverted ? -centered : centered);
}

void melee_vita_map_pad(const SceCtrlData* source, PADStatus* destination)
{
    static const struct { unsigned vita; u16 gc; } buttons[] = {
        { SCE_CTRL_CROSS, PAD_BUTTON_A }, { SCE_CTRL_SQUARE, PAD_BUTTON_B },
        { SCE_CTRL_CIRCLE, PAD_BUTTON_X }, { SCE_CTRL_TRIANGLE, PAD_BUTTON_Y },
        { SCE_CTRL_LTRIGGER, PAD_TRIGGER_L }, { SCE_CTRL_RTRIGGER, PAD_TRIGGER_R },
        { SCE_CTRL_SELECT, PAD_TRIGGER_Z }, { SCE_CTRL_START, PAD_BUTTON_START },
        { SCE_CTRL_UP, PAD_BUTTON_UP }, { SCE_CTRL_DOWN, PAD_BUTTON_DOWN },
        { SCE_CTRL_LEFT, PAD_BUTTON_LEFT }, { SCE_CTRL_RIGHT, PAD_BUTTON_RIGHT },
    };
    memset(destination, 0, sizeof(*destination));
    for (unsigned i = 0; i < sizeof(buttons)/sizeof(buttons[0]); ++i)
        if (source->buttons & buttons[i].vita) destination->button |= buttons[i].gc;
    destination->stickX = axis(source->lx, 0);
    destination->stickY = axis(source->ly, 1);
    destination->substickX = axis(source->rx, 0);
    destination->substickY = axis(source->ry, 1);
    destination->triggerLeft = (source->buttons & SCE_CTRL_LTRIGGER) ? 255 : 0;
    destination->triggerRight = (source->buttons & SCE_CTRL_RTRIGGER) ? 255 : 0;
    destination->analogA = (destination->button & PAD_BUTTON_A) ? 255 : 0;
    destination->analogB = (destination->button & PAD_BUTTON_B) ? 255 : 0;
    destination->err = PAD_ERR_NONE;
}

BOOL PADInit(void)
{
    return sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG) >= 0;
}

u32 PADRead(PADStatus* status)
{
    memset(status, 0, sizeof(*status) * PAD_MAX_CONTROLLERS);
    for (unsigned i = 0; i < PAD_MAX_CONTROLLERS; ++i)
        status[i].err = PAD_ERR_NO_CONTROLLER;
    SceCtrlData source = {0};
    int count = sceCtrlPeekBufferPositive(0, &source, 1);
    if (count > 0) melee_vita_map_pad(&source, &status[0]);
    else status[0].err = PAD_ERR_NOT_READY;
    /* PADRead returns rumble capability bits, NOT connected-controller bits.
       Vita has no rumble motor; status[].err carries connection state. */
    return 0;
}

int PADReset(unsigned long mask)
{
    (void)mask;
    return 1;
}

BOOL PADRecalibrate(u32 mask)
{
    (void)mask;
    return 1;
}

void PADSetSamplingRate(unsigned long msec)
{
    if (msec > 11) msec = 11;
    pad_sampling_rate = msec;
}

void PADControlAllMotors(const u32* commandArray)
{
    (void)commandArray;
}

void PADControlMotor(s32 chan, u32 command)
{
    (void)chan;
    (void)command;
}

void PADSetSpec(u32 spec)
{
    pad_spec = spec;
}

unsigned long PADGetSpec(void)
{
    return pad_spec;
}

int melee_vita_pad_selftest(void)
{
    PADStatus status[PAD_MAX_CONTROLLERS] = {0};
    SceCtrlData source = {0};
    source.lx = source.ly = source.rx = source.ry = 128;
    melee_vita_map_pad(&source, status);
    PADClamp(status);
    if (status[0].button || status[0].stickX || status[0].stickY ||
        status[0].substickX || status[0].substickY) return 0;
    source.buttons = SCE_CTRL_CROSS | SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT;
    source.lx = 255;
    source.ry = 0;
    melee_vita_map_pad(&source, status);
    PADClamp(status);
    return status[0].button == (PAD_BUTTON_A | PAD_TRIGGER_L | PAD_TRIGGER_Z) &&
        status[0].stickX == 72 && status[0].substickY == 59 &&
        status[0].triggerLeft == 150 && status[0].triggerRight == 0 &&
        status[0].analogA == 255 && status[0].err == PAD_ERR_NONE;
}
