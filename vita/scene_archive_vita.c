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
extern void mv_stage_archive_prepare_raw(void*, size_t, const char*);
extern void mv_effect_archive_prepare_raw(void*, size_t, const char*);
extern void mv_fighter_archive_prepare_raw(void*, size_t, const char*);
extern void mv_gameplay_archive_prepare_raw(void*, size_t, const char*);
extern void mv_pause_scene_archive_prepare_raw(void*, size_t, const char*);
extern void mv_ifall_archive_prepare_raw(void*, size_t, const char*);
extern void mv_scene_sidecar_archive_prepare_raw(void*, size_t, const char*);

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

static void classic_easy_swap_float_records(uint8_t *data, uint32_t offset,
                                            unsigned count, unsigned stride,
                                            unsigned words)
{
    for (unsigned i = 0; i < count; ++i) {
        for (unsigned j = 0; j < words; ++j)
            swap32(data + offset + i * stride + j * sizeof(uint32_t));
    }
}

static void classic_easy_prepare_raw(void *bytes, size_t size,
                                     const char *filename)
{
    if (!filename || (strcmp(filename, "GmIntEz.dat") != 0 &&
                      strcmp(filename, "GmIntEz.usd") != 0))
        return;

    MvDat dat;
    if (mv_dat_open(&dat, bytes, size))
        HSD_Panic(__FILE__, __LINE__, "Classic Easy table DAT parse failed");

    uint32_t root = 0;
    if (dat_public_offset(&dat, "gmIntroEasyTable", &root) || root != 0 ||
        dat.data_size != 0x9B8 || dat.relocation_count != 0) {
        mv_dat_close(&dat);
        HSD_Panic(__FILE__, __LINE__, "Classic Easy table layout mismatch");
    }

    uint8_t *data = (uint8_t *) (uintptr_t) dat.data;
    /* gmIntroEasyTable is a scalar-only 0x9B8-byte root.  Convert only the
     * typed float fields from gm_1832.c; the non-zero padding/byte regions are
     * intentionally preserved byte-for-byte. */
    classic_easy_swap_float_records(data, 0x000, 1, 0x06C, 27);
    classic_easy_swap_float_records(data, 0x06C, 28, 0x01C, 5);
    classic_easy_swap_float_records(data, 0x37C, 25, 0x014, 3);
    classic_easy_swap_float_records(data, 0x57C, 3, 0x030, 12);
    classic_easy_swap_float_records(data, 0x630, 1, 0x078, 30);
    classic_easy_swap_float_records(data, 0x6A8, 28, 0x01C, 5);

    OSReport("VITA_CLASSIC_EASY_TABLE_NATIVE_PASS file=%s floats=448 bytes=%u\n",
             filename, dat.data_size);
    mv_dat_close(&dat);
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
    mv_gameplay_archive_prepare_raw(bytes, size, filename);
    /* Stage archives need their nested HSD descriptor scalars converted while
     * relocation fields are still raw GameCube offsets. Pointer relocation is
     * then left to HSD_ArchiveParse exactly as upstream expects. */
    mv_stage_archive_prepare_raw(bytes, size, filename);
    /* Fighter/effect DATs use the same mixed-endian HSD descriptors but expose
     * them through eff*DataTable rather than map_head. Nativeize those graphs
     * at the same pre-relocation boundary. */
    mv_effect_archive_prepare_raw(bytes, size, filename);
    /* Costume/model DATs expose Ply*5K_Share_joint and optional MatAnim roots.
     * They are heavily preloaded through lbdvd type 2, so they need the same
     * scalar conversion before pointer relocation. */
    mv_fighter_archive_prepare_raw(bytes, size, filename);
    /* Ordinary SceneDesc archives can contain HSD graphs that are not exposed
     * through stage/fighter/effect public roots.  GmPause is the first retail
     * gameplay archive to hit this path on hardware. */
    mv_pause_scene_archive_prepare_raw(bytes, size, filename);
    /* IfAll is commonly loaded through preload type 2, where filename is
     * intentionally unavailable. Detect and nativeize its complete HUD model
     * schema from public roots before relocation. */
    mv_ifall_archive_prepare_raw(bytes, size, filename);
    /* SceneDesc sidecars such as IfCoGet/IfPrize are independent archives
       loaded from gameplay/UI callbacks, not children of IfAll. Nativeize the
       whole source-derived family before HSD relocation as well. */
    mv_scene_sidecar_archive_prepare_raw(bytes, size, filename);
    classic_easy_prepare_raw(bytes, size, filename);

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
    /* SceneDesc descriptor scalars are now nativeized once, pre-relocation,
       by mv_scene_sidecar_archive_prepare_raw().  The old NtMsgWin camera
       post-pass would byte-swap a correctly native descriptor a second time. */
}
