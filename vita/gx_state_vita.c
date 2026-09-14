#include "gx_capture_vita.h"

#include <dolphin/gx.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "gx_boot_vita.h"
#include "gx_state_vita.h"

/* Software GX state for Vita. These entry points replace the GameCube BP/XF
 * register shadow: state that affects the current material is copied into the
 * capture snapshot, while hardware-only cache/sync operations are synchronous. */

typedef struct {
    u32 image;
    u16 width;
    u16 height;
    u8 format;
    u8 wrap_s;
    u8 wrap_t;
    u8 mipmap;
    u32 tlut_name;
    u8 min_filter;
    u8 mag_filter;
    u8 bias_clamp;
    u8 edge_lod;
    f32 min_lod;
    f32 max_lod;
    f32 lod_bias;
} MvGXTexObj;

typedef struct {
    u32 palette;
    u16 entries;
    u8 format;
    u8 reserved;
    u32 pad;
} MvGXTlutObj;

_Static_assert(sizeof(MvGXTexObj) <= sizeof(GXTexObj), "GXTexObj software ABI");
_Static_assert(sizeof(MvGXTlutObj) <= sizeof(GXTlutObj), "GXTlutObj software ABI");

static MvGXTlutObj tluts[256];
static u8 tlut_valid[256];
static GXColor tev_regs[4];
static GXColor konst_regs[4];
static GXColor chan_amb[2] = {{0,0,0,0},{0,0,0,0}};
static GXColor chan_mat[2] = {{255,255,255,255},{255,255,255,255}};
typedef struct {
    uint8_t enable;
    uint8_t amb_src;
    uint8_t mat_src;
    uint8_t diff_fn;
    uint8_t attn_fn;
    uint8_t reserved[3];
    uint32_t light_mask;
} MvGXChannelCtrl;
static MvGXChannelCtrl chan_color[2] = {
    {0, GX_SRC_REG, GX_SRC_VTX, GX_DF_NONE, GX_AF_NONE, {0}, GX_LIGHT_NULL},
    {0, GX_SRC_REG, GX_SRC_VTX, GX_DF_NONE, GX_AF_NONE, {0}, GX_LIGHT_NULL},
};
static MvGXChannelCtrl chan_alpha[2] = {
    {0, GX_SRC_REG, GX_SRC_VTX, GX_DF_NONE, GX_AF_NONE, {0}, GX_LIGHT_NULL},
    {0, GX_SRC_REG, GX_SRC_VTX, GX_DF_NONE, GX_AF_NONE, {0}, GX_LIGHT_NULL},
};
static u8 num_tex_gens, num_tev_stages, num_channels, num_ind_stages;
static u8 line_width = 6, point_size = 6;
static struct {
    u16 left, top, width, height;
    GXTexFmt format;
    GXBool mipmap;
} tex_copy;

static uint32_t pack_rgba(GXColor c)
{
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8) | c.a;
}

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static float dot3(const float a[3], const float b[3])
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static int normalized3(const float in[3], float out[3])
{
    if (!in || !isfinite(in[0]) || !isfinite(in[1]) || !isfinite(in[2])) return 0;
    float len2 = dot3(in, in);
    if (!isfinite(len2) || len2 <= 1.0e-20f) return 0;
    float inv = 1.0f / sqrtf(len2);
    out[0] = in[0] * inv; out[1] = in[1] * inv; out[2] = in[2] * inv;
    return 1;
}

static float light_attenuation(const MvGxLoadedLight *light,
                               const float light_dir[3], float distance,
                               GXAttnFn fn)
{
    if (fn == GX_AF_NONE) return 1.0f;
    if (fn == GX_AF_SPEC) {
        /* Specular is normally routed through COLOR1 by HSD.  Keep the
         * COLOR0 fallback finite and bounded rather than inventing a second
         * half-vector pipeline here. */
        return 1.0f;
    }

    float spot = dot3(light->direction, light_dir);
    float num = light->a[0] + light->a[1] * spot + light->a[2] * spot * spot;
    float den = light->k[0] + light->k[1] * distance +
                light->k[2] * distance * distance;
    if (!isfinite(num) || !isfinite(den) || den <= 1.0e-20f) return 0.0f;
    return clamp01(num / den);
}

