#include "memory.h"

#include <Runtime/platform.h>

#include "debug.h"
#include "initialize.h"
#include <dolphin/os/OSAlloc.h>

void HSD_Free(void* ptr)
{
    OSFreeToHeap(HSD_GetHeap(), ptr);
}

void* HSD_MemAlloc(ssize_t size)
{
    void* adr;

    if (size <= 0) {
        return NULL;
    }

    adr = OSAllocFromHeap(HSD_GetHeap(), size);
#ifdef MELEE_VITA_PLATFORM
    if (adr == NULL) {
        OSHeapHandle heap = HSD_GetHeap();
        OSReport("VITA_HSD_ALLOC_FAIL size=%ld heap=%d free=%ld\n",
                 (long) size, heap, OSCheckHeap(heap));
    }
#endif
    HSD_ASSERT(52, adr);

    return adr;
}
