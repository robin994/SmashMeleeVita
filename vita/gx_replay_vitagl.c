#define MV_GX_VA_PNMTXIDX 0u
#define MV_GX_VA_POS 9u
#define MV_GX_VA_CLR0 11u
#define MV_GX_VA_TEX0 13u
#define MV_GX_QUADS 0x80u
#define MV_GX_TRIANGLES 0x90u
#define MV_GX_TRIANGLESTRIP 0x98u
#define MV_GX_TRIANGLEFAN 0xa0u
#define MV_GX_REPLAY_TARGET_WIDTH 960u
#define MV_GX_REPLAY_TARGET_HEIGHT 544u
#define MV_GX_CULL_NONE 0u
#define MV_GX_CULL_FRONT 1u
#define MV_GX_CULL_BACK 2u
#define MV_GX_CULL_ALL 3u
#include "gx_replay_vita.h"
#include "gx_texture.h"
#include <dolphin/gx/GXEnum.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
/* vitaGL backend. CPU decoding/known TEV bakes are retained; projection,
 * clipping, interpolation, depth, culling and two-texture composition use GL. */

static int triangle_indices(const MvGxCaptureCommand *command, uint32_t triangle,
                            uint32_t out[3])
{
    switch (command->primitive) {
    case MV_GX_TRIANGLES:
        out[0] = triangle * 3; out[1] = out[0] + 1; out[2] = out[0] + 2;
        break;
    case MV_GX_QUADS: {
        uint32_t base = (triangle / 2) * 4;
        if (triangle & 1) { out[0] = base; out[1] = base + 2; out[2] = base + 3; }
        else { out[0] = base; out[1] = base + 1; out[2] = base + 2; }
        break;
    }
    case MV_GX_TRIANGLESTRIP:
        out[0] = triangle; out[1] = triangle + 1; out[2] = triangle + 2;
        if (triangle & 1) { uint32_t swap = out[0]; out[0] = out[1]; out[1] = swap; }
        break;
    case MV_GX_TRIANGLEFAN:
        out[0] = 0; out[1] = triangle + 1; out[2] = triangle + 2;
        break;
    default:
        return -1;
    }
    if (out[0] >= command->vertex_count || out[1] >= command->vertex_count ||
        out[2] >= command->vertex_count) return -1;
    return 0;
}


