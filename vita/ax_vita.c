#include <dolphin/ax.h>
#include <dolphin/axfx.h>

#include <stdint.h>
#include <string.h>

#define MV_AX_VOICE_COUNT AX_MAX_VOICES

typedef struct {
    AXVPB vpb;
    int allocated;
} MvAxVoice;

static MvAxVoice mv_ax_voices[MV_AX_VOICE_COUNT];
static void (*mv_ax_frame_callback)(void);
static void (*mv_ax_aux_a_callback)(void *, void *);
static void *mv_ax_aux_a_context;
static void (*mv_ax_aux_b_callback)(void *, void *);
static void *mv_ax_aux_b_context;
static void *(*mv_axfx_alloc_hook)(unsigned long);
static void (*mv_axfx_free_hook)(void *);
static u32 mv_ax_mode;
static u32 mv_ax_max_dsp_cycles;
static int mv_ax_initialized;

static int voice_index(const AXVPB *voice)
{
    for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
        if (&mv_ax_voices[i].vpb == voice) {
            return i;
        }
    }
    return -1;
}

static void split_addr(u32 addr, u16 *hi, u16 *lo)
{
    *hi = (u16)(addr >> 16);
    *lo = (u16)addr;
}

void AXInit(void)
{
    memset(mv_ax_voices, 0, sizeof(mv_ax_voices));
    for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
        mv_ax_voices[i].vpb.index = (u32)i;
    }
    mv_ax_frame_callback = NULL;
    mv_ax_aux_a_callback = NULL;
    mv_ax_aux_a_context = NULL;
    mv_ax_aux_b_callback = NULL;
    mv_ax_aux_b_context = NULL;
    mv_ax_initialized = 1;
}

void AXQuit(void)
{
    memset(mv_ax_voices, 0, sizeof(mv_ax_voices));
    mv_ax_frame_callback = NULL;
    mv_ax_initialized = 0;
}

AXVPB *AXAcquireVoice(u32 priority, void (*callback)(void *), u32 userContext)
{
    if (!mv_ax_initialized) {
        return NULL;
    }

    int slot = -1;
    for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
        if (!mv_ax_voices[i].allocated) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        int victim = -1;
        u32 victim_priority = UINT32_MAX;
        for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
            if ((u32)mv_ax_voices[i].vpb.priority < priority &&
                (u32)mv_ax_voices[i].vpb.priority < victim_priority)
            {
                victim = i;
                victim_priority = (u32)mv_ax_voices[i].vpb.priority;
            }
        }
        if (victim < 0) {
            return NULL;
        }
        if (mv_ax_voices[victim].vpb.callback) {
            mv_ax_voices[victim].vpb.callback(
                (void *)(uintptr_t)mv_ax_voices[victim].vpb.userContext);
        }
        slot = victim;
    }

    AXVPB *voice = &mv_ax_voices[slot].vpb;
    memset(voice, 0, sizeof(*voice));
    voice->priority = (int)priority;
    voice->callback = callback;
    voice->userContext = userContext;
    voice->index = (u32)slot;
    mv_ax_voices[slot].allocated = 1;
    return voice;
}

void AXFreeVoice(AXVPB *voice)
{
    int idx = voice_index(voice);
    if (idx < 0) {
        return;
    }
    memset(&mv_ax_voices[idx].vpb, 0, sizeof(AXVPB));
    mv_ax_voices[idx].vpb.index = (u32)idx;
    mv_ax_voices[idx].allocated = 0;
}

void AXSetVoicePriority(AXVPB *p, u32 priority)
{
    if (p) p->priority = (int)priority;
}

void AXRegisterAuxACallback(void (*callback)(void *, void *), void *context)
{
    mv_ax_aux_a_callback = callback;
    mv_ax_aux_a_context = context;
}

void AXRegisterAuxBCallback(void (*callback)(void *, void *), void *context)
{
    mv_ax_aux_b_callback = callback;
    mv_ax_aux_b_context = context;
}

void AXSetMode(u32 mode) { mv_ax_mode = mode; }
u32 AXGetMode(void) { return mv_ax_mode; }

void AXRegisterCallback(void (*callback)())
{
    mv_ax_frame_callback = (void (*)(void))callback;
}

void AXSetVoiceSrcType(AXVPB *p, u32 type)
{
    if (p) p->pb.srcSelect = (u16)type;
}

void AXSetVoiceState(AXVPB *p, u16 state)
{
    if (p) p->pb.state = state;
}

void AXSetVoiceType(AXVPB *p, u16 type)
{
    if (p) p->pb.type = type;
}

void AXSetVoiceMix(AXVPB *p, AXPBMIX *mix)
{
    if (p && mix) p->pb.mix = *mix;
}

void AXSetVoiceItdOn(AXVPB *p)
{
    if (p) p->pb.itd.flag = 1;
}

void AXSetVoiceItdTarget(AXVPB *p, u16 lShift, u16 rShift)
{
    if (!p) return;
    p->pb.itd.targetShiftL = lShift;
    p->pb.itd.targetShiftR = rShift;
}

void AXSetVoiceUpdateIncrement(AXVPB *p)
{
    if (p) ++p->updateCounter;
}

void AXSetVoiceUpdateWrite(AXVPB *p, u16 param, u16 data)
{
    if (!p || p->updateCounter >= 64) return;
    p->updateData[p->updateCounter * 2] = param;
    p->updateData[p->updateCounter * 2 + 1] = data;
    ++p->updateCounter;
}

void AXSetVoiceDpop(AXVPB *p, AXPBDPOP *dpop)
{
    if (p && dpop) p->pb.dpop = *dpop;
}

