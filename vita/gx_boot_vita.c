#include "gx_boot_vita.h"
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <sysdolphin/baselib/video.h>
#include <sysdolphin/baselib/debug.h>
#include <psp2/display.h>
#include <string.h>

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
    /* The only EFB contents this backend can currently produce are the initial
       black surface. Fail closed before a game rendering copy can be mistaken
       for a successfully implemented GX renderer. */
    if (!registered || pass != HSD_RP_SCREEN || !vi->black ||
        vi->clear_clr.r || vi->clear_clr.g || vi->clear_clr.b || vi->rmode.aa ||
        vi->rmode.fbWidth != 640 || vi->rmode.xfbHeight != 480)
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
}
u32 VIGetNextField(void) { return (retraces + 1) & 1; }
