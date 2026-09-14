#include <dolphin/ax.h>
#include <dolphin/axfx.h>
#include <dolphin/os.h>
#include "audio_boot_vita.h"

#include <sysdolphin/baselib/debug.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>

#define MV_AX_VOICE_COUNT AX_MAX_VOICES

typedef struct {
    AXVPB vpb;
    int allocated;
    uint32_t decode_serial;
    uint32_t decode_seen;
    uint32_t nibble_addr;
    uint32_t phase_q16;
    int16_t sample;
    int16_t next_sample;
    int source_eof;
    int16_t yn1;
    int16_t yn2;
    uint16_t pred_scale;
    int decode_ready;
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
static volatile int mv_ax_audio_running;
static SceUID mv_ax_audio_thread = -1;
static int mv_ax_audio_port = -1;
static OSTime mv_ax_last_pump_tick;
static OSTime mv_ax_pump_remainder;
static int mv_ax_hw_enabled;
static volatile int mv_ax_audio_last_error;
static int mv_ax_audio_reported;
static volatile int mv_ax_voice_lock;
static unsigned mv_ax_voice_trace_count;

/* SCE_AUDIO_OUT_PORT_TYPE_MAIN accepts only 48 kHz on real Vita. Melee's
 * AX/DSP clock is 32 kHz, so the software mixer resamples 32 -> 48 kHz. */
#define MV_AX_OUTPUT_RATE 48000u
#define MV_AX_DSP_RATE 32000u
#define MV_AX_OUTPUT_SAMPLES 256u
static uint32_t mv_ax_ve_phase_q16;

static void ax_voice_lock_enter(void)
{
    while (__sync_lock_test_and_set(&mv_ax_voice_lock, 1))
        sceKernelDelayThread(50);
}

static void ax_voice_lock_leave(void)
{
    __sync_lock_release(&mv_ax_voice_lock);
}

static uint32_t ax_addr(u16 hi, u16 lo)
{
    return ((uint32_t)hi << 16) | lo;
}

static void ax_store_addr(u16 *hi, u16 *lo, uint32_t addr)
{
    *hi = (u16)(addr >> 16);
    *lo = (u16)addr;
}

static int16_t ax_clamp_s16(int32_t value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static int ax_read_nibble(uint32_t nibble_addr, uint8_t *value)
{
    const uint8_t *aram = mv_aram_data();
    const uint32_t capacity = mv_aram_capacity();
    const uint32_t byte_addr = nibble_addr >> 1;
    if (!value || !aram || byte_addr >= capacity) return -1;
    const uint8_t byte = aram[byte_addr];
    *value = (nibble_addr & 1u) ? (byte & 0x0fu) : (byte >> 4);
    return 0;
}

static int ax_decode_source(MvAxVoice *voice, int16_t *out)
{
    AXVPB *v = &voice->vpb;
    uint32_t addr = voice->nibble_addr;
    const uint32_t end = ax_addr(v->pb.addr.endAddressHi, v->pb.addr.endAddressLo);

retry_after_loop:
    if (addr > end) {
        if (!v->pb.addr.loopFlag) return 1;
        addr = ax_addr(v->pb.addr.loopAddressHi, v->pb.addr.loopAddressLo);
        voice->pred_scale = v->pb.adpcmLoop.loop_pred_scale;
        voice->yn1 = (int16_t)v->pb.adpcmLoop.loop_yn1;
        voice->yn2 = (int16_t)v->pb.adpcmLoop.loop_yn2;
        voice->nibble_addr = addr;
        if (addr > end) return -1;
    }

    switch (v->pb.addr.format) {
    case 0: {
        if ((addr & 0x0fu) < 2u) {
            const uint32_t frame = addr & ~0x0fu;
            uint8_t hi, lo;
            if (ax_read_nibble(frame, &hi) || ax_read_nibble(frame + 1u, &lo)) return -1;
            voice->pred_scale = (u16)((hi << 4) | lo);
            addr = frame + 2u;
            if (addr > end) { voice->nibble_addr = addr; goto retry_after_loop; }
        }
        uint8_t raw;
        if (ax_read_nibble(addr, &raw)) return -1;
        int nibble = (raw & 8u) ? (int)raw - 16 : (int)raw;
        unsigned predictor = voice->pred_scale >> 4;
        unsigned scale = voice->pred_scale & 0x0fu;
        if (predictor >= 8u) predictor = 0;
        const int16_t c1 = (int16_t)v->pb.adpcm.a[predictor][0];
        const int16_t c2 = (int16_t)v->pb.adpcm.a[predictor][1];
        int32_t accum = nibble * (1 << scale) * 2048;
        accum += (int32_t)c1 * voice->yn1 + (int32_t)c2 * voice->yn2;
        const int16_t sample = ax_clamp_s16((accum + 1024) >> 11);
        voice->yn2 = voice->yn1; voice->yn1 = sample; *out = sample;
        voice->nibble_addr = addr + 1u;
        return 0;
    }
    case 10: {
        const uint8_t *aram = mv_aram_data();
        const uint32_t capacity = mv_aram_capacity();
        const uint64_t byte_addr = (uint64_t)addr * 2u;
        if (!aram || byte_addr + 1u >= capacity) return -1;
        *out = (int16_t)(((uint16_t)aram[byte_addr] << 8) | aram[byte_addr + 1u]);
        voice->nibble_addr = addr + 1u;
        return 0;
    }
    case 25: {
        const uint8_t *aram = mv_aram_data();
        const uint32_t capacity = mv_aram_capacity();
        if (!aram || addr >= capacity) return -1;
        *out = (int16_t)((int8_t)aram[addr]) << 8;
        voice->nibble_addr = addr + 1u;
        return 0;
    }
    default:
        return -1;
    }
}

static void ax_decode_reset(MvAxVoice *voice)
{
    AXVPB *v = &voice->vpb;
    voice->nibble_addr = ax_addr(v->pb.addr.currentAddressHi, v->pb.addr.currentAddressLo);
    voice->yn1 = (int16_t)v->pb.adpcm.yn1;
    voice->yn2 = (int16_t)v->pb.adpcm.yn2;
    voice->pred_scale = v->pb.adpcm.pred_scale;
    voice->phase_q16 = 0;
    voice->sample = 0;
    voice->next_sample = 0;
    voice->source_eof = 0;
    voice->decode_seen = voice->decode_serial;
    voice->decode_ready = 1;
    int r = ax_decode_source(voice, &voice->sample);
    if (r != 0) { v->pb.state = 0; voice->decode_ready = 0; return; }
    r = ax_decode_source(voice, &voice->next_sample);
    if (r == 1) { voice->next_sample = voice->sample; voice->source_eof = 1; }
    else if (r < 0) { v->pb.state = 0; voice->decode_ready = 0; }
}

static void ax_mix_block(int16_t *output, unsigned frames)
{
    ax_voice_lock_enter();
    memset(output, 0, sizeof(*output) * frames * 2u);
    const uint32_t ve_step_q16 = (uint32_t)(((uint64_t)MV_AX_DSP_RATE << 16) / MV_AX_OUTPUT_RATE);
    for (unsigned frame = 0; frame < frames; ++frame) {
        int64_t mix_l = 0, mix_r = 0;
        mv_ax_ve_phase_q16 += ve_step_q16;
        int advance_ve = 0;
        if (mv_ax_ve_phase_q16 >= 0x10000u) { mv_ax_ve_phase_q16 -= 0x10000u; advance_ve = 1; }
        for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
            MvAxVoice *voice = &mv_ax_voices[i];
            AXVPB *v = &voice->vpb;
            if (!voice->allocated || v->pb.state == 0) continue;
            if (!voice->decode_ready || voice->decode_seen != voice->decode_serial) ax_decode_reset(voice);
            if (!voice->decode_ready || !v->pb.state) continue;
            uint32_t ratio = ((uint32_t)v->pb.src.ratioHi << 16) | v->pb.src.ratioLo;
            if (!ratio) continue;
            uint32_t step = (uint32_t)(((uint64_t)ratio * MV_AX_DSP_RATE) / MV_AX_OUTPUT_RATE);
            if (!step) step = 1;
            const int32_t delta_sample = (int32_t)voice->next_sample - voice->sample;
            const int32_t resampled = voice->sample + (int32_t)(((int64_t)delta_sample * voice->phase_q16) >> 16);
            int32_t volume = v->pb.ve.currentVolume;
            if (advance_ve && v->pb.ve.currentDelta) {
                int32_t next = volume + v->pb.ve.currentDelta;
                if (next < 0) next = 0; if (next > 32767) next = 32767;
                v->pb.ve.currentVolume = (u16)next;
            }
            const int64_t scaled = (int64_t)resampled * volume;
            mix_l += scaled * v->pb.mix.vL / (32767ll * 32767ll);
            mix_r += scaled * v->pb.mix.vR / (32767ll * 32767ll);
            voice->phase_q16 += step;
            while (voice->phase_q16 >= 0x10000u && v->pb.state) {
                voice->phase_q16 -= 0x10000u;
                voice->sample = voice->next_sample;
                if (voice->source_eof) { v->pb.state = 0; voice->decode_ready = 0; break; }
                int r = ax_decode_source(voice, &voice->next_sample);
                if (r == 1) { voice->next_sample = voice->sample; voice->source_eof = 1; }
                else if (r < 0) { v->pb.state = 0; voice->decode_ready = 0; break; }
            }
        }
        output[frame * 2u] = ax_clamp_s16((int32_t)mix_l);
        output[frame * 2u + 1u] = ax_clamp_s16((int32_t)mix_r);
    }
    for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
        MvAxVoice *voice = &mv_ax_voices[i];
        if (voice->allocated && voice->decode_ready)
            ax_store_addr(&voice->vpb.pb.addr.currentAddressHi, &voice->vpb.pb.addr.currentAddressLo, voice->nibble_addr);
    }
    ax_voice_lock_leave();
}

