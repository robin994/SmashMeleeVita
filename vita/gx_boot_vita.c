#include "gx_boot_vita.h"
#include "gx_capture_vita.h"
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/vi.h>
#include <sysdolphin/baselib/video.h>
#include <sysdolphin/baselib/debug.h>
#include <psp2/display.h>
#include <string.h>
#include <math.h>

GXRenderModeObj GXNtsc480IntDf = { 0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
    { 6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6 },
    { 8,8,10,12,10,8,8 } };
GXRenderModeObj GXNtsc480Int = { 0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
    { 6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6 },
    { 0,0,21,22,21,0,0 } };
GXRenderModeObj GXNtsc480Prog = { 2, 640, 480, 480, 40, 0, 640, 480, 0, 0, 0,
    { 6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6 },
    { 0,0,21,22,21,0,0 } };

static VIRetraceCallback pre_cb, post_cb;
static GXDrawDoneCallback draw_done;
static GXRenderModeObj pending_mode, mode;
static void *pending_fb, *display_fb;
static int pending_black = 1, black = 1, configured;
static u32 retraces, copies, flushes, light_mask;
static u32 xf_flush_vertices, dl_save_context;
static GXFifoObj fifo;
static void *fifo_memory;
static u32 fifo_size;
typedef struct {
    u32 reserved[3], color;
    f32 a[3], k[3], position[3], direction[3];
} NativeLight;
_Static_assert(sizeof(NativeLight) == sizeof(GXLightObj), "GX light ABI");
static NativeLight lights[8];

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{ VIRetraceCallback old = pre_cb; pre_cb = cb; return old; }
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{ VIRetraceCallback old = post_cb; post_cb = cb; return old; }
void VIConfigure(GXRenderModeObj *rm)
{
    if (!rm || rm->fbWidth != 640 || rm->efbHeight != 480 || rm->xfbHeight != 480 || rm->aa)
        HSD_Panic(__FILE__, __LINE__, "unsupported VI boot mode");
    pending_mode = *rm;
    configured = 1;
}
void VISetBlack(BOOL value) { pending_black = value != 0; }
void VISetNextFrameBuffer(void *fb)
{
    int found = 0;
    for (int i = 0; i < HSD_VI_XFB_MAX; ++i)
        if (HSD_VIData.xfb[i].buffer == fb && fb) found = 1;
    if (!found) HSD_Panic(__FILE__, __LINE__, "VI framebuffer is not registered");
    pending_fb = fb;
}
void VIFlush(void)
{
    mode = pending_mode; black = pending_black; display_fb = pending_fb; ++flushes;
}
u32 VIGetTvFormat(void) { return VI_NTSC; }
u32 VIGetRetraceCount(void) { return retraces; }
void mv_vi_tick(void)
{
    ++retraces;
    if (pre_cb) pre_cb(retraces);
    if (post_cb) post_cb(retraces);
}
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb)
{ GXDrawDoneCallback old = draw_done; draw_done = cb; return old; }
/* All operations supported here are synchronous CPU boot operations. These
 * callbacks must acquire a real GPU fence when GX primitive submission is added. */
void GXSetDrawDone(void) { if (draw_done) draw_done(); }
void GXWaitDrawDone(void) { /* No outstanding asynchronous boot operations. */ }