static uint8_t clamp_u8_int(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static uint8_t tev_lerp_u8(uint8_t a, uint8_t b, uint8_t c)
{
    return (uint8_t)(((unsigned)a * (255u - c) + (unsigned)b * c + 127u) / 255u);
}

static uint8_t tev_scale_u8(uint8_t value, float factor)
{
    if (!isfinite(factor)) return 0;
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    return (uint8_t)(value * factor + 0.5f);
}

static uint8_t tev_apply_scale_u8(unsigned value, uint8_t scale)
{
    switch (scale) {
    case GX_CS_SCALE_2: value *= 2u; break;
    case GX_CS_SCALE_4: value *= 4u; break;
    case GX_CS_DIVIDE_2: value = (value + 1u) / 2u; break;
    default: break;
    }
    return (uint8_t)(value > 255u ? 255u : value);
}

static void bake_custom_tev_pixel(const MvGxMaterialState *material, uint8_t rgba[4])
{
    if (!material->tev_valid ||
        !mv_gx_material_custom_tev_cpu_bakeable(material)) return;

    const uint32_t active = material->tev_active;
    const uint8_t *op = material->tev_op;
    uint8_t tex[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    if (active & 0x40000000u) {
        for (unsigned c = 0; c < 3; ++c) {
            unsigned v = tev_lerp_u8(material->tev0[c],
                                     material->tev_konst[c], tex[c]);
            rgba[c] = tev_apply_scale_u8(v, op[4]);
        }
    }
    if (active & 0x80000000u) {
        unsigned v = tev_lerp_u8(material->tev0[3],
                                 material->tev_konst[3], tex[3]);
        rgba[3] = tev_apply_scale_u8(v, op[5]);
    }
}

static void bake_material_pixel(const MvGxMaterialState *material, uint8_t rgba[4])
{
    bake_custom_tev_pixel(material, rgba);
    uint8_t src[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    uint8_t mat[4] = {
        (uint8_t)(material->material_rgba >> 24),
        (uint8_t)(material->material_rgba >> 16),
        (uint8_t)(material->material_rgba >> 8),
        (uint8_t)material->material_rgba,
    };
    unsigned colormap = (material->tobj_flags >> 16) & 0xfu;
    unsigned alphamap = (material->tobj_flags >> 20) & 0xfu;
    uint8_t blend = tev_scale_u8(255, material->blending);

    for (unsigned c = 0; c < 3; ++c) {
        switch (colormap) {
        case 0: /* NONE */
        case 6: /* PASS */
            rgba[c] = mat[c];
            break;
        case 1: /* ALPHA_MASK */
            rgba[c] = tev_lerp_u8(mat[c], src[c], src[3]);
            break;
        case 2: /* RGB_MASK */
            rgba[c] = tev_lerp_u8(mat[c], src[c], src[c]);
            break;
        case 3: /* BLEND */
            rgba[c] = tev_lerp_u8(mat[c], src[c], blend);
            break;
        case 4: /* MODULATE */
            rgba[c] = tev_lerp_u8(0, mat[c], src[c]);
            break;
        case 5: /* REPLACE */
            rgba[c] = src[c];
            break;
        case 7: /* ADD */
            rgba[c] = clamp_u8_int((int)mat[c] + src[c]);
            break;
        case 8: /* SUB */
            rgba[c] = clamp_u8_int((int)mat[c] - src[c]);
            break;
        default:
            rgba[c] = 0;
            break;
        }
    }

    switch (alphamap) {
    case 0: /* NONE */
    case 5: /* PASS */
        rgba[3] = mat[3];
        break;
    case 1: /* ALPHA_MASK */
        rgba[3] = tev_lerp_u8(mat[3], src[3], src[3]);
        break;
    case 2: /* BLEND */
        rgba[3] = tev_lerp_u8(mat[3], src[3], blend);
        break;
    case 3: /* MODULATE */
        rgba[3] = tev_lerp_u8(0, mat[3], src[3]);
        break;
    case 4: /* REPLACE */
        rgba[3] = src[3];
        break;
    case 6: /* ADD */
        rgba[3] = clamp_u8_int((int)mat[3] + src[3]);
        break;
    case 7: /* SUB */
        rgba[3] = clamp_u8_int((int)mat[3] - src[3]);
        break;
    default:
        rgba[3] = 0;
        break;
    }
}


static void second_layer_material(const MvGxMaterialState *source, MvGxMaterialState *out)
{
    memset(out, 0, sizeof(*out));
    out->image = source->image1;
    out->palette = source->palette1;
    out->material_rgba = 0xffffffffu;
    out->tobj_flags = source->tobj1_flags;
    out->blending = source->blending1;
    out->width = source->width1;
    out->height = source->height1;
    out->palette_entries = source->palette_entries1;
    out->format = source->format1;
    out->palette_format = source->palette_format1;
    out->wrap_s = source->wrap_s1;
    out->wrap_t = source->wrap_t1;
    out->mag_filter = source->mag_filter1;
    out->texture_count = 1;
    out->uv_mtx_valid = source->uv_mtx1_valid;
    memcpy(out->uv_mtx, source->uv_mtx1, sizeof(out->uv_mtx));
    out->tev_valid = source->tev1_valid;
    memcpy(out->tev_op, source->tev1_op, sizeof(out->tev_op));
    memcpy(out->tev_konst, source->tev1_konst, sizeof(out->tev_konst));
    memcpy(out->tev0, source->tev1_reg0, sizeof(out->tev0));
    memcpy(out->tev1, source->tev1_reg1, sizeof(out->tev1));
    out->tev_active = source->tev1_active;
}

static int material_needs_bake(const MvGxMaterialState *m)
{
    const unsigned colormap = (m->tobj_flags >> 16) & 0xfu;
    const unsigned alphamap = (m->tobj_flags >> 20) & 0xfu;
    return (m->tev_valid && m->tev_active != 0 &&
            mv_gx_material_custom_tev_cpu_bakeable(m)) ||
           !((colormap == 4u || colormap == 5u) &&
             (alphamap == 3u || alphamap == 4u));
}

static int texture_key_equal(const MvGxMaterialState *a,
                             const MvGxMaterialState *b)
{
    if (a->image != b->image || a->palette != b->palette ||
        a->width != b->width || a->height != b->height ||
        a->palette_entries != b->palette_entries || a->format != b->format ||
        a->palette_format != b->palette_format || a->wrap_s != b->wrap_s ||
        a->wrap_t != b->wrap_t || a->mag_filter != b->mag_filter)
        return 0;

    const int bake_a = material_needs_bake(a);
    const int bake_b = material_needs_bake(b);
    if (bake_a != bake_b) return 0;
    if (!bake_a) return 1;

    return a->material_rgba == b->material_rgba &&
           a->tobj_flags == b->tobj_flags && a->blending == b->blending &&
           a->tev_valid == b->tev_valid && a->tev_active == b->tev_active &&
           (!a->tev_valid ||
            (!memcmp(a->tev_op, b->tev_op, sizeof(a->tev_op)) &&
             !memcmp(a->tev_konst, b->tev_konst, sizeof(a->tev_konst)) &&
             !memcmp(a->tev0, b->tev0, sizeof(a->tev0)) &&
             !memcmp(a->tev1, b->tev1, sizeof(a->tev1))));
}

static uint32_t material_draw_color(const MvGxMaterialState *m)
{
    if (material_needs_bake(m)) return 0xffffffffu;
    const unsigned colormap = (m->tobj_flags >> 16) & 0xfu;
    const unsigned alphamap = (m->tobj_flags >> 20) & 0xfu;
    uint32_t color = colormap == 4u ? (m->material_rgba & 0xffffff00u)
                                    : 0xffffff00u;
    color |= alphamap == 3u ? (m->material_rgba & 0xffu) : 0xffu;
    return color;
}

static GLuint prepare(MvGxReplay *r,const MvGxMaterialState *m)
{
    const uint32_t use=++r->use_serial;
    for(unsigned i=0;i<r->texture_count;i++)
        if(texture_key_equal(&r->textures[i].key,m)) {
            r->textures[i].last_use=use;
            return r->textures[i].id;
        }
    if(!m->image || !m->width || !m->height || m->wrap_s>2 || m->wrap_t>2) return 0;
    size_t size=mv_gx_texture_size(m->width,m->height,m->format);
    size_t bytes=(size_t)m->width*m->height*4;
    uint8_t *pixels=malloc(bytes);
    if(!pixels) return 0;
    if(mv_gx_decode(pixels,bytes,m->width*4,m->image,size,m->width,m->height,m->format,
                    m->palette,(size_t)m->palette_entries*2,m->palette_format)) {free(pixels);return 0;}
    if(material_needs_bake(m))
        for(size_t i=0;i<bytes;i+=4) bake_material_pixel(m,pixels+i);
    GLuint id;glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D,id);
    const GLenum wrap[]={GL_CLAMP_TO_EDGE,GL_REPEAT,GL_MIRRORED_REPEAT};
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap[m->wrap_s]);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap[m->wrap_t]);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,m->mag_filter?GL_LINEAR:GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,m->width,m->height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    free(pixels);
    if(glGetError()!=GL_NO_ERROR) {glDeleteTextures(1,&id);return 0;}
    unsigned slot;
    if(r->texture_count<r->texture_capacity) slot=r->texture_count++;
    else {
        slot=0;
        for(unsigned i=1;i<r->texture_capacity;i++)
            if(r->textures[i].last_use<r->textures[slot].last_use) slot=i;
        glDeleteTextures(1,&r->textures[slot].id);
        r->texture_bytes-=r->textures[slot].bytes;
    }
    r->textures[slot]=(MvGlTexture){.key=*m,.id=id,.bytes=(unsigned)bytes,.last_use=use};
    r->texture_bytes+=(unsigned)bytes;
    return id;
}
static int alpha_compare_supported(const MvGxMaterialState *m)
{
    if (m->pe_alpha_comp0 == GX_ALWAYS && m->pe_alpha_comp1 == GX_ALWAYS) return 1;
    return m->pe_alpha_comp0 == GX_GREATER && m->pe_alpha_ref0 == 0 &&
           m->pe_alpha_op == GX_AOP_OR && m->pe_alpha_comp1 == GX_NEVER;
}