static int ax_audio_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int16_t buffer[MV_AX_OUTPUT_SAMPLES * 2u];
    mv_ax_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                                           MV_AX_OUTPUT_SAMPLES,
                                           MV_AX_OUTPUT_RATE,
                                           SCE_AUDIO_OUT_MODE_STEREO);
    if (mv_ax_audio_port < 0) {
        mv_ax_audio_last_error = mv_ax_audio_port;
        mv_ax_audio_running = 0;
        return mv_ax_audio_port;
    }
    {
        int volumes[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
        sceAudioOutSetVolume(mv_ax_audio_port,
                             SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                             volumes);
    }
    while (mv_ax_audio_running) {
        ax_mix_block(buffer, MV_AX_OUTPUT_SAMPLES);
        int result = sceAudioOutOutput(mv_ax_audio_port, buffer);
        if (result < 0) {
            mv_ax_audio_last_error = result;
            break;
        }
    }
    sceAudioOutOutput(mv_ax_audio_port, NULL);
    sceAudioOutReleasePort(mv_ax_audio_port);
    mv_ax_audio_port = -1;
    mv_ax_audio_running = 0;
    return 0;
}

static void ax_audio_start(void)
{
    if (mv_ax_audio_running || mv_ax_audio_thread >= 0) return;
    mv_ax_audio_last_error = 0;
    mv_ax_audio_reported = 0;
    mv_ax_audio_running = 1;
    mv_ax_audio_thread = sceKernelCreateThread("melee_ax_audio", ax_audio_thread,
                                               0x10000100, 0x8000, 0, 0, NULL);
    if (mv_ax_audio_thread < 0) {
        OSReport("VITA_AUDIO_OUTPUT_FAIL stage=create_thread code=%08x\n",
                 (unsigned)mv_ax_audio_thread);
        mv_ax_audio_running = 0;
        return;
    }
    int result = sceKernelStartThread(mv_ax_audio_thread, 0, NULL);
    if (result < 0) {
        OSReport("VITA_AUDIO_OUTPUT_FAIL stage=start_thread code=%08x\n",
                 (unsigned)result);
        sceKernelDeleteThread(mv_ax_audio_thread);
        mv_ax_audio_thread = -1;
        mv_ax_audio_running = 0;
    }
}

