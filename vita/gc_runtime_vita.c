#include "gc_runtime_vita.h"
#include "gx_boot_vita.h"
#include "audio_boot_vita.h"

#include <dolphin/ar.h>
#include <dolphin/card.h>
#include <dolphin/card/CARDStat.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <melee/lb/lbfile.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lblanguage.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/devcom.h>

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MV_GC_SIM_MEM_BYTES (24u * 1024u * 1024u)
#define MV_GC_ARENA_BYTES   (20u * 1024u * 1024u)
#define MV_GC_TIMER_HZ      40500000ull
#define MV_DVD_MAX_ENTRIES  512u
#define MV_OS_HEAP_MAX      8
#define MV_OS_HEAP_ALIGN    32u

u32 __OSBusClock = 162000000u;
u32 __OSCoreClock = 486000000u;

static uint8_t *arena_raw;
static uint8_t *arena_lo;
static uint8_t *arena_hi;
static int vi_ready;
static int dvd_ready;
static int card_ready;
static int alarm_ready;
static unsigned critical_depth;
static u32 sound_mode = OS_SOUND_MODE_STEREO;
static OSAlarm *alarm_head;
static int hsd_card_runtime_ready;
static u32 reset_code;

/* synth.c owns this handle in the original runtime. HSD_OSInit writes it
 * before HSD_SynthInit starts allocating from the audio heap. */
extern OSHeapHandle HSD_Synth_804D6018;

typedef struct MvHeapBlock {
    uint32_t size;
    uint32_t free;
    struct MvHeapBlock *next;
    struct MvHeapBlock *prev;
    uint8_t reserved[16];
} MvHeapBlock;

typedef struct {
    uint8_t *start;
    uint8_t *end;
    MvHeapBlock *head;
    int active;
} MvOsHeap;

_Static_assert(sizeof(MvHeapBlock) == MV_OS_HEAP_ALIGN,
               "Vita OS heap block header must preserve 32-byte alignment");

static MvOsHeap os_heaps[MV_OS_HEAP_MAX];
static int os_heap_limit;
volatile OSHeapHandle __OSCurrHeap = -1;

typedef struct {
    s32 id;
    u32 size;
    char path[512];
} MvDvdEntry;

static MvDvdEntry dvd_entries[MV_DVD_MAX_ENTRIES];
static unsigned dvd_entry_count;
static int devcom_request_id = 4;
static DVDDiskID current_disk_id = {
    .gameName = {'G', 'A', 'L', 'E'},
    .company = {'0', '1'},
    .diskNumber = 0,
    .gameVersion = 2,
    .streaming = 0,
    .streamingBufSize = 0,
};

static uintptr_t align_up(uintptr_t value, uint32_t align)
{
    return (value + (align - 1u)) & ~(uintptr_t)(align - 1u);
}

static uintptr_t align_down(uintptr_t value, uint32_t align)
{
    return value & ~(uintptr_t)(align - 1u);
}

void OSInit(void)
{
    if (arena_raw) return;
    arena_raw = malloc(MV_GC_ARENA_BYTES + 31u);
    if (!arena_raw) return;
    arena_lo = (uint8_t *)align_up((uintptr_t)arena_raw, 32);
    arena_hi = arena_lo + MV_GC_ARENA_BYTES;
}

void *OSGetArenaHi(void)
{
    return arena_hi;
}

void *OSGetArenaLo(void)
{
    return arena_lo;
}

void OSSetArenaHi(void *value)
{
    uint8_t *p = value;
    if (!arena_lo || p < arena_lo || p > arena_lo + MV_GC_ARENA_BYTES) return;
    arena_hi = p;
}

void OSSetArenaLo(void *value)
{
    uint8_t *p = value;
    if (!arena_hi || p < (uint8_t *)align_up((uintptr_t)arena_raw, 32) || p > arena_hi) return;
    arena_lo = p;
}

void *OSAllocFromArenaLo(u32 size, u32 align)
{
    if (!arena_lo || !arena_hi || !align || (align & (align - 1u))) return NULL;
    uintptr_t start = align_up((uintptr_t)arena_lo, align);
    uintptr_t end = start + size;
    if (end < start || end > (uintptr_t)arena_hi) return NULL;
    arena_lo = (uint8_t *)end;
    return (void *)start;
}

