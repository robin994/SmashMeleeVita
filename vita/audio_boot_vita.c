#include "audio_boot_vita.h"

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <sysdolphin/baselib/debug.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MV_ARAM_SIZE (16u * 1024u * 1024u)
#define MV_ARAM_BASE 0x4000u

static uint8_t *aram_raw;
static uint8_t *aram;
static uint32_t ar_stack_pointer;
static u32 *ar_block_lengths;
static uint32_t ar_free_blocks;
static int ar_initialized;
static int arq_initialized;
static uint32_t arq_chunk_size = 0x1000;
static ARQCallback ar_dma_callback;

static int ai_initialized;
static int ai_dma_enabled;
static uint32_t ai_dma_start;
static uint32_t ai_dma_length;
static AIDCallback ai_dma_callback;
static AISCallback ai_stream_callback;
static uint32_t ai_stream_count;
static uint32_t ai_stream_trigger;
static uint32_t ai_stream_state;
static uint32_t ai_dsp_rate = AI_SAMPLERATE_32KHZ;
static uint32_t ai_stream_rate = AI_SAMPLERATE_48KHZ;
static uint8_t ai_volume_l = 0xff;
static uint8_t ai_volume_r = 0xff;

static uint32_t audio_banks[4];
static int audio_prefix_seen;

static uintptr_t align_up(uintptr_t value, uintptr_t align)
{
    return (value + align - 1u) & ~(align - 1u);
}

ARQCallback ARRegisterDMACallback(ARQCallback callback)
{
    ARQCallback previous = ar_dma_callback;
    ar_dma_callback = callback;
    return previous;
}

u32 ARGetDMAStatus(void)
{
    return 0;
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
    if (!ar_initialized || !aram || (mainmem_addr & 31u) ||
        (aram_addr & 31u) || (length & 31u) || length == 0 ||
        aram_addr < MV_ARAM_BASE || aram_addr > MV_ARAM_SIZE ||
        length > MV_ARAM_SIZE - aram_addr)
    {
        HSD_Panic(__FILE__, __LINE__, "invalid Vita ARAM DMA");
    }
    void *mainmem = (void *)(uintptr_t)mainmem_addr;
    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        memcpy(aram + aram_addr, mainmem, length);
    } else if (type == ARAM_DIR_ARAM_TO_MRAM) {
        memcpy(mainmem, aram + aram_addr, length);
    } else {
        HSD_Panic(__FILE__, __LINE__, "unsupported ARAM DMA direction");
    }
    if (ar_dma_callback) {
        ar_dma_callback(NULL);
    }
}

u32 ARAlloc(u32 length)
{
    if (!ar_initialized || (length & 31u) || length == 0 ||
        !ar_block_lengths || ar_free_blocks == 0 ||
        ar_stack_pointer > MV_ARAM_SIZE || length > MV_ARAM_SIZE - ar_stack_pointer)
    {
        HSD_Panic(__FILE__, __LINE__, "Vita ARAM allocation failed");
    }
    uint32_t result = ar_stack_pointer;
    ar_stack_pointer += length;
    *ar_block_lengths++ = length;
    --ar_free_blocks;
    return result;
}

u32 ARFree(u32 *length)
{
    if (!ar_initialized || !ar_block_lengths || ar_stack_pointer <= MV_ARAM_BASE) {
        HSD_Panic(__FILE__, __LINE__, "Vita ARAM free underflow");
    }
    --ar_block_lengths;
    uint32_t released = *ar_block_lengths;
    if (released > ar_stack_pointer - MV_ARAM_BASE) {
        HSD_Panic(__FILE__, __LINE__, "Vita ARAM free corrupt stack");
    }
    ar_stack_pointer -= released;
    ++ar_free_blocks;
    if (length) {
        *length = released;
    }
    return ar_stack_pointer;
}

int ARCheckInit(void)
{
    return ar_initialized;
}