static void eval_channel_rgb(const MvGXChannelCtrl *ctrl,
                             const GXColor *ambient_reg,
                             const GXColor *material_reg,
                             const uint8_t vertex[4],
                             const float position[3], const float normal[3],
                             int normal_valid, uint8_t out[3])
{
    const uint8_t *mat = ctrl->mat_src == GX_SRC_VTX ? vertex : &material_reg->r;
    const uint8_t *amb = ctrl->amb_src == GX_SRC_VTX ? vertex : &ambient_reg->r;
    if (!ctrl->enable) {
        out[0] = mat[0]; out[1] = mat[1]; out[2] = mat[2];
        return;
    }

    float accum[3] = {amb[0] / 255.0f, amb[1] / 255.0f, amb[2] / 255.0f};
    for (unsigned i = 0; i < 8u; ++i) {
        if (!(ctrl->light_mask & (1u << i))) continue;
        MvGxLoadedLight light;
        if (mv_gx_loaded_light(i, &light) != 0 || !light.valid) continue;
        float delta[3] = {light.position[0] - position[0],
                          light.position[1] - position[1],
                          light.position[2] - position[2]};
        float len2 = dot3(delta, delta);
        if (!isfinite(len2) || len2 <= 1.0e-20f) continue;
        float distance = sqrtf(len2);
        float ldir[3] = {delta[0] / distance, delta[1] / distance, delta[2] / distance};
        float diffuse = 1.0f;
        if (ctrl->diff_fn != GX_DF_NONE) {
            if (!normal_valid) diffuse = 0.0f;
            else {
                diffuse = dot3(normal, ldir);
                if (ctrl->diff_fn == GX_DF_CLAMP && diffuse < 0.0f) diffuse = 0.0f;
            }
        }
        float attn = light_attenuation(&light, ldir, distance, (GXAttnFn)ctrl->attn_fn);
        float scale = diffuse * attn;
        accum[0] += ((light.color >> 24) & 0xffu) / 255.0f * scale;
        accum[1] += ((light.color >> 16) & 0xffu) / 255.0f * scale;
        accum[2] += ((light.color >> 8) & 0xffu) / 255.0f * scale;
    }
    out[0] = (uint8_t)(clamp01(accum[0]) * mat[0] + 0.5f);
    out[1] = (uint8_t)(clamp01(accum[1]) * mat[1] + 0.5f);
    out[2] = (uint8_t)(clamp01(accum[2]) * mat[2] + 0.5f);
}

static uint8_t eval_channel_alpha(const MvGXChannelCtrl *ctrl,
                                  const GXColor *ambient_reg,
                                  const GXColor *material_reg,
                                  const uint8_t vertex[4],
                                  const float position[3], const float normal[3],
                                  int normal_valid)
{
    uint8_t mat = ctrl->mat_src == GX_SRC_VTX ? vertex[3] : material_reg->a;
    uint8_t amb = ctrl->amb_src == GX_SRC_VTX ? vertex[3] : ambient_reg->a;
    if (!ctrl->enable) return mat;
    float accum = amb / 255.0f;
    for (unsigned i = 0; i < 8u; ++i) {
        if (!(ctrl->light_mask & (1u << i))) continue;
        MvGxLoadedLight light;
        if (mv_gx_loaded_light(i, &light) != 0 || !light.valid) continue;
        float delta[3] = {light.position[0] - position[0],
                          light.position[1] - position[1],
                          light.position[2] - position[2]};
        float len2 = dot3(delta, delta);
        if (!isfinite(len2) || len2 <= 1.0e-20f) continue;
        float distance = sqrtf(len2);
        float ldir[3] = {delta[0] / distance, delta[1] / distance, delta[2] / distance};
        float diffuse = 1.0f;
        if (ctrl->diff_fn != GX_DF_NONE) {
            diffuse = normal_valid ? dot3(normal, ldir) : 0.0f;
            if (ctrl->diff_fn == GX_DF_CLAMP && diffuse < 0.0f) diffuse = 0.0f;
        }
        float attn = light_attenuation(&light, ldir, distance, (GXAttnFn)ctrl->attn_fn);
        accum += (light.color & 0xffu) / 255.0f * diffuse * attn;
    }
    return (uint8_t)(clamp01(accum) * mat + 0.5f);
}

