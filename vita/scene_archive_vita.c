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
#include <stdlib.h>
#include "hsd_data.h"
#include "hsd_native.h"
#include "hsd_anim_native.h"
#include "hsd_matanim_native.h"

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
extern void mv_event_menu_archive_prepare(HSD_Archive*);

typedef struct {
    int ready;
    void *source;
    size_t source_size;
    MvNativeHsd hsd;
    MvNativeAnim anim;
    MvNativeMatAnim matanim;
    HSD_AnimJoint *anim_table[2];
    HSD_MatAnimJoint *matanim_table[2];
    DynamicModelDesc model;
    DynamicModelDesc *models[2];
} MvIntroNativeScene;

static MvIntroNativeScene intro_native[5];
static const char *const intro_names[5] = {
    "IrAls", "IrEzTarg", "IrEzTuki", "IrEzFigG", "IrRdMap"
};

static int intro_name_index(const char *filename)
{
    if (!filename) return -1;
    for (unsigned i = 0; i < 5; ++i) {
        size_t n = strlen(intro_names[i]);
        if (!strncmp(filename, intro_names[i], n) &&
            (filename[n] == 0 || !strcmp(filename + n, ".dat") ||
             !strcmp(filename + n, ".usd")))
            return (int)i;
    }
    return -1;
}

static int dat_public_offset(const MvDat *dat, const char *wanted, uint32_t *out)
{
    for (uint32_t i = 0; i < dat->public_count; ++i) {
        const char *name;
        uint32_t offset;
        if (mv_dat_public(dat, i, &name, &offset)) return -1;
        if (!strcmp(name, wanted)) { *out = offset; return 0; }
    }
    return -1;
}

static void intro_native_release(MvIntroNativeScene *n)
{
    if (!n) return;
    mv_native_matanim_free(&n->matanim);
    mv_native_anim_free(&n->anim);
    mv_hsd_native_free(&n->hsd);
    memset(n, 0, sizeof(*n));
}

void mv_boot_archive_prepare_raw(void *bytes, size_t size, const char *filename)
{
    int index = intro_name_index(filename);
    if (index < 0) return;
    MvIntroNativeScene *n = &intro_native[index];
    intro_native_release(n);

    MvDat dat;
    if (mv_dat_open(&dat, bytes, size))
        HSD_Panic(__FILE__, __LINE__, "Classic Intro DAT parse failed");

    uint32_t scene = 0, models = 0, model = 0, joint = 0;
    uint32_t table = 0, target = 0;
    if (dat_public_offset(&dat, "ScItrAllstar_scene_data", &scene) ||
        mv_dat_pointer(&dat, scene, &models) != 1 ||
        mv_dat_pointer(&dat, models, &model) != 1 ||
        mv_dat_pointer(&dat, model, &joint) != 1) {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "Classic Intro model descriptor missing");
    }

    int r = mv_hsd_native_build_at(&dat, joint, &n->hsd);
    if (r) {
        OSReport("VITA_INTRO_NATIVE_MODEL_FAIL file=%s code=%d unsupported=%s off=%08x val=%08x\n",
                 filename, r, mv_hsd_native_unsupported_name(n->hsd.unsupported_kind),
                 n->hsd.unsupported_offset, n->hsd.unsupported_value);
        mv_dat_close(&dat);
        intro_native_release(n);
        HSD_Panic(__FILE__, __LINE__, "Classic Intro native JObj conversion failed");
    }

    r = mv_dat_pointer(&dat, model + 4, &table);
    if (r < 0) { mv_dat_close(&dat); intro_native_release(n); HSD_Panic(__FILE__, __LINE__, "Classic Intro AnimJoint table invalid"); }
    if (r == 1) {
        if (mv_dat_pointer(&dat, table, &target) != 1 ||
            mv_native_anim_build_at(&dat, target, &n->anim)) {
            mv_dat_close(&dat); intro_native_release(n); HSD_Panic(__FILE__, __LINE__, "Classic Intro AnimJoint conversion failed");
        }
        n->anim_table[0] = n->anim.root;
    }

    r = mv_dat_pointer(&dat, model + 8, &table);
    if (r < 0) { mv_dat_close(&dat); intro_native_release(n); HSD_Panic(__FILE__, __LINE__, "Classic Intro MatAnim table invalid"); }
    if (r == 1) {
        if (mv_dat_pointer(&dat, table, &target) != 1 ||
            mv_native_matanim_build_at(&dat, target, &n->matanim)) {
            mv_dat_close(&dat); intro_native_release(n); HSD_Panic(__FILE__, __LINE__, "Classic Intro MatAnim conversion failed");
        }
        n->matanim_table[0] = n->matanim.root;
    }

    r = mv_dat_pointer(&dat, model + 12, &target);
    if (r != 0) {
        mv_dat_close(&dat); intro_native_release(n);
        HSD_Panic(__FILE__, __LINE__, "Classic Intro ShapeAnim requires adapter");
    }

    n->model.joint = n->hsd.root;
    n->model.anims = n->anim_table[0] ? n->anim_table : NULL;
    n->model.matanims = n->matanim_table[0] ? n->matanim_table : NULL;
    n->model.shapeanims = NULL;
    n->models[0] = &n->model;
    n->source = bytes;
    n->source_size = size;
    n->ready = 1;
    OSReport("VITA_INTRO_NATIVE_RAW_PASS file=%s joints=%u dobjs=%u pobjs=%u anim=%u matanim=%u\n",
             filename, n->hsd.joint_count, n->hsd.dobj_count, n->hsd.pobj_count,
             n->anim_table[0] != NULL, n->matanim_table[0] != NULL);
    mv_dat_close(&dat);
}

void mv_boot_archive_prepare(HSD_Archive* a, const char* filename)
{
    int intro_index = intro_name_index(filename);
    if (intro_index >= 0) {
        MvIntroNativeScene *n = &intro_native[intro_index];
        if (!n->ready || n->source != a->top_ptr || n->source_size != a->header.file_size)
            HSD_Panic(__FILE__, __LINE__, "Classic Intro native proxy lifetime mismatch");
        SceneDesc *scene = HSD_ArchiveGetPublicAddress(a, "ScItrAllstar_scene_data");
        require_range(a, scene, sizeof(*scene));
        scene->models = n->models;
        OSReport("VITA_INTRO_NATIVE_SCENE_PASS file=%s models=1 source=typed_proxy\n", filename);
        return;
    }
    /* Only this scene root is consumed by the initial OnEnter callback today.
     * Other roots retain their original scalar encoding until adapted. */
    if (strcmp(filename, "GmEvent.dat") == 0) {
        mv_event_menu_archive_prepare(a);
        return;
    }
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