u32 ARInit(u32 *stack_index_addr, u32 num_entries)
{
    if (ar_initialized) {
        return MV_ARAM_BASE;
    }
    if (!stack_index_addr || num_entries == 0) {
        HSD_Panic(__FILE__, __LINE__, "invalid ARInit stack");
    }
    aram_raw = malloc(MV_ARAM_SIZE + 31u);
    if (!aram_raw) {
        HSD_Panic(__FILE__, __LINE__, "Vita ARAM allocation failed");
    }
    aram = (uint8_t *)align_up((uintptr_t)aram_raw, 32u);
    memset(aram, 0, MV_ARAM_SIZE);
    ar_stack_pointer = MV_ARAM_BASE;
    ar_block_lengths = stack_index_addr;
    ar_free_blocks = num_entries;
    ar_initialized = 1;
    return MV_ARAM_BASE;
}

void ARReset(void)
{
    free(aram_raw);
    aram_raw = NULL;
    aram = NULL;
    ar_stack_pointer = 0;
    ar_block_lengths = NULL;
    ar_free_blocks = 0;
    ar_initialized = 0;
}

void ARSetSize(void) {}
u32 ARGetBaseAddress(void) { return MV_ARAM_BASE; }
u32 ARGetSize(void) { return ar_initialized ? MV_ARAM_SIZE : 0; }

void ARQInit(void)
{
    if (!ar_initialized) {
        HSD_Panic(__FILE__, __LINE__, "ARQInit before ARInit");
    }
    arq_initialized = 1;
}

void ARQReset(void) { arq_initialized = 0; }

void ARQPostRequest(ARQRequest *request, u32 owner, u32 type, u32 priority,
                    u32 source, u32 dest, u32 length, ARQCallback callback)
{
    if (!arq_initialized || !request ||
        (priority != ARQ_PRIORITY_LOW && priority != ARQ_PRIORITY_HIGH))
    {
        HSD_Panic(__FILE__, __LINE__, "invalid synchronous Vita ARQ request");
    }
    request->next = NULL;
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) {
        ARStartDMA(type, source, dest, length);
    } else if (type == ARQ_TYPE_ARAM_TO_MRAM) {
        ARStartDMA(type, dest, source, length);
    } else {
        HSD_Panic(__FILE__, __LINE__, "unsupported Vita ARQ direction");
    }
    if (callback) {
        callback(request);
    }
}

void ARQRemoveRequest(ARQRequest *request) { (void)request; }
void ARQRemoveOwnerRequest(u32 owner) { (void)owner; }
void ARQFlushQueue(void) {}
void ARQSetChunkSize(u32 size)
{
    if (!size || (size & 31u)) {
        HSD_Panic(__FILE__, __LINE__, "invalid ARQ chunk size");
    }
    arq_chunk_size = size;
}
u32 ARQGetChunkSize(void) { return arq_chunk_size; }

int mv_aram_clear(uint32_t dest, uint32_t size)
{
    if (!ar_initialized || !aram || !size || dest < MV_ARAM_BASE ||
        dest > MV_ARAM_SIZE || size > MV_ARAM_SIZE - dest)
    {
        return -1;
    }
    memset(aram + dest, 0, size);
    return 0;
}

AIDCallback AIRegisterDMACallback(AIDCallback callback)
{
    AIDCallback previous = ai_dma_callback;
    ai_dma_callback = callback;
    return previous;
}