uint32_t mv_gx_channel0_eval(uint32_t vertex_rgba,
                             const float position[3],
                             const float normal_in[3],
                             uint32_t *flags)
{
    uint32_t f = 0;
    if (!num_channels) {
        if (flags) *flags = 0;
        return vertex_rgba;
    }
    f |= MV_GX_CHANNEL_EVAL_ACTIVE;
    uint8_t vertex[4] = {(uint8_t)(vertex_rgba >> 24),
                         (uint8_t)(vertex_rgba >> 16),
                         (uint8_t)(vertex_rgba >> 8),
                         (uint8_t)vertex_rgba};
    float normal[3];
    int normal_valid = normalized3(normal_in, normal);
    if (normal_valid) f |= MV_GX_CHANNEL_EVAL_NORMAL;
    if (chan_color[0].enable || chan_alpha[0].enable) f |= MV_GX_CHANNEL_EVAL_LIT;
    uint8_t rgb[3];
    eval_channel_rgb(&chan_color[0], &chan_amb[0], &chan_mat[0], vertex,
                     position, normal, normal_valid, rgb);
    uint8_t alpha = eval_channel_alpha(&chan_alpha[0], &chan_amb[0], &chan_mat[0],
                                       vertex, position, normal, normal_valid);
    if (flags) *flags = f;
    return (uint32_t)rgb[0] << 24 | (uint32_t)rgb[1] << 16 |
           (uint32_t)rgb[2] << 8 | alpha;
}

static u8 clamp_s10(s16 v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u8)v;
}

static void tex_tile_shift(u32 fmt, u32 *sx, u32 *sy)
{
    switch (fmt) {
    case GX_TF_I4: case 0x8: case GX_TF_CMPR: case GX_CTF_R4: case GX_CTF_Z4:
        *sx = 3; *sy = 3; break;
    case GX_TF_I8: case GX_TF_IA4: case 0x9: case GX_TF_Z8:
    case GX_CTF_RA4: case GX_TF_A8: case GX_CTF_R8: case GX_CTF_G8:
    case GX_CTF_B8: case GX_CTF_Z8M: case GX_CTF_Z8L:
        *sx = 3; *sy = 2; break;
    default:
        *sx = 2; *sy = 2; break;
    }
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod)
{
    u32 sx, sy, bytes = (format == GX_TF_RGBA8 || format == GX_TF_Z24X8) ? 64u : 32u;
    u32 total = 0;
    tex_tile_shift(format, &sx, &sy);
    if (!mipmap) {
        u32 nx = (width + (1u << sx) - 1u) >> sx;
        u32 ny = (height + (1u << sy) - 1u) >> sy;
        return nx * ny * bytes;
    }
    if (!max_lod) max_lod = 1;
    for (u32 level = 0; level < max_lod; ++level) {
        u32 nx = (width + (1u << sx) - 1u) >> sx;
        u32 ny = (height + (1u << sy) - 1u) >> sy;
        total += nx * ny * bytes;
        if (width == 1 && height == 1) break;
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
    }
    return total;
}

void GXInitTexObj(GXTexObj *obj, void *image, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                  u8 mipmap)
{
    MvGXTexObj *t = (MvGXTexObj *)obj;
    memset(obj, 0, sizeof(*obj));
    t->image = (u32)(uintptr_t)image;
    t->width = width; t->height = height; t->format = (u8)format;
    t->wrap_s = (u8)wrap_s; t->wrap_t = (u8)wrap_t; t->mipmap = mipmap != 0;
    t->min_filter = mipmap ? GX_LIN_MIP_LIN : GX_LINEAR;
    t->mag_filter = GX_LINEAR;
}

void GXInitTexObjCI(GXTexObj *obj, void *image, u16 width, u16 height,
                    GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                    u8 mipmap, u32 tlut_name)
{
    GXInitTexObj(obj, image, width, height, format, wrap_s, wrap_t, mipmap);
    ((MvGXTexObj *)obj)->tlut_name = tlut_name;
}

void GXInitTexObjLOD(GXTexObj *obj, GXTexFilter min_filt, GXTexFilter mag_filt,
                     f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp,
                     GXBool do_edge_lod, GXAnisotropy max_aniso)
{
    (void)max_aniso;
    MvGXTexObj *t = (MvGXTexObj *)obj;
    t->min_filter=(u8)min_filt; t->mag_filter=(u8)mag_filt;
    t->min_lod=min_lod; t->max_lod=max_lod; t->lod_bias=lod_bias;
    t->bias_clamp=bias_clamp != 0; t->edge_lod=do_edge_lod != 0;
}