void AXSetVoiceVe(AXVPB *p, AXPBVE *ve)
{
    if (p && ve) p->pb.ve = *ve;
}

void AXSetVoiceVeDelta(AXVPB *p, s16 delta)
{
    if (p) p->pb.ve.currentDelta = delta;
}

void AXSetVoiceFir(AXVPB *p, AXPBFIR *fir)
{
    if (p && fir) p->pb.fir = *fir;
}

void AXSetVoiceAddr(AXVPB *p, AXPBADDR *addr)
{
    if (p && addr) p->pb.addr = *addr;
}

void AXSetVoiceLoop(AXVPB *p, u16 loop)
{
    if (p) p->pb.addr.loopFlag = loop;
}

void AXSetVoiceLoopAddr(AXVPB *p, u32 addr)
{
    if (p) split_addr(addr, &p->pb.addr.loopAddressHi, &p->pb.addr.loopAddressLo);
}

void AXSetVoiceEndAddr(AXVPB *p, u32 addr)
{
    if (p) split_addr(addr, &p->pb.addr.endAddressHi, &p->pb.addr.endAddressLo);
}

void AXSetVoiceCurrentAddr(AXVPB *p, u32 addr)
{
    if (p) split_addr(addr, &p->pb.addr.currentAddressHi, &p->pb.addr.currentAddressLo);
}

void AXSetVoiceAdpcm(AXVPB *p, AXPBADPCM *adpcm)
{
    if (p && adpcm) p->pb.adpcm = *adpcm;
}

void AXSetVoiceSrc(AXVPB *p, AXPBSRC *src)
{
    if (p && src) p->pb.src = *src;
}

void AXSetVoiceSrcRatio(AXVPB *p, float ratio)
{
    if (!p) return;
    if (ratio < 0.0f) ratio = 0.0f;
    uint32_t fixed = (uint32_t)(ratio * 65536.0f + 0.5f);
    p->pb.src.ratioHi = (u16)(fixed >> 16);
    p->pb.src.ratioLo = (u16)fixed;
}

void AXSetVoiceAdpcmLoop(AXVPB *p, AXPBADPCMLOOP *adpcmloop)
{
    if (p && adpcmloop) p->pb.adpcmLoop = *adpcmloop;
}

void AXSetMaxDspCycles(u32 cycles) { mv_ax_max_dsp_cycles = cycles; }
u32 AXGetMaxDspCycles(void) { return mv_ax_max_dsp_cycles; }
u32 AXGetDspCycles(void) { return 0; }

void AXFXSetHooks(void *(*alloc_hook)(unsigned long), void (*free_hook)(void *))
{
    mv_axfx_alloc_hook = alloc_hook;
    mv_axfx_free_hook = free_hook;
}

void *AXFXAllocFunction(unsigned long size)
{
    return mv_axfx_alloc_hook ? mv_axfx_alloc_hook(size) : NULL;
}

void AXFXFreeFunction(void *ptr)
{
    if (mv_axfx_free_hook) mv_axfx_free_hook(ptr);
}

/* The boot path explicitly disables both aux buses. These entry points remain
 * fail-closed for effects until an actual Vita effect/mix implementation exists. */
int AXFXReverbHiInit(struct AXFX_REVERBHI *rev) { (void)rev; return 0; }
int AXFXReverbHiShutdown(struct AXFX_REVERBHI *rev) { (void)rev; return 0; }
int AXFXReverbHiSettings(struct AXFX_REVERBHI *rev) { (void)rev; return 0; }
void AXFXReverbHiCallback(struct AXFX_BUFFERUPDATE *b, struct AXFX_REVERBHI *r)
{ (void)b; (void)r; }
int AXFXReverbStdInit(struct AXFX_REVERBSTD *rev) { (void)rev; return 0; }
int AXFXReverbStdShutdown(struct AXFX_REVERBSTD *rev) { (void)rev; return 0; }
int AXFXReverbStdSettings(struct AXFX_REVERBSTD *rev) { (void)rev; return 0; }
void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE *b, struct AXFX_REVERBSTD *r)
{ (void)b; (void)r; }
int AXFXChorusInit(struct AXFX_CHORUS *c) { (void)c; return 0; }
int AXFXChorusShutdown(struct AXFX_CHORUS *c) { (void)c; return 0; }
int AXFXChorusSettings(struct AXFX_CHORUS *c) { (void)c; return 0; }
void AXFXChorusCallback(struct AXFX_BUFFERUPDATE *b, struct AXFX_CHORUS *c)
{ (void)b; (void)c; }
int AXFXDelayInit(struct AXFX_DELAY *d) { (void)d; return 0; }
int AXFXDelayShutdown(struct AXFX_DELAY *d) { (void)d; return 0; }
int AXFXDelaySettings(struct AXFX_DELAY *d) { (void)d; return 0; }
void AXFXDelayCallback(struct AXFX_BUFFERUPDATE *b, struct AXFX_DELAY *d)
{ (void)b; (void)d; }

int mv_ax_boot_validate(uint32_t out[6])
{
    if (!out || !mv_ax_initialized || !mv_ax_frame_callback) return -1;
    uint32_t allocated = 0;
    for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
        allocated += mv_ax_voices[i].allocated ? 1u : 0u;
    }
    out[0] = MV_AX_VOICE_COUNT;
    out[1] = allocated;
    out[2] = mv_ax_aux_a_callback ? 1u : 0u;
    out[3] = mv_ax_aux_b_callback ? 1u : 0u;
    out[4] = mv_ax_mode;
    out[5] = mv_ax_max_dsp_cycles;
    return 0;
}
