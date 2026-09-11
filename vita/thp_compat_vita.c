#include <dolphin/thp/thp.h>
#include <string.h>

/* The retail THP decoder is paired-single PowerPC assembly.  Keep the API
 * linkable on ARM without pretending those instructions were ported.  The
 * native opening movie uses thp_jpeg.c; retail THP scenes remain a bounded
 * unsupported frontier and fail closed rather than executing PPC code. */
void THPInit(void) {}

s32 THPDec_8032F8D4(u8 *data, THPDec_8032FD40_Data *out)
{
    (void)data;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

s32 THPDec_8032FD40(THPDec_8032FD40_Data *data, u16 height)
{
    (void)data; (void)height;
    return 32;
}

s32 THPVideoDecode(void *file, void *tileY, void *tileU, void *tileV, void *work)
{
    (void)file; (void)tileY; (void)tileU; (void)tileV; (void)work;
    return -1;
}

void THPDec_80331340(s32 decoded, void *y, void *u, void *v)
{
    (void)decoded; (void)y; (void)u; (void)v;
}

void THPDec_803313D0(s32 decoded, void *y, void *u, void *v, u32 width)
{
    (void)decoded; (void)y; (void)u; (void)v; (void)width;
}