void GXInitTlutObj(GXTlutObj *obj, void *lut, GXTlutFmt fmt, u16 entries)
{
    MvGXTlutObj *t=(MvGXTlutObj *)obj;
    memset(obj,0,sizeof(*obj)); t->palette=(u32)(uintptr_t)lut;
    t->entries=entries; t->format=(u8)fmt;
}

void GXLoadTlut(GXTlutObj *obj, u32 name)
{
    const u32 slot = name & 0xffu;
    tluts[slot] = *(MvGXTlutObj *)obj;
    tlut_valid[slot] = 1;
}

static int texture_uses_tlut(u8 format)
{
    return format == GX_TF_C4 || format == GX_TF_C8 || format == GX_TF_C14X2;
}

static void bind_texture_layer(const MvGXTexObj *t, unsigned layer)
{
    MvGxMaterialState *m = mv_gx_capture_material_state();
    const u32 slot = t->tlut_name & 0xffu;
    const MvGXTlutObj *p = &tluts[slot];
    /* GX_TLUT0 is numeric zero and is a fully valid TLUT name.  Do not use
     * tlut_name!=0 as a palette-presence sentinel: HSD assigns its first CI
     * texture to slot 0.  Track GXLoadTlut state independently instead. */
    const int uses_tlut = texture_uses_tlut(t->format);
    const uint8_t *palette =
        uses_tlut && tlut_valid[slot] ? (const uint8_t *)(uintptr_t)p->palette : NULL;
    const u16 palette_entries = uses_tlut && tlut_valid[slot] ? p->entries : 0;
    const u8 palette_format = uses_tlut && tlut_valid[slot] ? p->format : 0;
    if (layer == 0) {
        m->image=(const uint8_t *)(uintptr_t)t->image; m->palette=palette;
        m->width=t->width; m->height=t->height; m->format=t->format;
        m->palette_format=palette_format; m->palette_entries=palette_entries;
        m->wrap_s=t->wrap_s; m->wrap_t=t->wrap_t; m->mag_filter=t->mag_filter;
    } else if (layer == 1) {
        m->image1=(const uint8_t *)(uintptr_t)t->image; m->palette1=palette;
        m->width1=t->width; m->height1=t->height; m->format1=t->format;
        m->palette_format1=palette_format; m->palette_entries1=palette_entries;
        m->wrap_s1=t->wrap_s; m->wrap_t1=t->wrap_t; m->mag_filter1=t->mag_filter;
    } else {
        m->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_MULTITEX;
    }
    if (m->texture_count < layer + 1) m->texture_count = (u8)(layer + 1);
}

void GXLoadTexObj(GXTexObj *obj, GXTexMapID id)
{
    bind_texture_layer((const MvGXTexObj *)obj, (unsigned)id);
}

void GXInvalidateTexAll(void) {}
void GXInvalidateVtxCache(void) {}
void GXPixModeSync(void) {}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op)
{ MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_blend_type=type; m->pe_src_factor=src; m->pe_dst_factor=dst; m->pe_logic_op=op; m->pe_custom=1; }
void GXSetColorUpdate(GXBool v) { MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_color_update=v!=0; m->pe_custom=1; }
void GXSetAlphaUpdate(GXBool v) { MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_alpha_update=v!=0; m->pe_custom=1; }
void GXSetZMode(GXBool e, GXCompare f, GXBool u) { MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_z_enable=e!=0; m->pe_z_func=f; m->pe_z_update=u!=0; m->pe_custom=1; }
void GXSetZCompLoc(GXBool v) { MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_z_comp_loc=v!=0; m->pe_custom=1; }
void GXSetDither(GXBool v) { MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_dither=v!=0; m->pe_custom=1; }
void GXSetDstAlpha(GXBool e, u8 a) { MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_dst_alpha_enable=e!=0; m->pe_dst_alpha=a; m->pe_custom=1; }
void GXSetAlphaCompare(GXCompare c0,u8 r0,GXAlphaOp op,GXCompare c1,u8 r1) { MvGxMaterialState *m=mv_gx_capture_material_state(); m->pe_alpha_comp0=c0; m->pe_alpha_ref0=r0; m->pe_alpha_op=op; m->pe_alpha_comp1=c1; m->pe_alpha_ref1=r1; m->pe_custom=1; }