static void apply_alpha_compare(const MvGxMaterialState *m)
{
    if (m->pe_alpha_comp0 == GX_ALWAYS && m->pe_alpha_comp1 == GX_ALWAYS) {
        glDisable(GL_ALPHA_TEST);
        return;
    }
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, (float)m->pe_alpha_ref0 / 255.0f);
}

static int supported(const MvGxCaptureCommand *c,int relaxed)
{
    unsigned bad=c->material.unsupported;
    bad &= ~(MV_GX_MATERIAL_UNSUPPORTED_COLORMAP|MV_GX_MATERIAL_UNSUPPORTED_ALPHAMAP);
    if(mv_gx_material_multitex_vitagl_supported(&c->material)) bad &= ~MV_GX_MATERIAL_UNSUPPORTED_MULTITEX;
    if(mv_gx_material_custom_tev_cpu_bakeable(&c->material)) bad &= ~MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV;
    /* Retain existing title bridge's explicit approximation until general TEV. */
    if(relaxed) bad &= ~(MV_GX_MATERIAL_UNSUPPORTED_CUSTOM_TEV|MV_GX_MATERIAL_UNSUPPORTED_MULTITEX);
    return !bad && (c->attr_mask&(1u<<MV_GX_VA_POS)) && c->material.pe_blend_type<=1 &&
        !c->material.pe_dst_alpha_enable && alpha_compare_supported(&c->material);
}
int mv_gx_replay_init_relaxed_from(MvGxReplay *r,const MvCamera *camera,FILE *log,uint32_t relaxed)
{
    (void)camera;memset(r,0,sizeof(*r));r->log=log;r->relaxed_from_command=relaxed;
    r->texture_capacity=MV_GX_REPLAY_TEXTURE_LIMIT;
    r->textures=calloc(r->texture_capacity,sizeof(*r->textures));
    if(!r->textures) {
        if(log) {fprintf(log,"VITAGL_REPLAY_READY result=0 reason=texture_cache_alloc capacity=%u\n",r->texture_capacity);fflush(log);}
        memset(r,0,sizeof(*r));
        return -1;
    }
    uint32_t count;const MvGxCaptureCommand *c=mv_gx_capture_commands(&count);
    glActiveTexture(GL_TEXTURE0);
    for(unsigned i=0;i<count;i++) {
        if(!supported(c+i,i>=relaxed)) {r->skipped_commands++;continue;}
        if(c[i].material.texture_count && !prepare(r,&c[i].material)) r->texture_failures++;
        if(c[i].material.texture_count==2 && mv_gx_material_multitex_vitagl_supported(&c[i].material)) {
            MvGxMaterialState second;second_layer_material(&c[i].material,&second);
            if(!prepare(r,&second)) r->texture_failures++;
        }
    }
    r->ready=!r->texture_failures;
    if(log) {fprintf(log,"VITAGL_REPLAY_READY result=%d commands=%u textures=%u capacity=%u bytes=%u skipped=%u texture_failures=%u relaxed_from=%u\n",r->ready,count,r->texture_count,r->texture_capacity,r->texture_bytes,r->skipped_commands,r->texture_failures,relaxed);fflush(log);}
    if(!r->ready) {mv_gx_replay_close(r);return -1;}
    return 0;
}
int mv_gx_replay_init(MvGxReplay *r,const MvCamera *c,FILE *f)
{return mv_gx_replay_init_relaxed_from(r,c,f,UINT32_MAX);}
int mv_gx_replay_init_streaming(MvGxReplay *r, FILE *log)
{
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    r->log = log;
    r->relaxed_from_command = 0;
    r->texture_capacity = MV_GX_REPLAY_TEXTURE_LIMIT;
    r->textures = calloc(r->texture_capacity, sizeof(*r->textures));
    if (!r->textures) return -1;
    r->ready = 1;
    if (log) {
        fprintf(log, "VITAGL_REPLAY_STREAMING_READY result=1 capacity=%u\n",
                r->texture_capacity);
        fflush(log);
    }
    return 0;
}

