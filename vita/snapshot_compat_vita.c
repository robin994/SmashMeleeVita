#include <Runtime/platform.h>

/* GameCube screenshot/JPEG encoder support is not part of the gameplay
 * renderer. Keep snapshot calls bounded on Vita instead of linking MCC/FIO
 * and the PowerPC-era JPEG encoder state. */
s32 hsd_803B51C8(s32 image, s32 width, s32 height, char *name, s32 quality)
{
    (void)image; (void)width; (void)height; (void)name; (void)quality;
    return -1;
}

void hsd_803B5C2C(s32 arg0) { (void)arg0; }

/* GameCube MCC/FIO host-debug channel has no Vita equivalent. */
int hsd_80392E80(void) { return 0; }
int hsd_80393A5C(char *filename, int data, int size)
{ (void)filename; (void)data; (void)size; return -1; }