void GXSetNumTexGens(u8 n) { num_tex_gens=n; }
void GXSetNumTevStages(u8 n)
{
    num_tev_stages=n;
    MvGxMaterialState *m=mv_gx_capture_material_state();
    m->tev_stage_count=n;
    if (n != 1) {
        m->simple_tev_color_c0=0;
        m->simple_tev_alpha_texa_a0=0;
    }
}
void GXSetNumChans(u8 n) { num_channels=n; }
void GXSetNumIndStages(u8 n) { num_ind_stages=n; if(n) mv_gx_capture_material_state()->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_BUMP; }
void GXSetLineWidth(u8 w, GXTexOffset o) { (void)o; line_width=w; }
void GXSetPointSize(u8 w, GXTexOffset o) { (void)o; point_size=w; }
void GXEnableTexOffsets(GXTexCoordID c,u8 l,u8 p) { (void)c;(void)l;(void)p; }

static unsigned channel_index(GXChannelID c)
{
    return c == GX_COLOR1 || c == GX_ALPHA1 || c == GX_COLOR1A1 ? 1u : 0u;
}

void GXSetChanAmbColor(GXChannelID c, GXColor v)
{
    unsigned i = channel_index(c);
    if (c == GX_COLOR0 || c == GX_COLOR1) {
        chan_amb[i].r=v.r; chan_amb[i].g=v.g; chan_amb[i].b=v.b;
    } else if (c == GX_ALPHA0 || c == GX_ALPHA1) {
        chan_amb[i].a=v.a;
    } else if (c == GX_COLOR0A0 || c == GX_COLOR1A1) {
        chan_amb[i]=v;
    }
}
void GXSetChanMatColor(GXChannelID c, GXColor v)
{
    unsigned i = channel_index(c);
    if (c == GX_COLOR0 || c == GX_COLOR1) {
        chan_mat[i].r=v.r; chan_mat[i].g=v.g; chan_mat[i].b=v.b;
    } else if (c == GX_ALPHA0 || c == GX_ALPHA1) {
        chan_mat[i].a=v.a;
    } else if (c == GX_COLOR0A0 || c == GX_COLOR1A1) {
        chan_mat[i]=v;
    }
}
void GXSetChanCtrl(GXChannelID c,GXBool e,GXColorSrc a,GXColorSrc m,u32 lm,GXDiffuseFn d,GXAttnFn at)
{
    unsigned i = channel_index(c);
    MvGXChannelCtrl ctrl = {(uint8_t)(e != GX_DISABLE), (uint8_t)a, (uint8_t)m,
                            (uint8_t)d, (uint8_t)at, {0}, lm};
    if (c == GX_COLOR0 || c == GX_COLOR1) chan_color[i] = ctrl;
    else if (c == GX_ALPHA0 || c == GX_ALPHA1) chan_alpha[i] = ctrl;
    else if (c == GX_COLOR0A0 || c == GX_COLOR1A1) {
        chan_color[i] = ctrl;
        chan_alpha[i] = ctrl;
    }
}