static void ax_audio_stop(void)
{
    if (mv_ax_audio_thread < 0) return;
    mv_ax_audio_running = 0;
    sceKernelWaitThreadEnd(mv_ax_audio_thread, NULL, NULL);
    sceKernelDeleteThread(mv_ax_audio_thread);
    mv_ax_audio_thread = -1;
}

void mv_ax_audio_enable_hardware(void)
{
    mv_ax_hw_enabled = 1;
    if (mv_ax_initialized) ax_audio_start();
}

void mv_ax_vblank_pump(void)
{
    /* The output thread never writes runtime.log directly. Report its state
     * here on the main emulation thread so stdio cannot corrupt the log. */
    if (mv_ax_hw_enabled && !mv_ax_audio_reported) {
        if (mv_ax_audio_port >= 0) {
            OSReport("VITA_AUDIO_OUTPUT_BEGIN port=%d rate=%u frames=%u mixer=AX_DSP_ADPCM_SYNCED\n",
                     mv_ax_audio_port, MV_AX_OUTPUT_RATE, MV_AX_OUTPUT_SAMPLES);
            mv_ax_audio_reported = 1;
        } else if (mv_ax_audio_last_error < 0) {
            OSReport("VITA_AUDIO_OUTPUT_FAIL code=%08x\n",
                     (unsigned)mv_ax_audio_last_error);
            mv_ax_audio_reported = 1;
        }
    }
    if (!mv_ax_initialized || !mv_ax_frame_callback) return;

    /* AX is a 5 ms (200 Hz) control engine.  The Vita port has two kinds of
     * scene loops: original VI-driven gameplay and native title/menu loops
     * that present directly through vitaGL.  Derive callback cadence from the
     * emulated OS timer instead of assuming one call == one retrace, so both
     * paths advance HSD Synth identically and duplicate pump sites are safe. */
    const OSTime frame_ticks = 40500000ll / 200ll;
    OSTime now = OSGetTime();
    if (!mv_ax_last_pump_tick) {
        mv_ax_last_pump_tick = now;
        return;
    }
    OSTime delta = now - mv_ax_last_pump_tick;
    mv_ax_last_pump_tick = now;
    if (delta < 0) {
        mv_ax_pump_remainder = 0;
        return;
    }

    /* A debugger pause or long asset load must not dump hundreds of audio
     * callbacks into one render frame.  Eight AX frames (40 ms) is enough to
     * recover normal cadence while keeping the main thread responsive. */
    const OSTime max_catchup = frame_ticks * 8;
    if (delta > max_catchup) delta = max_catchup;
    mv_ax_pump_remainder += delta;
    unsigned count = (unsigned)(mv_ax_pump_remainder / frame_ticks);
    mv_ax_pump_remainder %= frame_ticks;
    while (count--) mv_ax_frame_callback();
}