static void norm(float *a)
{float n=sqrtf(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);if(n)for(int i=0;i<3;i++)a[i]/=n;}
static void cross(const float *a,const float *b,float *v)
{v[0]=a[1]*b[2]-a[2]*b[1];v[1]=a[2]*b[0]-a[0]*b[2];v[2]=a[0]*b[1]-a[1]*b[0];}
static void captured_viewport(const MvGxCaptureCommand *c);
void mv_gx_replay_draw(MvGxReplay *r,const MvCamera *cam)
{
    if(!r || !r->ready || !cam || cam->projection_type!=1) return;
    float w=544*cam->aspect;
    glViewport((int)((960-w)*0.5f),0,(int)w,544);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();
    float n=cam->near_z>0?cam->near_z:0.01f;
    float top=n*tanf(cam->fov_y*0.00872664626f);
    glFrustum(-top*cam->aspect,top*cam->aspect,-top,top,n,cam->far_z);
    float f[3],right[3],up[3];for(int i=0;i<3;i++)f[i]=cam->interest[i]-cam->eye[i];
    norm(f);cross(f,cam->up,right);norm(right);cross(right,f,up);
    float view[16]={right[0],up[0],-f[0],0,right[1],up[1],-f[1],0,right[2],up[2],-f[2],0,0,0,0,1};
    glMatrixMode(GL_MODELVIEW);glLoadMatrixf(view);glTranslatef(-cam->eye[0],-cam->eye[1],-cam->eye[2]);
    uint32_t count,nverts;const MvGxCaptureCommand *cmd=mv_gx_capture_commands(&count);
    const MvGxCaptureVertex *verts=mv_gx_capture_vertices(&nverts);
    const GLenum compares[]={GL_NEVER,GL_LESS,GL_EQUAL,GL_LEQUAL,GL_GREATER,GL_NOTEQUAL,GL_GEQUAL,GL_ALWAYS};
    const GLenum factors[]={GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    unsigned submitted=0;
    for(unsigned i=0;i<count;i++) {
        const MvGxCaptureCommand *c=cmd+i;const MvGxMaterialState *m=&c->material;
        if(!supported(c,i>=r->relaxed_from_command) || c->first_vertex>nverts || c->vertex_count>nverts-c->first_vertex || m->pe_z_func>7 || m->pe_src_factor>7 || m->pe_dst_factor>7 || c->cull_mode==3) continue;
        if (c->projection_valid) {
            captured_viewport(c);
            float projection[16];
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    projection[col * 4 + row] = c->projection[row][col];
            glMatrixMode(GL_PROJECTION); glLoadMatrixf(projection);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        }
        if(m->pe_blend_type) {glEnable(GL_BLEND);glBlendFunc(m->pe_src_factor==2?GL_DST_COLOR:m->pe_src_factor==3?GL_ONE_MINUS_DST_COLOR:factors[m->pe_src_factor],factors[m->pe_dst_factor]);} else glDisable(GL_BLEND);
        if(m->pe_z_enable) glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
        glDepthFunc(compares[m->pe_z_func]);glDepthMask(m->pe_z_update);
        glColorMask(m->pe_color_update,m->pe_color_update,m->pe_color_update,m->pe_alpha_update);
        apply_alpha_compare(m);glFrontFace(GL_CW);
        if(c->cull_mode) {glEnable(GL_CULL_FACE);glCullFace(c->cull_mode==1?GL_FRONT:GL_BACK);} else glDisable(GL_CULL_FACE);
        int two=m->texture_count==2 && mv_gx_material_multitex_vitagl_supported(m);
        glActiveTexture(GL_TEXTURE0);
        if(m->texture_count) {GLuint id=prepare(r,m);if(!id)continue;glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,id);glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);} else glDisable(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE1);
        if(two) {
            MvGxMaterialState second;second_layer_material(m,&second);GLuint id=prepare(r,&second);if(!id)continue;
            glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,id);
            glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_RGB,GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV,GL_SRC0_RGB,GL_PREVIOUS);glTexEnvi(GL_TEXTURE_ENV,GL_SRC1_RGB,GL_TEXTURE);
            if(((m->tobj1_flags>>20)&0xfu)==3u) {
                glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_ALPHA,GL_MODULATE);
                glTexEnvi(GL_TEXTURE_ENV,GL_SRC0_ALPHA,GL_PREVIOUS);
                glTexEnvi(GL_TEXTURE_ENV,GL_SRC1_ALPHA,GL_TEXTURE);
            } else {
                glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_ALPHA,GL_REPLACE);
                glTexEnvi(GL_TEXTURE_ENV,GL_SRC0_ALPHA,GL_PREVIOUS);
            }
        } else glDisable(GL_TEXTURE_2D);
        glActiveTexture(GL_TEXTURE0);
        glPushMatrix();float model[16]={0};model[15]=1;
        for(int row=0;row<3;row++)for(int col=0;col<4;col++)model[col*4+row]=c->pos_mtx[row][col];
        glMultMatrixf(model);glBegin(GL_TRIANGLES);
        for(unsigned t=0;t<c->triangle_count;t++) {
            uint32_t ids[3];if(triangle_indices(c,t,ids))continue;
            for(int k=0;k<3;k++) {
                const MvGxCaptureVertex *v=verts+c->first_vertex+ids[k];
                unsigned color=m->texture_count?material_draw_color(m):m->material_rgba;
                if(v->present&(1u<<MV_GX_VA_CLR0))color=v->color0;
                glColor4ub(color>>24,color>>16,color>>8,color);
                for(int unit=0;unit<(two?2:1);unit++) {
                    const float (*mat)[3]=unit?m->uv_mtx1:m->uv_mtx;
                    int valid=unit?m->uv_mtx1_valid:m->uv_mtx_valid;
                    float u=v->tex0[0],vv=v->tex0[1];
                    glMultiTexCoord2f(GL_TEXTURE0+unit,valid?mat[0][0]*u+mat[0][1]*vv+mat[0][2]:u,valid?mat[1][0]*u+mat[1][1]*vv+mat[1][2]:vv);
                }
                glVertex3fv(v->position);
            }
        }
        glEnd();glPopMatrix();submitted++;
    }
    r->submitted_commands=submitted;
    if(!r->submit_logged && r->log) {fprintf(r->log,"VITAGL_REPLAY_SUBMIT commands=%u total=%u gl_error=%x perspective=GPU\n",submitted,count,glGetError());fflush(r->log);r->submit_logged=1;}
    glActiveTexture(GL_TEXTURE1);glDisable(GL_TEXTURE_2D);glActiveTexture(GL_TEXTURE0);
}