void GXSetTevColor(GXTevRegID id, GXColor c)
{ unsigned i=(unsigned)id & 3u; tev_regs[i]=c; if(id==GX_TEVREG0 || id==GX_TEVPREV) mv_gx_capture_material_state()->material_rgba=pack_rgba(c); }
void GXSetTevColorS10(GXTevRegID id, GXColorS10 c)
{ GXColor v={clamp_s10(c.r),clamp_s10(c.g),clamp_s10(c.b),clamp_s10(c.a)}; GXSetTevColor(id,v); }
void GXSetTevKColor(GXTevKColorID id, GXColor c)
{
    unsigned i = (unsigned)id & 3u;
    konst_regs[i] = c;
    mv_gx_capture_material_state()->tev_kcolor_regs[i] = pack_rgba(c);
}
void GXSetTevOrder(GXTevStageID st,GXTexCoordID coord,GXTexMapID map,GXChannelID color)
{
    MvGxMaterialState *m=mv_gx_capture_material_state();
    unsigned stage=(unsigned)st;
    if(stage<4u) { m->tev_order_coord[stage]=(uint8_t)coord; m->tev_order_map[stage]=(uint8_t)map; m->tev_order_color[stage]=(uint8_t)color; }
    if(map != GX_TEXMAP_NULL && m->texture_count < (u8)((unsigned)map+1u)) m->texture_count=(u8)((unsigned)map+1u);
}
void GXSetTevOp(GXTevStageID id,GXTevMode mode) { (void)id;(void)mode; }
void GXSetTevColorIn(GXTevStageID s,GXTevColorArg a,GXTevColorArg b,GXTevColorArg c,GXTevColorArg d)
{
    MvGxMaterialState *m=mv_gx_capture_material_state();
    unsigned stage=(unsigned)s;
    if(stage<4u) { m->tev_color_in[stage][0]=(uint8_t)a; m->tev_color_in[stage][1]=(uint8_t)b; m->tev_color_in[stage][2]=(uint8_t)c; m->tev_color_in[stage][3]=(uint8_t)d; }
    if (s == GX_TEVSTAGE0) {
        m->simple_tev_color_c0 =
            a==GX_CC_ZERO && b==GX_CC_ZERO && c==GX_CC_ZERO && d==GX_CC_C0;
        if (m->simple_tev_color_c0)
            m->tobj_flags=(m->tobj_flags & ~(0x0fu<<16)) | (4u<<16);
    } else {
        m->simple_tev_color_c0 = 0;
    }
}
void GXSetTevAlphaIn(GXTevStageID s,GXTevAlphaArg a,GXTevAlphaArg b,GXTevAlphaArg c,GXTevAlphaArg d)
{
    MvGxMaterialState *m=mv_gx_capture_material_state();
    unsigned stage=(unsigned)s;
    if(stage<4u) { m->tev_alpha_in[stage][0]=(uint8_t)a; m->tev_alpha_in[stage][1]=(uint8_t)b; m->tev_alpha_in[stage][2]=(uint8_t)c; m->tev_alpha_in[stage][3]=(uint8_t)d; }
    if (s == GX_TEVSTAGE0) {
        m->simple_tev_alpha_texa_a0 =
            a==GX_CA_ZERO && b==GX_CA_TEXA && c==GX_CA_A0 && d==GX_CA_ZERO;
        if (m->simple_tev_alpha_texa_a0)
            m->tobj_flags=(m->tobj_flags & ~(0x0fu<<20)) | (3u<<20);
    } else {
        m->simple_tev_alpha_texa_a0 = 0;
    }
}

void GXSetTevColorOp(GXTevStageID s,GXTevOp o,GXTevBias b,GXTevScale sc,GXBool cl,GXTevRegID r) { MvGxMaterialState *m=mv_gx_capture_material_state(); unsigned st=(unsigned)s; if(st<4u){m->tev_color_op[st][0]=(uint8_t)o;m->tev_color_op[st][1]=(uint8_t)b;m->tev_color_op[st][2]=(uint8_t)sc;m->tev_color_op[st][3]=(uint8_t)cl;m->tev_color_op[st][4]=(uint8_t)r;} }
void GXSetTevAlphaOp(GXTevStageID s,GXTevOp o,GXTevBias b,GXTevScale sc,GXBool cl,GXTevRegID r) { MvGxMaterialState *m=mv_gx_capture_material_state(); unsigned st=(unsigned)s; if(st<4u){m->tev_alpha_op[st][0]=(uint8_t)o;m->tev_alpha_op[st][1]=(uint8_t)b;m->tev_alpha_op[st][2]=(uint8_t)sc;m->tev_alpha_op[st][3]=(uint8_t)cl;m->tev_alpha_op[st][4]=(uint8_t)r;} }
void GXSetTevKColorSel(GXTevStageID s,GXTevKColorSel v)
{
    unsigned stage = (unsigned)s;
    if (stage < 4u) mv_gx_capture_material_state()->tev_kcolor_sel[stage] = (uint8_t)v;
}
void GXSetTevKAlphaSel(GXTevStageID s,GXTevKAlphaSel v)
{
    unsigned stage = (unsigned)s;
    if (stage < 4u) mv_gx_capture_material_state()->tev_kalpha_sel[stage] = (uint8_t)v;
}
void GXSetTevSwapMode(GXTevStageID s,GXTevSwapSel r,GXTevSwapSel t) { (void)s;(void)r;(void)t; }
void GXSetTevClampMode(int a,int b) { (void)a;(void)b; }