static int voice_index(const AXVPB *voice)
{
    for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
        if (&mv_ax_voices[i].vpb == voice) {
            return i;
        }
    }
    return -1;
}

static void mark_decode_reset(AXVPB *voice)
{
    int idx = voice_index(voice);
    if (idx < 0) return;
    ++mv_ax_voices[idx].decode_serial;
    mv_ax_voices[idx].decode_ready = 0;
}

static void split_addr(u32 addr, u16 *hi, u16 *lo)
{
    *hi = (u16)(addr >> 16);
    *lo = (u16)addr;
}

void AXInit(void)
{
    ax_audio_stop();
    memset(mv_ax_voices, 0, sizeof(mv_ax_voices));
    for (int i = 0; i < MV_AX_VOICE_COUNT; ++i) {
        mv_ax_voices[i].vpb.index = (u32)i;
    }
    mv_ax_frame_callback = NULL;
    mv_ax_aux_a_callback = NULL;
    mv_ax_aux_a_context = NULL;
    mv_ax_aux_b_callback = NULL;
    mv_ax_aux_b_context = NULL;
    mv_ax_last_pump_tick = 0;
    mv_ax_pump_remainder = 0;
    mv_ax_ve_phase_q16 = 0;
    mv_ax_voice_lock = 0;
    mv_ax_voice_trace_count = 0;
    mv_ax_initialized = 1;
    if (mv_ax_hw_enabled) ax_audio_start();
}

