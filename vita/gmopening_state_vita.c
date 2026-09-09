#include <Runtime/platform.h>

/* gmboot only needs the opening selector byte from gmopeningmode.c. Keeping
 * that state here avoids linking the entire opening movie scene before the
 * bounded GM_BOOT smoke test has even entered GS_MEMCARD. */
static u8 opening_selector;

void gm_801BF708(s8 value)
{
    opening_selector = (u8)value;
}

u8 gm_801BF718(void)
{
    return opening_selector;
}