GXFifoObj *GXInit(void *memory, u32 size)
{
    if (!memory || ((uintptr_t)memory & 31) || size < 0x10000 || (size & 31))
        HSD_Panic(__FILE__, __LINE__, "invalid boot FIFO allocation");
    fifo_memory = memory; fifo_size = size;
    memset(memory, 0, size); memset(&fifo, 0, sizeof(fifo));
    /* CPU storage is reserved; this is not a GameCube hardware FIFO mapping. */
    return &fifo;
}
void GXSetMisc(GXMiscToken token, u32 value)
{
    switch (token) {
    case GX_MT_XF_FLUSH:
        xf_flush_vertices = value;
        break;
    case GX_MT_DL_SAVE_CONTEXT:
        dl_save_context = value != 0;
        break;
    case GX_MT_NULL:
        break;
    default:
        HSD_Panic(__FILE__, __LINE__, "unsupported GXSetMisc token");
    }
}
void GXInitLightPos(GXLightObj *obj, f32 x, f32 y, f32 z)
{
    NativeLight *light = (NativeLight *)obj;
    memset(light->reserved, 0, sizeof(light->reserved));
    light->position[0] = x; light->position[1] = y; light->position[2] = z;
}
void GXInitLightDir(GXLightObj *obj, f32 x, f32 y, f32 z)
{
    NativeLight *light = (NativeLight *)obj;
    light->direction[0] = -x; light->direction[1] = -y; light->direction[2] = -z;
}
void GXInitLightAttn(GXLightObj *obj, f32 a, f32 b, f32 c, f32 d, f32 e, f32 f)
{
    NativeLight *light = (NativeLight *)obj;
    light->a[0] = a; light->a[1] = b; light->a[2] = c;
    light->k[0] = d; light->k[1] = e; light->k[2] = f;
}
void GXInitLightColor(GXLightObj *obj, GXColor color)
{
    ((NativeLight *)obj)->color = (u32)color.r << 24 | (u32)color.g << 16 |
                                (u32)color.b << 8 | color.a;
}
void GXLoadLightObjImm(GXLightObj *obj, GXLightID id)
{
    unsigned mask = (unsigned)id;
    if (!obj || !mask || mask > 128 || (mask & (mask - 1)))
        HSD_Panic(__FILE__, __LINE__, "invalid GX boot light");
    unsigned index = 0;
    while ((1u << index) != mask) ++index;
    memcpy(&lights[index], obj, sizeof(*obj)); light_mask |= mask;
}
void mv_vi_boot_copy(const void *status, void *buffer, int pass)
{
    const HSD_VIStatus *vi = status;
    int registered = 0;
    for (int i = 0; i < HSD_VI_XFB_MAX; ++i)
        if (HSD_VIData.xfb[i].buffer == buffer && buffer) registered = 1;
    if (!registered || pass != HSD_RP_SCREEN)
        HSD_Panic(__FILE__, __LINE__, "invalid Vita XFB bookkeeping copy");
#ifdef MELEE_VITA_FULL_GAMEPLAY_SCENE
    /* Retail gameplay is presented by the captured vitaGL renderer.  Keep the
     * HSD VI/XFB state machine moving, but do not overwrite the visible Vita
     * framebuffer with a fake GameCube XFB once VI black has been cleared. */
    if (!vi->black) { ++copies; return; }
#endif
    /* During boot, preserve the deterministic black YUV copy used by the
     * existing VI validation path. */
    if (!vi->black || vi->clear_clr.r || vi->clear_clr.g || vi->clear_clr.b ||
        vi->rmode.aa || vi->rmode.fbWidth != 640 || vi->rmode.xfbHeight != 480)
        HSD_Panic(__FILE__, __LINE__, "non-boot EFB copy requires GX renderer");
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < 640u * 480u * 2u; i += 4) {
        bytes[i] = bytes[i + 2] = 16;
        bytes[i + 1] = bytes[i + 3] = 128;
    }
    ++copies;
}
int mv_vi_boot_validate(uint32_t out[6])
{
    if (!configured || !black || HSD_VIGetNbXFB() != 2 || copies != 1 ||
        !fifo_memory || fifo_size != 0x40000 || light_mask != 255) return -1;
    for (int i = 0; i < 8; ++i)
        if (lights[i].position[0] != 1 || lights[i].direction[0] != -1 ||
            lights[i].a[0] != 1 || lights[i].k[0] != 1 || lights[i].color != 0) return -2;
    HSD_VIGXDrawDoneCallback previous = HSD_VISetUserGXDrawDoneCallback(HSD_VIDrawDoneXFB);
    int index = HSD_VIGetXFBDrawEnable();
    if (index < 0) return -3;
    HSD_VISetXFBWaitDone(index);
    HSD_VIGXSetDrawDone(index);
    if (HSD_VIData.xfb[index].status != HSD_VI_XFB_NEXT) return -4;
    VIWaitForRetrace();
    HSD_VISetUserGXDrawDoneCallback(previous);
    if (HSD_VIData.xfb[index].status != HSD_VI_XFB_DISPLAY ||
        display_fb != HSD_VIData.xfb[index].buffer) return -5;
    const uint8_t *pixels = HSD_VIData.xfb[0].buffer;
    for (size_t i = 0; i < 640u * 480u * 2u; i += 2)
        if (pixels[i] != 16 || pixels[i+1] != 128) return -6;
    out[0] = mode.fbWidth; out[1] = mode.xfbHeight; out[2] = retraces;
    out[3] = copies; out[4] = flushes; out[5] = light_mask;
    return 0;
}
int mv_gx_misc_validate(uint32_t out[2])
{
    if (!out || xf_flush_vertices != 8 || dl_save_context != 0)
        return -1;
    out[0] = xf_flush_vertices;
    out[1] = dl_save_context;
    return 0;
}