static void captured_viewport(const MvGxCaptureCommand *c)
{
    const float scale = 544.0f / 480.0f;
    const float base_x = (960.0f - 640.0f * scale) * 0.5f;
    float x = 0.0f, y = 0.0f, w = 640.0f, h = 480.0f;
    float near_z = 0.0f, far_z = 1.0f;
    if (c->viewport_valid) {
        x = c->viewport[0]; y = c->viewport[1];
        w = c->viewport[2]; h = c->viewport[3];
        near_z = c->viewport[4]; far_z = c->viewport[5];
    }
    int vx = (int)(base_x + x * scale + 0.5f);
    int vy = (int)((480.0f - (y + h)) * scale + 0.5f);
    int vw = (int)(w * scale + 0.5f);
    int vh = (int)(h * scale + 0.5f);
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > 960) vw = 960 - vx;
    if (vy + vh > 544) vh = 544 - vy;
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;
    glViewport(vx, vy, vw, vh);
    glDepthRangef(near_z, far_z);
}

void mv_gx_replay_draw_captured(MvGxReplay *r)
{
    if (!r || !r->ready) return;
    uint32_t count, nverts;
    const MvGxCaptureCommand *cmd = mv_gx_capture_commands(&count);
    const MvGxCaptureVertex *verts = mv_gx_capture_vertices(&nverts);
    const GLenum compares[] = {GL_NEVER,GL_LESS,GL_EQUAL,GL_LEQUAL,GL_GREATER,GL_NOTEQUAL,GL_GEQUAL,GL_ALWAYS};
    const GLenum factors[] = {GL_ZERO,GL_ONE,GL_SRC_COLOR,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_DST_ALPHA,GL_ONE_MINUS_DST_ALPHA};
    unsigned submitted = 0;

    for (unsigned i = 0; i < count; ++i) {
        const MvGxCaptureCommand *c = cmd + i;
        const MvGxMaterialState *m = &c->material;
        if (!c->projection_valid || !supported(c, 1) ||
            c->first_vertex > nverts || c->vertex_count > nverts - c->first_vertex ||
            m->pe_z_func > 7 || m->pe_src_factor > 7 || m->pe_dst_factor > 7 ||
            c->cull_mode == 3)
            continue;

        captured_viewport(c);
        float projection[16];
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                projection[col * 4 + row] = c->projection[row][col];
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(projection);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        if (m->pe_blend_type) {
            glEnable(GL_BLEND);
            glBlendFunc(m->pe_src_factor == 2 ? GL_DST_COLOR :
                        m->pe_src_factor == 3 ? GL_ONE_MINUS_DST_COLOR : factors[m->pe_src_factor],
                        factors[m->pe_dst_factor]);
        } else {
            glDisable(GL_BLEND);
        }
        if (m->pe_z_enable) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthFunc(compares[m->pe_z_func]);
        glDepthMask(m->pe_z_update);
        glColorMask(m->pe_color_update, m->pe_color_update,
                    m->pe_color_update, m->pe_alpha_update);
        apply_alpha_compare(m);
        glFrontFace(GL_CW);
        if (c->cull_mode) {
            glEnable(GL_CULL_FACE);
            glCullFace(c->cull_mode == 1 ? GL_FRONT : GL_BACK);
        } else {
            glDisable(GL_CULL_FACE);
        }

        int two = m->texture_count == 2 && mv_gx_material_multitex_vitagl_supported(m);
        glActiveTexture(GL_TEXTURE0);
        if (m->texture_count) {
            GLuint id = prepare(r, m);
            if (!id) continue;
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        glActiveTexture(GL_TEXTURE1);
        if (two) {
            MvGxMaterialState second;
            second_layer_material(m, &second);
            GLuint id = prepare(r, &second);
            if (!id) continue;
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_PREVIOUS);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_TEXTURE);
            if (((m->tobj1_flags >> 20) & 0xfu) == 3u) {
                glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
                glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PREVIOUS);
                glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_TEXTURE);
            } else {
                glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
                glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PREVIOUS);
            }
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        glActiveTexture(GL_TEXTURE0);

        glPushMatrix();
        float model[16] = {0};
        model[15] = 1.0f;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                model[col * 4 + row] = c->pos_mtx[row][col];
        glMultMatrixf(model);
        glBegin(GL_TRIANGLES);
        for (unsigned t = 0; t < c->triangle_count; ++t) {
            uint32_t ids[3];
            if (triangle_indices(c, t, ids)) continue;
            for (int k = 0; k < 3; ++k) {
                const MvGxCaptureVertex *v = verts + c->first_vertex + ids[k];
                unsigned color = m->texture_count ? material_draw_color(m) : m->material_rgba;
                if (v->present & (1u << MV_GX_VA_CLR0)) color = v->color0;
                glColor4ub(color >> 24, color >> 16, color >> 8, color);
                for (int unit = 0; unit < (two ? 2 : 1); ++unit) {
                    const float (*mat)[3] = unit ? m->uv_mtx1 : m->uv_mtx;
                    int valid = unit ? m->uv_mtx1_valid : m->uv_mtx_valid;
                    float u = v->tex0[0], vv = v->tex0[1];
                    glMultiTexCoord2f(GL_TEXTURE0 + unit,
                        valid ? mat[0][0] * u + mat[0][1] * vv + mat[0][2] : u,
                        valid ? mat[1][0] * u + mat[1][1] * vv + mat[1][2] : vv);
                }
                glVertex3fv(v->position);
            }
        }
        glEnd();
        glPopMatrix();
        ++submitted;
    }

    r->submitted_commands = submitted;
    if (!r->submit_logged && r->log) {
        fprintf(r->log,
                "VITAGL_REPLAY_CAPTURED_SUBMIT commands=%u total=%u gl_error=%x camera=per-command\n",
                submitted, count, glGetError());
        fflush(r->log);
        r->submit_logged = 1;
    }
    glDepthRangef(0.0f, 1.0f);
    glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
}

void mv_gx_replay_close(MvGxReplay *r)
{if(!r)return;glFinish();for(unsigned i=0;i<r->texture_count;i++)glDeleteTextures(1,&r->textures[i].id);free(r->textures);memset(r,0,sizeof(*r));}
