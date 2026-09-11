#include "hsd_anim_native.h"
#include "hsd_anim.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define LIMIT 4096u
/* Typed immutable conversion; compressed FObj values are already little-endian
 * byte streams in the original format and must not be swapped. */
typedef struct { uint32_t offset; unsigned type, busy; void *value; } Entry;
typedef struct {
    const MvDat *dat;
    MvNativeAnim *out;
    int error;
    int allow_obj_refs;
} Context;
static float scalar(const uint8_t *p)
{ uint32_t u=mv_be32(p); float f; memcpy(&f,&u,4); return f; }
static void *convert(Context*, uint32_t, unsigned, unsigned);

static int jobj_fobj_type_supported(uint8_t type)
{
    return (type >= 1 && type <= 12) || (type >= 20 && type <= 42);
}
static void *reference(Context *c, uint32_t field, unsigned type, unsigned depth)
{
    uint32_t offset;
    int r=mv_dat_pointer(c->dat,field,&offset);
    if (r<0) { c->error=-1; return NULL; }
    return r ? convert(c,offset,type,depth+1) : NULL;
}
static void *convert(Context *c, uint32_t offset, unsigned type, unsigned depth)
{
    if (c->error || depth>256 || c->out->count>=LIMIT) { c->error=-1; return NULL; }
    Entry *entries=c->out->entries;
    for (unsigned i=0;i<c->out->count;i++) if(entries[i].offset==offset && entries[i].type==type) {
        if(entries[i].busy) { c->error=-1; return NULL; }
        return entries[i].value;
    }
    size_t bytes=type==1?20:type==2?16:20;
    const uint8_t *p=mv_dat_span(c->dat,offset,bytes);
    if (!p) { c->error=-1; return NULL; }
    Entry *e=&entries[c->out->count++];
    e->offset=offset;e->type=type;e->busy=1;
    e->value=calloc(1,type==1?sizeof(HSD_AnimJoint):type==2?sizeof(HSD_AObjDesc):sizeof(HSD_FObjDesc));
    if (!e->value) { c->error=-1; return NULL; }
    if(type==1) {
        HSD_AnimJoint *a=e->value; uint32_t unused;
        a->child=reference(c,offset,1,depth);a->next=reference(c,offset+4,1,depth);
        a->aobjdesc=reference(c,offset+8,2,depth);a->flags=mv_be32(p+16);
        if(mv_dat_pointer(c->dat,offset+12,&unused)!=0) c->error=-2;
    } else if(type==2) {
        HSD_AObjDesc *a=e->value; uint32_t obj_target;
        a->flags=mv_be32(p);a->end_frame=scalar(p+4);
        a->fobjdesc=reference(c,offset+8,3,depth);
        int obj_ref=mv_dat_pointer(c->dat,offset+12,&obj_target);
        if(obj_ref<0 || (obj_ref==1 && !c->allow_obj_refs) ||
           !isfinite(a->end_frame) || a->end_frame<0) c->error=-2;
        /* A validation-only build never escapes to HSD_AObjLoadDesc, so do not
           manufacture a native obj_id pointer here.  The raw archive nativeizer
           validates/converts that referenced JObj separately and leaves the
           relocation field untouched for HSD_ArchiveParse. */
        a->obj_id=0;
        /* Validate the compressed streams with the independent bounded reader. */
        if(mv_aobj_validate_jobj(c->dat,offset)) c->error=-2;
    } else {
        HSD_FObjDesc *f=e->value;uint32_t stream;
        f->next=reference(c,offset,3,depth);f->length=mv_be32(p+4);f->startframe=scalar(p+8);
        f->type=p[12];f->frac_value=p[13];f->frac_slope=p[14];
        if(!isfinite(f->startframe) || f->startframe < -32768 || f->startframe > 32767 ||
           !jobj_fobj_type_supported(f->type) ||
           mv_dat_pointer(c->dat,offset+16,&stream)!=1 ||
           !(f->ad=(uint8_t*)mv_dat_span(c->dat,stream,f->length))) c->error=-2;
    }
    e->busy=0;return e->value;
}
void mv_native_anim_free(MvNativeAnim *a)
{
    if (!a) return;
    Entry *e=a->entries;
    for(unsigned i=0;i<a->count;i++) free(e[i].value);
    free(e);memset(a,0,sizeof(*a));
}
int mv_native_anim_build_at(const MvDat *dat,uint32_t root_offset,MvNativeAnim *out)
{
    if(!dat || !out || root_offset > dat->data_size) return -1;
    memset(out,0,sizeof(*out));out->entries=calloc(LIMIT,sizeof(Entry));
    if(!out->entries) return -1;
    Context c={dat,out,0,0};
    out->root=convert(&c,root_offset,1,0);
    if(!out->root || c.error) { int r=c.error?c.error:-1;mv_native_anim_free(out);return r; }
    return 0;
}

int mv_native_anim_validate_at(const MvDat *dat, uint32_t root_offset)
{
    if(!dat || root_offset > dat->data_size) return -1;
    MvNativeAnim out;
    memset(&out,0,sizeof(out));
    out.entries=calloc(LIMIT,sizeof(Entry));
    if(!out.entries) return -1;
    Context c={dat,&out,0,1};
    out.root=convert(&c,root_offset,1,0);
    int result=(!out.root || c.error) ? (c.error ? c.error : -1) : 0;
    mv_native_anim_free(&out);
    return result;
}
int mv_native_anim_build(const MvDat *dat,const char *name,MvNativeAnim *out)
{
    if(!dat || !name || !out) return -1;
    for(uint32_t i=0;i<dat->public_count;i++) {
        const char *symbol;uint32_t offset;
        if(mv_dat_public(dat,i,&symbol,&offset)) return -1;
        if(!strcmp(name,symbol)) return mv_native_anim_build_at(dat,offset,out);
    }
    return -1;
}