void AIInitDMA(u32 start_addr, u32 length)
{
    if (!ai_initialized || (start_addr & 31u) || (length & 31u) || !length) {
        HSD_Panic(__FILE__, __LINE__, "invalid Vita AI DMA buffer");
    }
    ai_dma_start = start_addr;
    ai_dma_length = length;
}
BOOL AIGetDMAEnableFlag(void) { return ai_dma_enabled; }
void AIStartDMA(void) { ai_dma_enabled = 1; }
void AIStopDMA(void) { ai_dma_enabled = 0; }
u32 AIGetDMABytesLeft(void) { return ai_dma_enabled ? ai_dma_length : 0; }
u32 AIGetDMAStartAddr(void) { return ai_dma_start; }
u32 AIGetDMALength(void) { return ai_dma_length; }
BOOL AICheckInit(void) { return ai_initialized; }
AISCallback AIRegisterStreamCallback(AISCallback callback)
{
    AISCallback previous = ai_stream_callback;
    ai_stream_callback = callback;
    return previous;
}
u32 AIGetStreamSampleCount(void) { return ai_stream_count; }
void AIResetStreamSampleCount(void) { ai_stream_count = 0; }
void AISetStreamTrigger(u32 trigger) { ai_stream_trigger = trigger; }
u32 AIGetStreamTrigger(void) { return ai_stream_trigger; }
void AISetStreamPlayState(u32 state) { ai_stream_state = state; }
u32 AIGetStreamPlayState(void) { return ai_stream_state; }
void AISetDSPSampleRate(u32 rate)
{
    if (rate != AI_SAMPLERATE_32KHZ && rate != AI_SAMPLERATE_48KHZ)
        HSD_Panic(__FILE__, __LINE__, "unsupported AI DSP rate");
    ai_dsp_rate = rate;
}
u32 AIGetDSPSampleRate(void) { return ai_dsp_rate; }
void AISetStreamSampleRate(u32 rate)
{
    if (rate != AI_SAMPLERATE_32KHZ && rate != AI_SAMPLERATE_48KHZ)
        HSD_Panic(__FILE__, __LINE__, "unsupported AI stream rate");
    ai_stream_rate = rate;
}
u32 AIGetStreamSampleRate(void) { return ai_stream_rate; }
void AISetStreamVolLeft(uint8_t vol) { ai_volume_l = vol; }
uint8_t AIGetStreamVolLeft(void) { return ai_volume_l; }
void AISetStreamVolRight(uint8_t vol) { ai_volume_r = vol; }
uint8_t AIGetStreamVolRight(void) { return ai_volume_r; }
void AIInit(uint8_t *stack)
{
    (void)stack;
    ai_initialized = 1;
    ai_dma_enabled = 0;
    ai_dma_start = ai_dma_length = 0;
    ai_stream_count = ai_stream_trigger = ai_stream_state = 0;
    ai_dsp_rate = AI_SAMPLERATE_32KHZ;
    ai_stream_rate = AI_SAMPLERATE_48KHZ;
    ai_volume_l = ai_volume_r = 0xff;
}
void AIReset(void) { ai_initialized = 0; ai_dma_enabled = 0; }

void mv_audio_boot_record(uint32_t bank_base, uint32_t bank_common,
                          uint32_t bank_priority, uint32_t bank_total)
{
    audio_banks[0] = bank_base;
    audio_banks[1] = bank_common;
    audio_banks[2] = bank_priority;
    audio_banks[3] = bank_total;
    audio_prefix_seen = 1;
}

int mv_audio_boot_validate(uint32_t out[MV_AUDIO_BOOT_STAT_COUNT])
{
    static uint8_t dma_src[32] __attribute__((aligned(32)));
    static uint8_t dma_dst[32] __attribute__((aligned(32)));
    if (!out || !audio_prefix_seen || !ar_initialized || !arq_initialized ||
        !ai_initialized || ARGetSize() != MV_ARAM_SIZE || ARGetBaseAddress() != MV_ARAM_BASE ||
        audio_banks[3] != audio_banks[0] + audio_banks[1] + audio_banks[2])
        return -1;

    for (unsigned i = 0; i < sizeof(dma_src); ++i) dma_src[i] = (uint8_t)(i ^ 0x5a);
    memset(dma_dst, 0, sizeof(dma_dst));
    u32 ar_addr = ARAlloc(sizeof(dma_src));
    ARStartDMAWrite((uint32_t)(uintptr_t)dma_src, ar_addr, sizeof(dma_src));
    ARStartDMARead((uint32_t)(uintptr_t)dma_dst, ar_addr, sizeof(dma_dst));
    u32 released = 0;
    ARFree(&released);
    if (released != sizeof(dma_src) || memcmp(dma_src, dma_dst, sizeof(dma_src)) != 0)
        return -2;

    out[MV_AUDIO_AR_BASE] = ARGetBaseAddress();
    out[MV_AUDIO_AR_SIZE] = ARGetSize();
    out[MV_AUDIO_ARQ_CHUNK] = ARQGetChunkSize();
    out[MV_AUDIO_AI_RATE] = AIGetDSPSampleRate();
    out[MV_AUDIO_BANK_BASE] = audio_banks[0];
    out[MV_AUDIO_BANK_COMMON] = audio_banks[1];
    out[MV_AUDIO_BANK_PRIORITY] = audio_banks[2];
    out[MV_AUDIO_BANK_TOTAL] = audio_banks[3];
    return 0;
}