/* Camera state is retained for scene initialization. The diagnostic replay
 * still uses its own camera; this is not a general scene-rendering backend. */
static struct {
    float viewport[6], projection[4][4];
    u32 scissor[4], field;
    GXProjectionType projection_type;
} camera_state;
void GXSetViewport(f32 x, f32 y, f32 w, f32 h, f32 n, f32 f)
{
    camera_state.viewport[0]=x; camera_state.viewport[1]=y;
    camera_state.viewport[2]=w; camera_state.viewport[3]=h;
    camera_state.viewport[4]=n; camera_state.viewport[5]=f;
    mv_gx_capture_set_viewport(x, y, w, h, n, f);
}
void GXSetViewportJitter(f32 x, f32 y, f32 w, f32 h, f32 n, f32 f, u32 field)
{
    camera_state.field=field;
    GXSetViewport(x, y - (field == 0 ? 0.5f : 0.0f), w, h, n, f);
}
void GXSetScissor(u32 x, u32 y, u32 w, u32 h)
{
    camera_state.scissor[0]=x; camera_state.scissor[1]=y;
    camera_state.scissor[2]=w; camera_state.scissor[3]=h;
}
void GXSetProjection(f32 matrix[4][4], GXProjectionType type)
{
    memcpy(camera_state.projection, matrix, sizeof(camera_state.projection));
    camera_state.projection_type=type;
    mv_gx_capture_set_projection(matrix, (uint32_t)type);
}
void GXGetViewportv(f32 *vp)
{
    if (!vp) HSD_Panic(__FILE__, __LINE__, "GXGetViewportv null");
    memcpy(vp, camera_state.viewport, sizeof(camera_state.viewport));
}

void GXGetProjectionv(f32 *pm)
{
    if (!pm) HSD_Panic(__FILE__, __LINE__, "GXGetProjectionv null");
    pm[0] = (f32)camera_state.projection_type;
    pm[1] = camera_state.projection[0][0];
    pm[3] = camera_state.projection[1][1];
    pm[5] = camera_state.projection[2][2];
    pm[6] = camera_state.projection[2][3];
    if (camera_state.projection_type == GX_ORTHOGRAPHIC) {
        pm[2] = camera_state.projection[0][3];
        pm[4] = camera_state.projection[1][3];
    } else {
        pm[2] = camera_state.projection[0][2];
        pm[4] = camera_state.projection[1][2];
    }
}