void *OSAllocFromArenaHi(u32 size, u32 align)
{
    if (!arena_lo || !arena_hi || !align || (align & (align - 1u))) return NULL;
    uintptr_t hi = (uintptr_t)arena_hi;
    if (size > hi) return NULL;
    uintptr_t start = align_down(hi - size, align);
    if (start < (uintptr_t)arena_lo) return NULL;
    arena_hi = (uint8_t *)start;
    return (void *)start;
}

void *OSInitAlloc(void *arenaStart, void *arenaEnd, int maxHeaps)
{
    uintptr_t lo = align_up((uintptr_t)arenaStart, MV_OS_HEAP_ALIGN);
    uintptr_t hi = align_down((uintptr_t)arenaEnd, MV_OS_HEAP_ALIGN);
    if (!arenaStart || !arenaEnd || lo >= hi || maxHeaps <= 0) return NULL;
    memset(os_heaps, 0, sizeof(os_heaps));
    os_heap_limit = maxHeaps < MV_OS_HEAP_MAX ? maxHeaps : MV_OS_HEAP_MAX;
    __OSCurrHeap = -1;
    /* Dolphin stores allocator bookkeeping in the arena.  The Vita adapter
       keeps that metadata out-of-band, so no guest arena bytes are consumed. */
    return (void *)lo;
}

int OSCreateHeap(void *start, void *end)
{
    uintptr_t lo = align_up((uintptr_t)start, MV_OS_HEAP_ALIGN);
    uintptr_t hi = align_down((uintptr_t)end, MV_OS_HEAP_ALIGN);
    if (!start || !end || hi <= lo + sizeof(MvHeapBlock)) return -1;
    for (int i = 0; i < os_heap_limit; ++i) {
        if (os_heaps[i].active) continue;
        MvHeapBlock *head = (MvHeapBlock *)lo;
        memset(head, 0, sizeof(*head));
        head->size = (uint32_t)(hi - lo - sizeof(*head));
        head->free = 1;
        os_heaps[i].start = (uint8_t *)lo;
        os_heaps[i].end = (uint8_t *)hi;
        os_heaps[i].head = head;
        os_heaps[i].active = 1;
        return i;
    }
    return -1;
}

void OSDestroyHeap(int heap)
{
    if (heap < 0 || heap >= os_heap_limit || !os_heaps[heap].active) return;
    memset(&os_heaps[heap], 0, sizeof(os_heaps[heap]));
    if (__OSCurrHeap == heap) __OSCurrHeap = -1;
}

int OSSetCurrentHeap(int heap)
{
    int previous = __OSCurrHeap;
    if (heap >= 0 && heap < os_heap_limit && os_heaps[heap].active)
        __OSCurrHeap = heap;
    return previous;
}

int OSGetCurrentHeap(void)
{
    return __OSCurrHeap;
}

static int valid_heap(int heap)
{
    return heap >= 0 && heap < os_heap_limit && os_heaps[heap].active;
}

void *OSAllocFromHeap(int heap, u32 size)
{
    if (!valid_heap(heap) || size == 0 || size > UINT32_MAX - 31u) return NULL;
    uint32_t wanted = (uint32_t)align_up(size, MV_OS_HEAP_ALIGN);
    for (MvHeapBlock *block = os_heaps[heap].head; block; block = block->next) {
        if (!block->free || block->size < wanted) continue;
        uint32_t remain = block->size - wanted;
        if (remain >= sizeof(MvHeapBlock) + MV_OS_HEAP_ALIGN) {
            MvHeapBlock *split = (MvHeapBlock *)((uint8_t *)(block + 1) + wanted);
            memset(split, 0, sizeof(*split));
            split->size = remain - sizeof(MvHeapBlock);
            split->free = 1;
            split->next = block->next;
            split->prev = block;
            if (split->next) split->next->prev = split;
            block->next = split;
            block->size = wanted;
        }
        block->free = 0;
        return block + 1;
    }
    return NULL;
}

void OSFreeToHeap(int heap, void *ptr)
{
    if (!valid_heap(heap) || !ptr) return;
    MvOsHeap *h = &os_heaps[heap];
    if ((uint8_t *)ptr < h->start + sizeof(MvHeapBlock) ||
        (uint8_t *)ptr >= h->end)
        return;
    MvHeapBlock *block = (MvHeapBlock *)ptr - 1;
    block->free = 1;
    if (block->next && block->next->free) {
        MvHeapBlock *next = block->next;
        block->size += sizeof(MvHeapBlock) + next->size;
        block->next = next->next;
        if (block->next) block->next->prev = block;
    }
    if (block->prev && block->prev->free) {
        MvHeapBlock *prev = block->prev;
        prev->size += sizeof(MvHeapBlock) + block->size;
        prev->next = block->next;
        if (prev->next) prev->next->prev = prev;
    }
}