void GXSetTexCoordGen2(GXTexCoordID d,GXTexGenType f,GXTexGenSrc s,u32 m,GXBool n,u32 p)
{
    (void)f; MvGxMaterialState *state=mv_gx_capture_material_state();
    if(d==GX_TEXCOORD0){
        state->texgen_src0=(uint8_t)s;state->texgen_valid_mask|=1u;state->uv_mtx_valid=0;
        if(p!=GX_PTIDENTITY && mv_gx_capture_get_tex_mtx(p,state->uv_mtx)==0) state->uv_mtx_valid=1;
    } else if(d==GX_TEXCOORD1){
        state->texgen_src1=(uint8_t)s;state->texgen_valid_mask|=2u;state->uv_mtx1_valid=0;
        if(p!=GX_PTIDENTITY && mv_gx_capture_get_tex_mtx(p,state->uv_mtx1)==0) state->uv_mtx1_valid=1;
    }
    if(n || (m!=GX_IDENTITY && m!=GX_TEXMTX0 && m!=GX_TEXMTX1 && m!=GX_TEXMTX2 && m!=GX_TEXMTX3 && m!=GX_TEXMTX4 && m!=GX_TEXMTX5 && m!=GX_TEXMTX6 && m!=GX_TEXMTX7 && m!=GX_TEXMTX8 && m!=GX_TEXMTX9)) state->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_TEXCOORD;
}

void GXSetTevDirect(GXTevStageID s) { (void)s; }
void GXSetTevIndirect(GXTevStageID a,GXIndTexStageID b,GXIndTexFormat c,GXIndTexBiasSel d,GXIndTexMtxID e,GXIndTexWrap f,GXIndTexWrap g,GXBool h,GXBool i,GXIndTexAlphaSel j)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j; mv_gx_capture_material_state()->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_BUMP; }
void GXSetIndTexMtx(GXIndTexMtxID i,f32 o[2][3],s8 e) { (void)i;(void)o;(void)e; }
void GXSetIndTexCoordScale(GXIndTexStageID i,GXIndTexScale s,GXIndTexScale t) { (void)i;(void)s;(void)t; }
void GXSetIndTexOrder(GXIndTexStageID i,GXTexCoordID c,GXTexMapID m) { (void)i;(void)c;(void)m; }

void GXSetFog(GXFogType t,f32 s,f32 e,f32 n,f32 f,GXColor c) { (void)t;(void)s;(void)e;(void)n;(void)f;(void)c; }
void GXInitFogAdjTable(GXFogAdjTable *t,u16 w,f32 p[4][4]) { (void)w;(void)p; if(t) memset(t,0,sizeof(*t)); }
void GXSetFogRangeAdj(GXBool e,u16 c,GXFogAdjTable *t) { (void)e;(void)c;(void)t; }
void GXSetPixelFmt(GXPixelFmt p,GXZFmt16 z) { (void)p;(void)z; }
void GXSetFieldMode(GXBool f,GXBool h) { (void)f;(void)h; }

void GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height)
{ tex_copy.left=left; tex_copy.top=top; tex_copy.width=width; tex_copy.height=height; }

void GXSetTexCopyDst(u16 width, u16 height, GXTexFmt format, GXBool mipmap)
{ tex_copy.width=width; tex_copy.height=height; tex_copy.format=format; tex_copy.mipmap=mipmap; }

void GXCopyTex(void *dest, GXBool clear)
{
    (void)clear;
    if (!dest || !tex_copy.width || !tex_copy.height) return;
    /* No GameCube EFB exists on Vita. Materialize a deterministic empty copy
     * until the vitaGL framebuffer-copy path is wired into these effects. */
    memset(dest, 0, GXGetTexBufferSize(tex_copy.width, tex_copy.height,
                                      tex_copy.format, tex_copy.mipmap, 1));
}

/* GX object queries use the same software descriptors populated above. */
u16 GXGetTexObjWidth(const GXTexObj *obj) { return obj ? ((const MvGXTexObj *)obj)->width : 0; }
u16 GXGetTexObjHeight(const GXTexObj *obj) { return obj ? ((const MvGXTexObj *)obj)->height : 0; }
GXTexFmt GXGetTexObjFmt(const GXTexObj *obj) { return obj ? (GXTexFmt)((const MvGXTexObj *)obj)->format : GX_TF_I4; }
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan r, GXTevColorChan g, GXTevColorChan b, GXTevColorChan a)
{ (void)table; (void)r; (void)g; (void)b; (void)a; }
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias)
{ (void)op; (void)fmt; (void)bias; }