void GXProject(f32 x, f32 y, f32 z, f32 mtx[3][4], f32 *pm, f32 *vp,
               f32 *sx, f32 *sy, f32 *sz)
{
    Vec p;
    p.x = mtx[0][3] + mtx[0][2]*z + mtx[0][0]*x + mtx[0][1]*y;
    p.y = mtx[1][3] + mtx[1][2]*z + mtx[1][0]*x + mtx[1][1]*y;
    p.z = mtx[2][3] + mtx[2][2]*z + mtx[2][0]*x + mtx[2][1]*y;
    f32 xc, yc, zc, wc;
    if (pm[0] == 0.0f) {
        xc = p.x*pm[1] + p.z*pm[2]; yc = p.y*pm[3] + p.z*pm[4];
        zc = pm[6] + p.z*pm[5]; wc = 1.0f / -p.z;
    } else {
        xc = pm[2] + p.x*pm[1]; yc = pm[4] + p.y*pm[3];
        zc = pm[6] + p.z*pm[5]; wc = 1.0f;
    }
    *sx = vp[2]*0.5f + vp[0] + wc*xc*vp[2]*0.5f;
    *sy = vp[3]*0.5f + vp[1] + wc*(-yc)*vp[3]*0.5f;
    *sz = vp[5] + wc*zc*(vp[5]-vp[4]);
}

void GXInitLightSpot(GXLightObj *obj, f32 cutoff, GXSpotFn fn)
{
    NativeLight *light = (NativeLight *)obj;
    f32 a0, a1, a2, d, cr;
    if (!obj) HSD_Panic(__FILE__, __LINE__, "GXInitLightSpot null");
    if (cutoff <= 0.0f || cutoff > 90.0f) fn = GX_SP_OFF;
    cr = cosf(3.1415927f * cutoff / 180.0f);
    switch (fn) {
    case GX_SP_FLAT: a0=-1000.0f*cr; a1=1000.0f; a2=0.0f; break;
    case GX_SP_COS: a0=-cr/(1.0f-cr); a1=1.0f/(1.0f-cr); a2=0.0f; break;
    case GX_SP_COS2: a0=0.0f; a1=-cr/(1.0f-cr); a2=1.0f/(1.0f-cr); break;
    case GX_SP_SHARP: d=(1.0f-cr)*(1.0f-cr); a0=cr*(cr-2.0f)/d; a1=2.0f/d; a2=-1.0f/d; break;
    case GX_SP_RING1: d=(1.0f-cr)*(1.0f-cr); a0=-4.0f*cr/d; a1=4.0f*(1.0f+cr)/d; a2=-4.0f/d; break;
    case GX_SP_RING2: d=(1.0f-cr)*(1.0f-cr); a0=1.0f-2.0f*cr*cr/d; a1=4.0f*cr/d; a2=-2.0f/d; break;
    default: a0=1.0f; a1=0.0f; a2=0.0f; break;
    }
    light->a[0]=a0; light->a[1]=a1; light->a[2]=a2;
}

void GXInitLightDistAttn(GXLightObj *obj, f32 ref_dist, f32 ref_br, GXDistAttnFn fn)
{
    NativeLight *light = (NativeLight *)obj;
    f32 k0=1.0f, k1=0.0f, k2=0.0f;
    if (!obj) HSD_Panic(__FILE__, __LINE__, "GXInitLightDistAttn null");
    if (ref_dist < 0.0f || ref_br <= 0.0f || ref_br >= 1.0f) fn = GX_DA_OFF;
    switch (fn) {
    case GX_DA_GENTLE: k1=(1.0f-ref_br)/(ref_br*ref_dist); break;
    case GX_DA_MEDIUM: k1=0.5f*(1.0f-ref_br)/(ref_br*ref_dist); k2=0.5f*(1.0f-ref_br)/(ref_br*ref_dist*ref_dist); break;
    case GX_DA_STEEP: k2=(1.0f-ref_br)/(ref_br*ref_dist*ref_dist); break;
    default: break;
    }
    light->k[0]=k0; light->k[1]=k1; light->k[2]=k2;
}

u32 VIGetNextField(void) { return (retraces + 1) & 1; }