long OSCheckHeap(int heap)
{
    if (!valid_heap(heap)) return -1;
    uint32_t free_bytes = 0;
    for (MvHeapBlock *block = os_heaps[heap].head; block; block = block->next) {
        if (block->free) free_bytes += block->size;
    }
    return (long)free_bytes;
}

u32 OSGetPhysicalMemSize(void)
{
    return MV_GC_SIM_MEM_BYTES;
}

u32 OSGetConsoleSimulatedMemSize(void)
{
    return MV_GC_SIM_MEM_BYTES;
}

OSTick OSGetTick(void)
{
    uint64_t usec = sceKernelGetProcessTimeWide();
    return (OSTick)((usec * MV_GC_TIMER_HZ) / 1000000ull);
}

OSTime OSGetTime(void)
{
    uint64_t usec = sceKernelGetProcessTimeWide();
    return (OSTime)((usec * MV_GC_TIMER_HZ) / 1000000ull);
}

static int mv_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int mv_leap_days_before(int year)
{
    if (year < 1) return 0;
    return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime *td)
{
    static const int year_days[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    static const int leap_year_days[12] = {
        0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335
    };
    const OSTime ticks_per_second = (OSTime)OS_TIMER_CLOCK;
    OSTime subsecond = ticks % ticks_per_second;
    if (subsecond < 0) subsecond += ticks_per_second;
    td->usec = (int)((subsecond * 1000000ll / ticks_per_second) % 1000);
    td->msec = (int)((subsecond * 1000ll / ticks_per_second) % 1000);
    ticks -= subsecond;

    s64 seconds = ticks / ticks_per_second;
    int days = (int)(seconds / 86400ll) + 0xB2575;
    int secs = (int)(seconds % 86400ll);
    if (secs < 0) {
        --days;
        secs += 86400;
    }

    td->wday = (days + 6) % 7;
    int year = days / 365;
    int year_start;
    while (days < (year_start = year * 365 + mv_leap_days_before(year))) --year;
    days -= year_start;
    td->year = year;
    td->yday = days;

    const int *months = mv_is_leap_year(year) ? leap_year_days : year_days;
    int month = 12;
    while (days < months[--month]) {}
    td->mon = month;
    td->mday = days - months[month] + 1;
    td->hour = secs / 3600;
    td->min = (secs / 60) % 60;
    td->sec = secs % 60;
}

BOOL OSDisableInterrupts(void)
{
    BOOL previous = critical_depth == 0;
    ++critical_depth;
    return previous;
}

BOOL OSRestoreInterrupts(BOOL level)
{
    if (critical_depth) --critical_depth;
    if (level) critical_depth = 0;
    return level;
}

void VIInit(void)
{
    vi_ready = 1;
}

void VIWaitForRetrace(void)
{
    if (!vi_ready) VIInit();
    sceDisplayWaitVblankStart();
    mv_vi_tick();
    OSTime now = OSGetTime();
    for (OSAlarm *alarm = alarm_head; alarm != NULL;) {
        OSAlarm *next = alarm->next;
        if (alarm->handler && now >= alarm->fire) {
            OSAlarmHandler handler = alarm->handler;
            if (alarm->period > 0) {
                do {
                    alarm->fire += alarm->period;
                } while (alarm->fire <= now);
            } else {
                OSCancelAlarm(alarm);
            }
            handler(alarm, NULL);
        }
        alarm = next;
    }
}

void DVDInit(void)
{
    dvd_ready = 1;
}

static int make_dvd_path(const char *gc_path, char out[512])
{
    static const char root[] = "ux0:data/SmashMeleeVita/files";
    if (!gc_path || strstr(gc_path, "..")) return -1;
    while (*gc_path == '/') ++gc_path;
    size_t root_len = sizeof(root) - 1u;
    size_t path_len = strlen(gc_path);
    if (root_len + 1u + path_len + 1u > 512u) return -1;
    memcpy(out, root, root_len);
    out[root_len] = '/';
    memcpy(out + root_len + 1u, gc_path, path_len + 1u);
    return 0;
}

static MvDvdEntry *dvd_entry_by_id(s32 id)
{
    for (unsigned i = 0; i < dvd_entry_count; ++i)
        if (dvd_entries[i].id == id) return &dvd_entries[i];
    return NULL;
}

static MvDvdEntry *dvd_entry_by_path(const char *path)
{
    for (unsigned i = 0; i < dvd_entry_count; ++i)
        if (!strcmp(dvd_entries[i].path, path)) return &dvd_entries[i];
    return NULL;
}

s32 DVDConvertPathToEntrynum(const char *path)
{
    if (!dvd_ready) DVDInit();
    char native_path[512];
    SceIoStat st;
    if (make_dvd_path(path, native_path) < 0 || sceIoGetstat(native_path, &st) < 0) return -1;

    MvDvdEntry *entry = dvd_entry_by_path(native_path);
    if (!entry) {
        if (dvd_entry_count >= MV_DVD_MAX_ENTRIES) return -1;
        entry = &dvd_entries[dvd_entry_count++];
        memset(entry, 0, sizeof(*entry));
        entry->id = (s32)dvd_entry_count;
        memcpy(entry->path, native_path, strlen(native_path) + 1u);
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) return -1;
    entry->size = (u32)st.st_size;
    return entry->id;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo *fileInfo)
{
    MvDvdEntry *entry = dvd_entry_by_id(entrynum);
    if (!entry || !fileInfo) return 0;
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = (u32)entrynum;
    fileInfo->length = entry->size;
    fileInfo->cb.state = DVD_STATE_END;
    return 1;
}

BOOL DVDOpen(char *fileName, DVDFileInfo *fileInfo)
{
    s32 entry = DVDConvertPathToEntrynum(fileName);
    return entry >= 0 ? DVDFastOpen(entry, fileInfo) : 0;
}

BOOL DVDClose(DVDFileInfo *fileInfo)
{
    if (!fileInfo) return 0;
    memset(fileInfo, 0, sizeof(*fileInfo));
    return 1;
}

long DVDReadPrio(DVDFileInfo *fileInfo, void *addr, long length, long offset,
                 long prio)
{
    (void)prio;
    if (!fileInfo || !addr || length < 0 || offset < 0) return DVD_RESULT_FATAL_ERROR;
    MvDvdEntry *entry = dvd_entry_by_id((s32)fileInfo->startAddr);
    if (!entry || (uint64_t)offset > entry->size) return DVD_RESULT_FATAL_ERROR;

    size_t available = entry->size - (size_t)offset;
    size_t wanted = (size_t)length;
    size_t to_read = wanted < available ? wanted : available;
    size_t done = 0;

    /* Prefer native Vita I/O. If it fails for an extracted regular file,
       retry through newlib stdio, which is already used successfully by the
       movie/title/menu asset path on hardware. */
    int native_failed = 0;
    SceUID fd = sceIoOpen(entry->path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        native_failed = 1;
    } else {
        if (sceIoLseek(fd, offset, SCE_SEEK_SET) < 0) {
            native_failed = 2;
        } else {
            while (done < to_read) {
                int result = sceIoRead(fd, (uint8_t *)addr + done,
                                       to_read - done);
                if (result <= 0) {
                    native_failed = 3;
                    break;
                }
                done += (size_t)result;
            }
        }
        sceIoClose(fd);
    }

    if (native_failed) {
        FILE *stream = fopen(entry->path, "rb");
        if (!stream || fseek(stream, offset, SEEK_SET)) {
            if (stream) fclose(stream);
            OSReport("DVD_READ_FAIL path=%s backend=sceIo+stdio native_stage=%d "
                     "offset=%ld size=%ld\n",
                     entry->path, native_failed, offset, length);
            return DVD_RESULT_FATAL_ERROR;
        }
        done = 0;
        while (done < to_read) {
            size_t got = fread((uint8_t *)addr + done, 1, to_read - done,
                               stream);
            if (!got) {
                int failed = ferror(stream);
                fclose(stream);
                OSReport("DVD_READ_FAIL path=%s backend=stdio native_stage=%d "
                         "offset=%ld size=%ld ferror=%d done=%u/%u\n",
                         entry->path, native_failed, offset, length, failed,
                         (unsigned)done, (unsigned)to_read);
                return DVD_RESULT_FATAL_ERROR;
            }
            done += got;
        }
        fclose(stream);
        OSReport("DVD_READ_FALLBACK_PASS path=%s native_stage=%d offset=%ld "
                 "size=%ld\n",
                 entry->path, native_failed, offset, length);
    }

    /* DVD requests are 32-byte rounded and may legally extend past the file's
     * logical length into sector padding.  Extracted files do not contain that
     * padding, so materialize it deterministically as zeroes. */
    if (done < wanted) memset((uint8_t *)addr + done, 0, wanted - done);
    fileInfo->cb.transferredSize = (u32)wanted;
    fileInfo->cb.state = DVD_STATE_END;
    return length;
}

BOOL DVDReadAsyncPrio(DVDFileInfo *fileInfo, void *addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio)
{
    long result = DVDReadPrio(fileInfo, addr, length, offset, prio);
    if (callback) callback((s32)result, fileInfo);
    return result >= 0 ? 1 : 0;
}

long DVDGetFileInfoStatus(DVDFileInfo *fileInfo)
{
    return fileInfo ? fileInfo->cb.state : DVD_STATE_FATAL_ERROR;
}

void CARDInit(void)
{
    sceIoMkdir("ux0:data/SmashMeleeVita/save", 0777);
    card_ready = 1;
}

static s32 mv_card_channel_result(s32 chan)
{
    if (!card_ready) CARDInit();
    return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}

s32 CARDMountAsync(s32 chan, void *workArea, CARDCallback detachCallback,
                   CARDCallback attachCallback)
{
    (void)workArea;
    (void)detachCallback;
    s32 result = mv_card_channel_result(chan);
    if (attachCallback) attachCallback(chan, result);
    return result;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback)
{
    s32 result = mv_card_channel_result(chan);
    if (callback) callback(chan, result);
    return result;
}

s32 CARDFreeBlocks(s32 chan, s32 *byteNotUsed, s32 *filesNotUsed)
{
    s32 result = mv_card_channel_result(chan);
    if (result != CARD_RESULT_READY) return result;
    if (byteNotUsed) *byteNotUsed = (16 * 1024 * 1024 / 8) - (5 * 8192);
    if (filesNotUsed) *filesNotUsed = CARD_MAX_FILE;
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, char *fileName, CARDFileInfo *fileInfo)
{
    if (mv_card_channel_result(chan) != CARD_RESULT_READY)
        return CARD_RESULT_NOCARD;
    if (!fileName || !fileInfo) return CARD_RESULT_FATAL_ERROR;

    char path[512];
    snprintf(path, sizeof(path), "ux0:data/SmashMeleeVita/save/%s", fileName);
    SceIoStat stat;
    if (sceIoGetstat(path, &stat) < 0) return CARD_RESULT_NOFILE;
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->chan = chan;
    fileInfo->fileNo = 0;
    fileInfo->length = (s32)stat.st_size;
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo *fileInfo)
{
    return fileInfo ? CARD_RESULT_READY : CARD_RESULT_FATAL_ERROR;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat *stat)
{
    (void)fileNo;
    (void)stat;
    return mv_card_channel_result(chan) == CARD_RESULT_READY ? CARD_RESULT_NOFILE
                                                             : CARD_RESULT_NOCARD;
}

s32 CARDUnmount(s32 chan)
{
    return mv_card_channel_result(chan);
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback)
{
    s32 result = mv_card_channel_result(chan) == CARD_RESULT_READY
                     ? CARD_RESULT_IOERROR
                     : CARD_RESULT_NOCARD;
    if (callback) callback(chan, result);
    return result;
}

s32 CARDDeleteAsync(s32 chan, char *fileName, CARDCallback callback)
{
    (void)fileName;
    s32 result = mv_card_channel_result(chan) == CARD_RESULT_READY
                     ? CARD_RESULT_NOFILE
                     : CARD_RESULT_NOCARD;
    if (callback) callback(chan, result);
    return result;
}

s32 CARDRenameAsync(s32 chan, const char *oldName, const char *newName,
                    CARDCallback callback)
{
    (void)oldName;
    (void)newName;
    s32 result = mv_card_channel_result(chan) == CARD_RESULT_READY
                     ? CARD_RESULT_NOFILE
                     : CARD_RESULT_NOCARD;
    if (callback) callback(chan, result);
    return result;
}

DVDDiskID *DVDGetCurrentDiskID(void)
{
    return &current_disk_id;
}

/* The original HSD CARD layer resets its asynchronous command queue here.
 * Vita has no GameCube CARD queue, so resetting the logical backend is the
 * equivalent initialization step; no file operation is reported successful. */
void hsd_803B2374(void)
{
    hsd_card_runtime_ready = 1;
}

s32 CARDProbeEx(s32 chan, s32 *memSize, s32 *sectorSize)
{
    if (!card_ready) CARDInit();
    if (chan != 0) return -3;
    if (memSize) *memSize = 16;
    if (sectorSize) *sectorSize = 8192;
    return 0;
}

int CARDProbe(long chan)
{
    return CARDProbeEx((s32)chan, NULL, NULL);
}

void OSInitAlarm(void)
{
    alarm_head = NULL;
    alarm_ready = 1;
}

void OSCreateAlarm(OSAlarm *alarm)
{
    if (!alarm_ready) OSInitAlarm();
    if (!alarm) return;
    memset(alarm, 0, sizeof(*alarm));
}

static void alarm_link(OSAlarm *alarm)
{
    if (!alarm) return;
    OSCancelAlarm(alarm);
    alarm->prev = NULL;
    alarm->next = alarm_head;
    if (alarm_head) alarm_head->prev = alarm;
    alarm_head = alarm;
}

void OSSetAlarm(OSAlarm *alarm, OSTime tick, OSAlarmHandler handler)
{
    if (!alarm || !handler) return;
    alarm->handler = handler;
    alarm->period = 0;
    alarm->start = OSGetTime();
    alarm->fire = alarm->start + tick;
    alarm_link(alarm);
}

void OSSetAbsAlarm(OSAlarm *alarm, OSTime time, OSAlarmHandler handler)
{
    if (!alarm || !handler) return;
    alarm->handler = handler;
    alarm->period = 0;
    alarm->start = time;
    alarm->fire = time;
    alarm_link(alarm);
}

void OSSetPeriodicAlarm(OSAlarm *alarm, OSTime start, OSTime period,
                        OSAlarmHandler handler)
{
    if (!alarm || !handler || period <= 0) return;
    OSTime now = OSGetTime();
    alarm->handler = handler;
    alarm->period = period;
    alarm->start = now + start;
    alarm->fire = alarm->start;
    alarm_link(alarm);
}

void OSCancelAlarm(OSAlarm *alarm)
{
    if (!alarm) return;
    if (alarm->prev) alarm->prev->next = alarm->next;
    else if (alarm_head == alarm) alarm_head = alarm->next;
    if (alarm->next) alarm->next->prev = alarm->prev;
    alarm->prev = NULL;
    alarm->next = NULL;
}

BOOL OSCheckAlarmQueue(void)
{
    return alarm_head != NULL;
}

BOOL OSGetResetSwitchState(void)
{
    return false;
}

void OSResetSystem(int reset, u32 code, BOOL forceMenu)
{
    (void)forceMenu;
    reset_code = code;
    sceKernelExitProcess(reset ? 1 : 0);
}

void lb_800192A8(void (*cb)(void))
{
    /* Vita DVD reads complete synchronously, so the GameCube drive-state pump
     * collapses to the periodic callback used by Melee's wait loops. */
    if (cb) cb();
}

bool HSD_DevComIsBusy(int idx)
{
    (void)idx;
    return false;
}

int HSD_DevComCancelEx(int dcReq, u32 flags, HSD_DevComCallback cb, void *args)
{
    (void)dcReq;
    (void)flags;
    (void)cb;
    (void)args;
    /* All requests in this adapter finish inside HSD_DevComRequest(), so there
     * is no outstanding queue entry left to cancel. Upstream also returns 0
     * when the requested id is no longer present. */
    return 0;
}

int HSD_DevComRequest(int file, uintptr_t src, uintptr_t dest, size_t size,
                      int type, int pri, HSD_DevComCallback callback, void *args)
{
    static u8 relay[0x4000] __attribute__((aligned(32)));
    static ARQRequest relay_request;
    (void)pri;
    int req = devcom_request_id;
    devcom_request_id += 4;
    if (type == 3) {
        if (mv_aram_clear((uint32_t)dest, (uint32_t)size) != 0) {
            if (callback) callback(req, (int)(intptr_t)args, NULL, true);
            return -1;
        }
        if (callback) callback(req, (int)(intptr_t)args, NULL, false);
        return req;
    }
    if (type != 0x21 && type != 0x22 && type != 0x23) {
        OSReport("DEVCOM_FAIL reason=type file=%d src=%08lx dest=%08lx size=%u "
                 "type=%02x pri=%d\n",
                 file, (unsigned long)src, (unsigned long)dest,
                 (unsigned)size, type, pri);
        if (callback) callback(req, (int)(intptr_t)args, NULL, true);
        return -1;
    }
    if ((type != 0x22 && !dest) || !size) {
        OSReport("DEVCOM_FAIL reason=zero file=%d src=%08lx dest=%08lx size=%u "
                 "type=%02x pri=%d\n",
                 file, (unsigned long)src, (unsigned long)dest,
                 (unsigned)size, type, pri);
        if (callback) callback(req, (int)(intptr_t)args, NULL, true);
        return -1;
    }
    if ((src & 31u) || (type != 0x22 && (dest & 31u)) || (size & 31u)) {
        OSReport("DEVCOM_FAIL reason=alignment file=%d src=%08lx dest=%08lx "
                 "size=%u type=%02x pri=%d\n",
                 file, (unsigned long)src, (unsigned long)dest,
                 (unsigned)size, type, pri);
        if (callback) callback(req, (int)(intptr_t)args, NULL, true);
        return -1;
    }
    if (type == 0x22 && size > sizeof(relay)) {
        OSReport("DEVCOM_FAIL reason=sbuf_size file=%d src=%08lx size=%u max=%u "
                 "type=%02x pri=%d\n",
                 file, (unsigned long)src, (unsigned)size,
                 (unsigned)sizeof(relay), type, pri);
        if (callback) callback(req, (int)(intptr_t)args, NULL, true);
        return -1;
    }

    DVDFileInfo info;
    if (!DVDFastOpen(file, &info)) {
        OSReport("DEVCOM_FAIL reason=fastopen file=%d src=%08lx dest=%08lx "
                 "size=%u type=%02x pri=%d\n",
                 file, (unsigned long)src, (unsigned long)dest,
                 (unsigned)size, type, pri);
        if (callback) callback(req, (int)(intptr_t)args, NULL, true);
        return -1;
    }

    if (type == 0x21) {
        if (DVDReadPrio(&info, (void *)dest, (long)size, (long)src, 2) < 0) {
            DVDClose(&info);
            MvDvdEntry *entry = dvd_entry_by_id(file);
            OSReport("DEVCOM_FAIL reason=dvdread file=%d path=%s src=%08lx "
                     "dest=%08lx size=%u type=%02x pri=%d\n",
                     file, entry ? entry->path : "?", (unsigned long)src,
                     (unsigned long)dest, (unsigned)size, type, pri);
            if (callback) callback(req, (int)(intptr_t)args, NULL, true);
            return -1;
        }
    } else if (type == 0x22) {
        /* DEVCOMDEST_SBUF: GameCube reads into one of DevCom's internal
         * relay buffers and passes that buffer to the completion callback.
         * A destination address of zero is therefore intentional. */
        if (!callback) {
            DVDClose(&info);
            OSReport("DEVCOM_FAIL reason=sbuf_callback file=%d src=%08lx size=%u "
                     "type=%02x pri=%d\n",
                     file, (unsigned long)src, (unsigned)size, type, pri);
            return -1;
        }
        if (DVDReadPrio(&info, relay, (long)size, (long)src, 2) < 0) {
            DVDClose(&info);
            MvDvdEntry *entry = dvd_entry_by_id(file);
            OSReport("DEVCOM_FAIL reason=dvdread_sbuf file=%d path=%s src=%08lx "
                     "size=%u type=%02x pri=%d\n",
                     file, entry ? entry->path : "?", (unsigned long)src,
                     (unsigned)size, type, pri);
            callback(req, (int)(intptr_t)args, NULL, true);
            return -1;
        }
        DVDClose(&info);
        OSReport("DEVCOM_SBUF_PASS file=%d src=%08lx size=%u type=%02x pri=%d\n",
                 file, (unsigned long)src, (unsigned)size, type, pri);
        callback(req, (int)(intptr_t)args, relay, false);
        return req;
    } else {
        /* Type 0x23 is DVD -> relay RAM -> ARAM on GameCube. ARAM addresses
           are offsets and must never be treated as CPU pointers on Vita. */
        size_t done = 0;
        while (done < size) {
            size_t chunk = size - done;
            if (chunk > sizeof(relay)) chunk = sizeof(relay);
            if (DVDReadPrio(&info, relay, (long)chunk, (long)(src + done), 2) < 0) {
                DVDClose(&info);
                MvDvdEntry *entry = dvd_entry_by_id(file);
                OSReport("DEVCOM_FAIL reason=dvdread_aram file=%d path=%s "
                         "src=%08lx dest=%08lx size=%u done=%u type=%02x pri=%d\n",
                         file, entry ? entry->path : "?",
                         (unsigned long)src, (unsigned long)dest,
                         (unsigned)size, (unsigned)done, type, pri);
                if (callback) callback(req, (int)(intptr_t)args, NULL, true);
                return -1;
            }
            ARQPostRequest(&relay_request, 0, ARQ_TYPE_MRAM_TO_ARAM,
                           ARQ_PRIORITY_LOW, (u32)(uintptr_t)relay,
                           (u32)(dest + done), (u32)chunk, NULL);
            done += chunk;
        }
    }
    DVDClose(&info);
    if (callback) callback(req, (int)(intptr_t)args, NULL, false);
    return req;
}

u32 OSGetSoundMode(void)
{
    return sound_mode;
}

void OSSetSoundMode(u32 mode)
{
    if (mode != OS_SOUND_MODE_MONO && mode != OS_SOUND_MODE_STEREO) {
        return;
    }
    sound_mode = mode;
}

u32 OSGetResetCode(void)
{
    return reset_code;
}

int mv_lbfile_boot_probe(uint32_t out[MV_LBFILE_STAT_COUNT])
{
    if (!out) return -1;
    memset(out, 0, sizeof(uint32_t) * MV_LBFILE_STAT_COUNT);
    size_t size = lbFileGetSize("MnMaAll.usd");
    if (size < 32 || size > 64u * 1024u * 1024u) return -2;
    size_t rounded = (size + 31u) & ~31u;
    uint8_t *raw = malloc(rounded + 31u);
    if (!raw) return -3;
    uint8_t *data = (uint8_t *)align_up((uintptr_t)raw, 32);
    size_t loaded = 0;
    lbFile_8001668C("MnMaAll.usd", data, &loaded);
    if (loaded != size) {
        free(raw);
        return -4;
    }
    uint32_t header_size = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
                           (uint32_t)data[2] << 8 | data[3];
    if (header_size != size) {
        free(raw);
        return -5;
    }
    uint32_t checksum = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        checksum ^= data[i];
        checksum *= 16777619u;
    }
    out[MV_LBFILE_SIZE] = (uint32_t)size;
    out[MV_LBFILE_HEADER_SIZE] = header_size;
    out[MV_LBFILE_CHECKSUM] = checksum;
    free(raw);
    return 0;
}