void AXQuit(void)
{
    ax_audio_stop();
    memset(mv_ax_voices, 0, sizeof(mv_ax_voices));
    mv_ax_frame_callback = NULL;
    mv_ax_initialized = 0;
}

AXVPB *AXAcquireVoice(u32 priority, void (*callback)(void *), u32 userContext)
{
    ax_voice_lock_enter();
    if (!mv_ax_initialized) {
        ax_voice_lock_leave();
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
            ax_voice_lock_leave();
            return NULL;
        }
        void (*victim_callback)(void *) = mv_ax_voices[victim].vpb.callback;
        void *victim_context = (void *)(uintptr_t)mv_ax_voices[victim].vpb.userContext;
        mv_ax_voices[victim].vpb.pb.state = 0;
        if (victim_callback) {
            ax_voice_lock_leave();
            victim_callback(victim_context);
            ax_voice_lock_enter();
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
    ++mv_ax_voices[slot].decode_serial;
    mv_ax_voices[slot].decode_ready = 0;
    ax_voice_lock_leave();
    return voice;
}

void AXFreeVoice(AXVPB *voice)
{
    ax_voice_lock_enter();
    int idx = voice_index(voice);
    if (idx < 0) {
        ax_voice_lock_leave();
        return;
    }
    memset(&mv_ax_voices[idx].vpb, 0, sizeof(AXVPB));
    mv_ax_voices[idx].vpb.index = (u32)idx;
    mv_ax_voices[idx].allocated = 0;
    ++mv_ax_voices[idx].decode_serial;
    mv_ax_voices[idx].decode_ready = 0;
    ax_voice_lock_leave();
}

void AXSetVoicePriority(AXVPB *p, u32 priority)
{
    if (!p) return;
    ax_voice_lock_enter(); p->priority = (int)priority; ax_voice_lock_leave();
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
    if (!p) return; ax_voice_lock_enter(); p->pb.srcSelect = (u16)type; ax_voice_lock_leave();
}

void AXSetVoiceState(AXVPB *p, u16 state)
{
    if (!p) return;
    unsigned trace = 0;
    u32 ratio = 0, current = 0, end = 0, loop = 0;
    u16 format = 0, volume = 0, mix_l = 0, mix_r = 0;
    ax_voice_lock_enter();
    p->pb.state = state;
    mark_decode_reset(p);
    if (state && mv_ax_voice_trace_count < 16) {
        ++mv_ax_voice_trace_count;
        trace = mv_ax_voice_trace_count;
        ratio = ((u32) p->pb.src.ratioHi << 16) | p->pb.src.ratioLo;
        current = ax_addr(p->pb.addr.currentAddressHi, p->pb.addr.currentAddressLo);
        end = ax_addr(p->pb.addr.endAddressHi, p->pb.addr.endAddressLo);
        loop = ax_addr(p->pb.addr.loopAddressHi, p->pb.addr.loopAddressLo);
        format = p->pb.addr.format; volume = p->pb.ve.currentVolume;
        mix_l = p->pb.mix.vL; mix_r = p->pb.mix.vR;
    }
    ax_voice_lock_leave();
    if (trace) {
        OSReport("AX_VOICE_START n=%u idx=%u fmt=%u ratio=%08x cur=%08x end=%08x loop=%08x vol=%u mix=%u/%u\n",
                 trace, (unsigned) p->index, (unsigned) format, (unsigned) ratio,
                 (unsigned) current, (unsigned) end, (unsigned) loop,
                 (unsigned) volume, (unsigned) mix_l, (unsigned) mix_r);
    }
}

void AXSetVoiceType(AXVPB *p, u16 type)
{
    if (!p) return; ax_voice_lock_enter(); p->pb.type = type; ax_voice_lock_leave();
}

void AXSetVoiceMix(AXVPB *p, AXPBMIX *mix)
{
    if (!p || !mix) return; ax_voice_lock_enter(); p->pb.mix = *mix; ax_voice_lock_leave();
}

void AXSetVoiceItdOn(AXVPB *p)
{
    if (!p) return; ax_voice_lock_enter(); p->pb.itd.flag = 1; ax_voice_lock_leave();
}

void AXSetVoiceItdTarget(AXVPB *p, u16 lShift, u16 rShift)
{
    if (!p) return; ax_voice_lock_enter(); p->pb.itd.targetShiftL = lShift; p->pb.itd.targetShiftR = rShift; ax_voice_lock_leave();
}

void AXSetVoiceUpdateIncrement(AXVPB *p)
{
    if (!p) return; ax_voice_lock_enter(); ++p->updateCounter; ax_voice_lock_leave();
}

void AXSetVoiceUpdateWrite(AXVPB *p, u16 param, u16 data)
{
    if (!p) return; ax_voice_lock_enter();
    if (p->updateCounter < 64) { p->updateData[p->updateCounter * 2] = param; p->updateData[p->updateCounter * 2 + 1] = data; ++p->updateCounter; }
    ax_voice_lock_leave();
}

void AXSetVoiceDpop(AXVPB *p, AXPBDPOP *dpop)
{
    if (!p || !dpop) return; ax_voice_lock_enter(); p->pb.dpop = *dpop; ax_voice_lock_leave();
}

void AXSetVoiceVe(AXVPB *p, AXPBVE *ve)
{
    if (!p || !ve) return; ax_voice_lock_enter(); p->pb.ve = *ve; ax_voice_lock_leave();
}

void AXSetVoiceVeDelta(AXVPB *p, s16 delta)
{
    if (!p) return; ax_voice_lock_enter(); p->pb.ve.currentDelta = delta; ax_voice_lock_leave();
}

void AXSetVoiceFir(AXVPB *p, AXPBFIR *fir)
{
    if (!p || !fir) return; ax_voice_lock_enter(); p->pb.fir = *fir; ax_voice_lock_leave();
}

void AXSetVoiceAddr(AXVPB *p, AXPBADDR *addr)
{
    if (!p || !addr) return; ax_voice_lock_enter(); p->pb.addr = *addr; mark_decode_reset(p); ax_voice_lock_leave();
}

void AXSetVoiceLoop(AXVPB *p, u16 loop)
{
    if (!p) return; ax_voice_lock_enter(); p->pb.addr.loopFlag = loop; ax_voice_lock_leave();
}

void AXSetVoiceLoopAddr(AXVPB *p, u32 addr)
{
    if (!p) return; ax_voice_lock_enter(); split_addr(addr, &p->pb.addr.loopAddressHi, &p->pb.addr.loopAddressLo); ax_voice_lock_leave();
}

void AXSetVoiceEndAddr(AXVPB *p, u32 addr)
{
    if (!p) return; ax_voice_lock_enter(); split_addr(addr, &p->pb.addr.endAddressHi, &p->pb.addr.endAddressLo); ax_voice_lock_leave();
}

void AXSetVoiceCurrentAddr(AXVPB *p, u32 addr)
{
    if (!p) return; ax_voice_lock_enter(); split_addr(addr, &p->pb.addr.currentAddressHi, &p->pb.addr.currentAddressLo); mark_decode_reset(p); ax_voice_lock_leave();
}

void AXSetVoiceAdpcm(AXVPB *p, AXPBADPCM *adpcm)
{
    if (!p || !adpcm) return; ax_voice_lock_enter(); p->pb.adpcm = *adpcm; mark_decode_reset(p); ax_voice_lock_leave();
}

void AXSetVoiceSrc(AXVPB *p, AXPBSRC *src)
{
    if (!p || !src) return; ax_voice_lock_enter(); p->pb.src = *src; ax_voice_lock_leave();
}

void AXSetVoiceSrcRatio(AXVPB *p, float ratio)
{
    if (!p) return;
    if (ratio < 0.0f) ratio = 0.0f;
    uint32_t fixed = (uint32_t)(ratio * 65536.0f + 0.5f);
    ax_voice_lock_enter(); p->pb.src.ratioHi = (u16)(fixed >> 16); p->pb.src.ratioLo = (u16)fixed; ax_voice_lock_leave();
}

void AXSetVoiceAdpcmLoop(AXVPB *p, AXPBADPCMLOOP *adpcmloop)
{
    if (!p || !adpcmloop) return; ax_voice_lock_enter(); p->pb.adpcmLoop = *adpcmloop; ax_voice_lock_leave();
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
