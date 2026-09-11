#include "gx_capture_vita.h"

#include <dolphin/gx.h>
#include <stdint.h>
#include <string.h>

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
static GXColor tev_regs[4];
static GXColor konst_regs[4];
static GXColor chan_amb[2];
static GXColor chan_mat[2];
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
    tluts[name & 0xffu] = *(MvGXTlutObj *)obj;
}

static void bind_texture_layer(const MvGXTexObj *t, unsigned layer)
{
    MvGxMaterialState *m = mv_gx_capture_material_state();
    const MvGXTlutObj *p = &tluts[t->tlut_name & 0xffu];
    const uint8_t *palette = t->tlut_name ? (const uint8_t *)(uintptr_t)p->palette : NULL;
    if (layer == 0) {
        m->image=(const uint8_t *)(uintptr_t)t->image; m->palette=palette;
        m->width=t->width; m->height=t->height; m->format=t->format;
        m->palette_format=p->format; m->palette_entries=p->entries;
        m->wrap_s=t->wrap_s; m->wrap_t=t->wrap_t; m->mag_filter=t->mag_filter;
    } else if (layer == 1) {
        m->image1=(const uint8_t *)(uintptr_t)t->image; m->palette1=palette;
        m->width1=t->width; m->height1=t->height; m->format1=t->format;
        m->palette_format1=p->format; m->palette_entries1=p->entries;
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
void GXSetNumTevStages(u8 n) { num_tev_stages=n; }
void GXSetNumChans(u8 n) { num_channels=n; }
void GXSetNumIndStages(u8 n) { num_ind_stages=n; if(n) mv_gx_capture_material_state()->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_BUMP; }
void GXSetLineWidth(u8 w, GXTexOffset o) { (void)o; line_width=w; }
void GXSetPointSize(u8 w, GXTexOffset o) { (void)o; point_size=w; }
void GXEnableTexOffsets(GXTexCoordID c,u8 l,u8 p) { (void)c;(void)l;(void)p; }

void GXSetChanAmbColor(GXChannelID c, GXColor v) { chan_amb[(unsigned)c & 1u]=v; }
void GXSetChanMatColor(GXChannelID c, GXColor v) { chan_mat[(unsigned)c & 1u]=v; }
void GXSetChanCtrl(GXChannelID c,GXBool e,GXColorSrc a,GXColorSrc m,u32 lm,GXDiffuseFn d,GXAttnFn at)
{ (void)c;(void)e;(void)a;(void)m;(void)lm;(void)d;(void)at; }

void GXSetTevColor(GXTevRegID id, GXColor c)
{ unsigned i=(unsigned)id & 3u; tev_regs[i]=c; if(id==GX_TEVREG0 || id==GX_TEVPREV) mv_gx_capture_material_state()->material_rgba=pack_rgba(c); }
void GXSetTevColorS10(GXTevRegID id, GXColorS10 c)
{ GXColor v={clamp_s10(c.r),clamp_s10(c.g),clamp_s10(c.b),clamp_s10(c.a)}; GXSetTevColor(id,v); }
void GXSetTevKColor(GXTevKColorID id, GXColor c) { konst_regs[(unsigned)id & 3u]=c; }
void GXSetTevOrder(GXTevStageID st,GXTexCoordID coord,GXTexMapID map,GXChannelID color)
{ (void)st;(void)coord;(void)color; if(map != GX_TEXMAP_NULL && mv_gx_capture_material_state()->texture_count < (u8)((unsigned)map+1u)) mv_gx_capture_material_state()->texture_count=(u8)((unsigned)map+1u); }
void GXSetTevOp(GXTevStageID id,GXTevMode mode) { (void)id;(void)mode; }
void GXSetTevColorIn(GXTevStageID s,GXTevColorArg a,GXTevColorArg b,GXTevColorArg c,GXTevColorArg d)
{
    MvGxMaterialState *m=mv_gx_capture_material_state();
    if(s==GX_TEVSTAGE0 && a==GX_CC_ZERO && b==GX_CC_ZERO && c==GX_CC_ZERO && d==GX_CC_C0)
        m->tobj_flags=(m->tobj_flags & ~(0x0fu<<16)) | (4u<<16);
}
void GXSetTevAlphaIn(GXTevStageID s,GXTevAlphaArg a,GXTevAlphaArg b,GXTevAlphaArg c,GXTevAlphaArg d)
{
    MvGxMaterialState *m=mv_gx_capture_material_state();
    if(s==GX_TEVSTAGE0 && a==GX_CA_ZERO && b==GX_CA_TEXA && c==GX_CA_A0 && d==GX_CA_ZERO)
        m->tobj_flags=(m->tobj_flags & ~(0x0fu<<20)) | (3u<<20);
}

void GXSetTevColorOp(GXTevStageID s,GXTevOp o,GXTevBias b,GXTevScale sc,GXBool cl,GXTevRegID r) { (void)s;(void)o;(void)b;(void)sc;(void)cl;(void)r; }
void GXSetTevAlphaOp(GXTevStageID s,GXTevOp o,GXTevBias b,GXTevScale sc,GXBool cl,GXTevRegID r) { (void)s;(void)o;(void)b;(void)sc;(void)cl;(void)r; }
void GXSetTevKColorSel(GXTevStageID s,GXTevKColorSel v) { (void)s;(void)v; }
void GXSetTevKAlphaSel(GXTevStageID s,GXTevKAlphaSel v) { (void)s;(void)v; }
void GXSetTevSwapMode(GXTevStageID s,GXTevSwapSel r,GXTevSwapSel t) { (void)s;(void)r;(void)t; }
void GXSetTevClampMode(int a,int b) { (void)a;(void)b; }

void GXSetTexCoordGen2(GXTexCoordID d,GXTexGenType f,GXTexGenSrc s,u32 m,GXBool n,u32 p)
{ (void)d;(void)f;(void)s;(void)p; if(n || (m!=GX_IDENTITY && m!=GX_TEXMTX0 && m!=GX_TEXMTX1 && m!=GX_TEXMTX2 && m!=GX_TEXMTX3 && m!=GX_TEXMTX4 && m!=GX_TEXMTX5 && m!=GX_TEXMTX6 && m!=GX_TEXMTX7 && m!=GX_TEXMTX8 && m!=GX_TEXMTX9)) mv_gx_capture_material_state()->unsupported |= MV_GX_MATERIAL_UNSUPPORTED_TEXCOORD; }

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