int mv_lbarchive_boot_probe(uint32_t out[MV_LBARCHIVE_STAT_COUNT])
{
    if (!out) return -1;
    memset(out, 0, sizeof(uint32_t) * MV_LBARCHIVE_STAT_COUNT);
    size_t size = lbFileGetSize("MnMaAll.usd");
    if (size < 32 || size > 64u * 1024u * 1024u) return -2;
    size_t rounded = (size + 31u) & ~31u;
    uint8_t *raw = malloc(rounded + 31u);
    if (!raw) return -3;
    uint8_t *data = (uint8_t *)align_up((uintptr_t)raw, 32);
    size_t loaded = 0;
    lbFile_8001668C("MnMaAll.usd", data, &loaded);
    if (loaded != size) {
        free(raw);
        return -4;
    }

    HSD_Archive archive;
    lbArchive_InitializeDAT(&archive, data, loaded);
    void *root = HSD_ArchiveGetPublicAddress(&archive, "MenMainBack_Top_joint");
    if (!root || root < (void *)archive.data) {
        free(raw);
        return -5;
    }
    uintptr_t root_offset = (uintptr_t)root - (uintptr_t)archive.data;
    if (root_offset > archive.header.data_size) {
        free(raw);
        return -6;
    }

    out[MV_LBARCHIVE_FILE_SIZE] = archive.header.file_size;
    out[MV_LBARCHIVE_PUBLICS] = archive.header.nb_public;
    out[MV_LBARCHIVE_RELOCATIONS] = archive.header.nb_reloc;
    out[MV_LBARCHIVE_EXTERNS] = archive.header.nb_extern;
    out[MV_LBARCHIVE_ROOT_OFFSET] = (uint32_t)root_offset;
    free(raw);
    return 0;
}


/* PowerPC cache maintenance is unnecessary on Vita process memory. */
void DCFlushRange(void *addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCFlushRangeNoSync(void *addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCInvalidateRange(void *addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCStoreRange(void *addr, u32 nBytes) { (void)addr; (void)nBytes; }
