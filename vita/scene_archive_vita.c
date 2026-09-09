/* Typed conversion for the camera of the original memory-card message scene.
 * Archive relocation already converts pointers. Only descriptor scalars are
 * swapped here; texture/display-list/SIS byte streams must stay big-endian. */
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/wobj.h>
#include <sysdolphin/baselib/debug.h>
#include <melee/sc/types.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

static void require_range(HSD_Archive* a, const void* p, size_t n)
{
    uintptr_t start = (uintptr_t)a->data, q = (uintptr_t)p;
    if (!p || q < start || n > a->header.data_size ||
        q - start > a->header.data_size - n)
        HSD_Panic(__FILE__, __LINE__, "scene descriptor outside archive");
}
static void swap16(void* p)
{ uint16_t v; memcpy(&v,p,2); v=__builtin_bswap16(v); memcpy(p,&v,2); }
static void swap32(void* p)
{ uint32_t v; memcpy(&v,p,4); v=__builtin_bswap32(v); memcpy(p,&v,4); }
static void vector(HSD_Archive* a, Vec3* v)
{
    require_range(a,v,sizeof(*v));
    swap32(&v->x); swap32(&v->y); swap32(&v->z);
    if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z))
        HSD_Panic(__FILE__,__LINE__,"nonfinite scene vector");
}
static void world(HSD_Archive* a, HSD_WObjDesc* w)
{
    require_range(a,w,sizeof(*w));
    if (w->class_name || w->robjdesc)
        HSD_Panic(__FILE__,__LINE__,"unsupported scene WObj class/RObj");
    vector(a,&w->pos);
}
void mv_boot_archive_prepare(HSD_Archive* a, const char* filename)
{
    /* Only this scene root is consumed by the initial OnEnter callback today.
     * Other roots retain their original scalar encoding until adapted. */
    if (strcmp(filename,"NtMsgWin.dat") != 0) return;
    SceneDesc* scene = HSD_ArchiveGetPublicAddress(a,"ScNtcCommon_scene_data");
    require_range(a,scene,sizeof(*scene));
    require_range(a,scene->cameras,2*sizeof(*scene->cameras));
    if (scene->cameras[1].desc || scene->cameras[0].anims)
        HSD_Panic(__FILE__,__LINE__,"unsupported message scene camera list");
    HSD_CObjDesc* c = scene->cameras[0].desc;
    require_range(a,c,0x30);
    if (c->class_name) HSD_Panic(__FILE__,__LINE__,"unsupported camera class");
    swap16(&c->common.flags); swap16(&c->common.projection_type);
    for (unsigned i=8;i<24;i+=2) swap16((uint8_t*)c+i);
    swap32(&c->common.roll); swap32(&c->common.nnear); swap32(&c->common.ffar);
    if (c->common.up_vector) vector(a,c->common.up_vector);
    world(a,c->common.eyepos);
    if (c->common.interest != c->common.eyepos) world(a,c->common.interest);
    unsigned count = c->common.projection_type == PROJ_PERSPECTIVE ? 2 : 4;
    if (c->common.projection_type < 1 || c->common.projection_type > 3)
        HSD_Panic(__FILE__,__LINE__,"unsupported camera projection");
    require_range(a,c,0x30+4*count);
    for (unsigned i=0;i<count;i++) swap32((uint8_t*)c+0x30+4*i);
    OSReport("GS_MEMCARD_CAMERA_NATIVE_PASS projection=%u viewport=%d,%d,%d,%d\n",
        c->common.projection_type,c->common.viewport.xmin,c->common.viewport.xmax,
        c->common.viewport.ymin,c->common.viewport.ymax);
}
