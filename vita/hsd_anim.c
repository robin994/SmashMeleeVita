#include "hsd_anim.h"

#include <math.h>
#include <string.h>

#define MV_AOBJ_NO_UPDATE (1u << 28)
#define MV_AOBJ_LOOP (1u << 29)

#define MV_A_OP_CON 1u
#define MV_A_OP_LIN 2u
#define MV_A_OP_SPL0 3u
#define MV_A_OP_SPL 4u
#define MV_A_OP_SLP 5u
#define MV_A_OP_KEY 6u

#define MV_A_FRAC_S16 0x20u
#define MV_A_FRAC_U16 0x40u
#define MV_A_FRAC_S8 0x60u
#define MV_A_FRAC_U8 0x80u

#define MV_FOBJ_LOAD_DATA0 1u
#define MV_FOBJ_LOAD_DATA 2u
#define MV_FOBJ_LOAD_WAIT 3u
#define MV_FOBJ_INTERP 4u
#define MV_FOBJ_INTERP_READY 5u
#define MV_FOBJ_END 6u

#define MV_MAX_FOBJS 1024u

typedef struct {
    const uint8_t *ad, *end;
    uint8_t flags, op, op_intrp, obj_type, frac_value, frac_slope;
    uint16_t nb_pack, fterm;
    uint8_t state;
    float time, p0, p1, d0, d1;
} MvFObjState;

typedef int (*MvApplyChannelFn)(void *opaque, uint8_t type, float value);

static float be_float(const uint8_t *p)
{
    uint32_t bits = mv_be32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int read_pointer(const MvDat *dat, uint32_t field, uint32_t *target, uint8_t *present)
{
    int result = mv_dat_pointer(dat, field, target);
    if (result < 0) return -1;
    *present = result != 0;
    if (!result) *target = UINT32_MAX;
    return 0;
}

static int need(const MvFObjState *fobj, size_t count)
{
    return count <= (size_t)(fobj->end - fobj->ad) ? 0 : -1;
}

/* FObj AD streams are byte-packed little-endian by format, independent from
   the big-endian descriptor/archive fields. This mirrors upstream parseFloat. */
static int parse_float(MvFObjState *fobj, uint8_t frac, float *value)
{
    if (frac == 0) {
        if (need(fobj, 4)) return -1;
        uint32_t bits = (uint32_t)fobj->ad[0] | (uint32_t)fobj->ad[1] << 8 |
                        (uint32_t)fobj->ad[2] << 16 | (uint32_t)fobj->ad[3] << 24;
        memcpy(value, &bits, sizeof(*value));
        fobj->ad += 4;
        return isfinite(*value) ? 0 : -1;
    }

    float numer;
    switch (frac & 0xe0u) {
    case MV_A_FRAC_S8:
        if (need(fobj, 1)) return -1;
        numer = (float)(int8_t)*fobj->ad++;
        break;
    case MV_A_FRAC_U8:
        if (need(fobj, 1)) return -1;
        numer = (float)*fobj->ad++;
        break;
    case MV_A_FRAC_S16: {
        if (need(fobj, 2)) return -1;
        uint16_t raw = (uint16_t)fobj->ad[0] | (uint16_t)fobj->ad[1] << 8;
        numer = (float)(int16_t)raw;
        fobj->ad += 2;
        break;
    }
    case MV_A_FRAC_U16: {
        if (need(fobj, 2)) return -1;
        uint16_t raw = (uint16_t)fobj->ad[0] | (uint16_t)fobj->ad[1] << 8;
        numer = (float)raw;
        fobj->ad += 2;
        break;
    }
    default:
        /* Upstream returns 0.0 for a nonzero fraction with FLOAT storage. */
        *value = 0.0f;
        return 0;
    }
    *value = ldexpf(numer, -(int)(frac & 0x1fu));
    return isfinite(*value) ? 0 : -1;
}

static int parse_pack_info(MvFObjState *fobj, uint32_t *packs)
{
    if (need(fobj, 1)) return -1;
    uint8_t byte = *fobj->ad++;
    uint32_t count = ((byte >> 4) & 7u) + 1u;
    unsigned shift = 3;
    while (byte & 0x80u) {
        if (need(fobj, 1) || shift >= 32) return -1;
        byte = *fobj->ad++;
        count += (uint32_t)(byte & 0x7fu) << shift;
        shift += 7;
    }
    if (!count || count > UINT16_MAX) return -1;
    *packs = count;
    return 0;
}

static int parse_wait(MvFObjState *fobj, uint16_t *wait)
{
    uint32_t value = 0;
    unsigned shift = 0;
    uint8_t byte;
    do {
        if (need(fobj, 1) || shift >= 32) return -1;
        byte = *fobj->ad++;
        value |= (uint32_t)(byte & 0x7fu) << shift;
        shift += 7;
    } while (byte & 0x80u);
    *wait = (uint16_t)value;
    return 0;
}

static void launch_key_data(MvFObjState *fobj)
{
    if (fobj->flags & 0x40u) {
        fobj->op_intrp = fobj->op;
        fobj->flags &= (uint8_t)~0x40u;
        fobj->flags |= 0x80u;
        fobj->p0 = fobj->p1;
    }
}

static float hermite(float fterm, float time, float p0, float p1, float d0, float d1)
{
    float time2 = time * time;
    float term2 = fterm * fterm;
    float time2_term = time2 * fterm;
    float term2_time3 = term2 * (time2 * time);
    float two_time3_term3 = 2.0f * term2_time3 * fterm;
    float three_time2_term2 = 3.0f * time2 * term2;
    return d1 * (term2_time3 - time2_term) +
           d0 * (time + ((term2_time3 - time2_term) - time2_term)) +
           p0 * (1.0f + (two_time3_term3 - three_time2_term2)) +
           p1 * (-two_time3_term3 + three_time2_term2);
}

static int channel_value(MvFObjState *fobj, float *value, uint8_t *has_value)
{
    *has_value = 1;
    switch (fobj->op_intrp) {
    case MV_A_OP_KEY:
        if (!(fobj->flags & 0x80u)) { *has_value = 0; return 0; }
        *value = fobj->p0;
        fobj->flags &= (uint8_t)~0x80u;
        return 0;
    case MV_A_OP_CON:
        *value = fobj->time >= fobj->fterm ? fobj->p1 : fobj->p0;
        return 0;
    case MV_A_OP_LIN:
        if (fobj->flags & 0x20u) {
            fobj->flags &= (uint8_t)~0x20u;
            if (fobj->fterm != 0) fobj->d0 = (fobj->p1 - fobj->p0) / fobj->fterm;
            else { fobj->d0 = 0.0f; fobj->p0 = fobj->p1; }
        }
        *value = fobj->d0 * fobj->time + fobj->p0;
        return isfinite(*value) ? 0 : -1;
    case MV_A_OP_SPL0:
    case MV_A_OP_SPL:
    case MV_A_OP_SLP:
        *value = fobj->fterm != 0
            ? hermite(1.0f / fobj->fterm, fobj->time, fobj->p0, fobj->p1,
                      fobj->d0, fobj->d1)
            : fobj->p1;
        return isfinite(*value) ? 0 : -1;
    default:
        *has_value = 0;
        return 0;
    }
}

static int apply_joint_channel(void *opaque, uint8_t type, float value)
{
    MvAnimJointSample *sample = opaque;
    if (type < 1 || type > 10 || type == 4) return 1;
    if (!isfinite(value)) return -1;
    sample->channels[type - 1] = value;
    sample->channel_mask |= (uint16_t)(1u << (type - 1));
    ++sample->applied_channel_count;
    return 0;
}

static int apply_aobj_channel(void *opaque, uint8_t type, float value)
{
    MvAObjSample *sample = opaque;
    if (type < 1 || type > 32) return 1;
    if (!isfinite(value)) return -1;
    sample->channels[type - 1] = value;
    sample->channel_mask |= 1u << (type - 1);
    ++sample->applied_channel_count;
    return 0;
}

static int update_channel(MvFObjState *fobj, void *opaque, MvApplyChannelFn apply)
{
    float value = 0.0f;
    uint8_t has_value = 0;
    if (channel_value(fobj, &value, &has_value)) return -1;
    return has_value ? apply(opaque, fobj->obj_type, value) : 0;
}

static int load_data(MvFObjState *fobj)
{
    if (fobj->ad >= fobj->end) return MV_FOBJ_END;
    fobj->op_intrp = fobj->op;
    if (fobj->nb_pack == 0) {
        fobj->op = *fobj->ad & 0x0fu;
        uint32_t packs;
        if (parse_pack_info(fobj, &packs)) return -1;
        fobj->nb_pack = (uint16_t)packs;
    }
    --fobj->nb_pack;
    uint8_t old_state = fobj->state;
    switch (fobj->op) {
    case MV_A_OP_CON:
    case MV_A_OP_LIN:
        fobj->p0 = fobj->p1;
        if (parse_float(fobj, fobj->frac_value, &fobj->p1)) return -1;
        if (fobj->op_intrp != MV_A_OP_SLP) { fobj->d0 = fobj->d1; fobj->d1 = 0.0f; }
        return old_state == MV_FOBJ_LOAD_DATA0 ? MV_FOBJ_LOAD_WAIT : MV_FOBJ_INTERP;
    case MV_A_OP_SPL0:
        fobj->p0 = fobj->p1; fobj->d0 = fobj->d1;
        if (parse_float(fobj, fobj->frac_value, &fobj->p1)) return -1;
        fobj->d1 = 0.0f;
        return old_state == MV_FOBJ_LOAD_DATA0 ? MV_FOBJ_LOAD_WAIT : MV_FOBJ_INTERP;
    case MV_A_OP_SPL:
        fobj->p0 = fobj->p1; fobj->d0 = fobj->d1;
        if (parse_float(fobj, fobj->frac_value, &fobj->p1) ||
            parse_float(fobj, fobj->frac_slope, &fobj->d1)) return -1;
        return old_state == MV_FOBJ_LOAD_DATA0 ? MV_FOBJ_LOAD_WAIT : MV_FOBJ_INTERP;
    case MV_A_OP_SLP:
        fobj->d0 = fobj->d1;
        if (parse_float(fobj, fobj->frac_slope, &fobj->d1)) return -1;
        return old_state;
    case MV_A_OP_KEY:
        launch_key_data(fobj);
        if (parse_float(fobj, fobj->frac_value, &fobj->p1)) return -1;
        fobj->flags |= 0x40u;
        return old_state == MV_FOBJ_LOAD_DATA0 ? MV_FOBJ_LOAD_WAIT : MV_FOBJ_INTERP;
    default:
        return 0;
    }
}

static int interpret_first(MvFObjState *fobj, void *opaque, MvApplyChannelFn apply,
                           int apply_updates)
{
    if (fobj->state == 0 || fobj->time < 0.0f) return 0;
    float carried_term = 0.0f;
    for (unsigned guard = 0; guard < 100000; ++guard) {
        switch (fobj->state) {
        case MV_FOBJ_END: {
            fobj->time += carried_term;
            launch_key_data(fobj);
            if (apply_updates) return update_channel(fobj, opaque, apply);
            return 0;
        }
        case MV_FOBJ_LOAD_DATA0:
        case MV_FOBJ_LOAD_DATA: {
            int state = load_data(fobj);
            if (state < 0) return -1;
            if (state == 0) return 1;
            fobj->state = (uint8_t)state;
            break;
        }
        case MV_FOBJ_LOAD_WAIT:
            if ((fobj->flags & 0x80u) && apply_updates) {
                int result = update_channel(fobj, opaque, apply);
                if (result) return result;
            }
            if (fobj->ad >= fobj->end) fobj->state = MV_FOBJ_END;
            else {
                if (parse_wait(fobj, &fobj->fterm)) return -1;
                fobj->flags |= 0x20u;
                fobj->state = MV_FOBJ_LOAD_DATA;
            }
            break;
        case MV_FOBJ_INTERP:
            if (fobj->fterm <= fobj->time) {
                carried_term = fobj->fterm;
                fobj->time -= fobj->fterm;
                fobj->state = MV_FOBJ_LOAD_WAIT;
            } else {
                if (apply_updates) {
                    int result = update_channel(fobj, opaque, apply);
                    if (result) return result;
                }
                fobj->state = MV_FOBJ_INTERP_READY;
                return 0;
            }
            break;
        case MV_FOBJ_INTERP_READY:
            fobj->state = MV_FOBJ_INTERP;
            break;
        default:
            return -1;
        }
    }
    return -1;
}

static int sample_fobj(const MvDat *dat, uint32_t desc, float frame,
                       int apply_updates, void *opaque, MvApplyChannelFn apply,
                       size_t *fobj_count)
{
    const uint8_t *p = mv_dat_span(dat, desc, 20);
    if (!p) return -1;
    uint32_t length = mv_be32(p + 4);
    float start = be_float(p + 8);
    if (!isfinite(start) || start < -32768.0f || start > 32767.0f) return -1;
    uint32_t ad_offset;
    uint8_t has_ad;
    if (read_pointer(dat, desc + 16, &ad_offset, &has_ad) || !has_ad) return -1;
    const uint8_t *ad = mv_dat_span(dat, ad_offset, length);
    if (!ad && length) return -1;

    MvFObjState state;
    memset(&state, 0, sizeof(state));
    state.ad = ad;
    state.end = ad + length;
    state.obj_type = p[12];
    state.frac_value = p[13];
    state.frac_slope = p[14];
    state.time = (float)(int16_t)start + frame;
    state.state = MV_FOBJ_LOAD_DATA0;
    ++*fobj_count;
    return interpret_first(&state, opaque, apply, apply_updates);
}

static int sample_aobj_desc(const MvDat *dat, uint32_t aobj, float frame,
                            void *opaque, MvApplyChannelFn apply, size_t *fobj_count,
                            uint32_t *flags_out, float *end_frame_out, uint32_t *obj_id_out)
{
    const uint8_t *aobj_desc = mv_dat_span(dat, aobj, 16);
    if (!aobj_desc) return -1;
    uint32_t aobj_flags = mv_be32(aobj_desc);
    float end_frame = be_float(aobj_desc + 4);
    if (!isfinite(end_frame)) return -1;
    uint32_t obj_id = mv_be32(aobj_desc + 12);
    if (flags_out) *flags_out = aobj_flags;
    if (end_frame_out) *end_frame_out = end_frame;
    if (obj_id_out) *obj_id_out = obj_id;
    if (obj_id != 0) return 1; /* Upstream resolves this through the HSD ID table. */

    float requested = frame;
    if ((aobj_flags & MV_AOBJ_LOOP) && end_frame <= requested) {
        if (end_frame > 0.0f) requested = fmodf(requested, end_frame);
        else requested = end_frame;
    }
    int apply_updates = (aobj_flags & MV_AOBJ_NO_UPDATE) == 0;
    uint32_t fobj;
    uint8_t has_fobj;
    if (read_pointer(dat, aobj + 8, &fobj, &has_fobj)) return -1;
    uint32_t seen[MV_MAX_FOBJS];
    size_t seen_count = 0;
    while (has_fobj) {
        for (size_t i = 0; i < seen_count; ++i)
            if (seen[i] == fobj) return -1;
        if (seen_count >= MV_MAX_FOBJS) return -1;
        seen[seen_count++] = fobj;
        int result = sample_fobj(dat, fobj, requested, apply_updates, opaque, apply,
                                 fobj_count);
        if (result) return result;
        uint32_t next;
        uint8_t has_next;
        if (read_pointer(dat, fobj, &next, &has_next)) return -1;
        fobj = next; has_fobj = has_next;
    }
    return 0;
}

int mv_anim_joint_sample(const MvDat *dat, uint32_t anim_joint, float frame,
                         MvAnimJointSample *sample)
{
    if (!dat || !sample || !isfinite(frame)) return -1;
    memset(sample, 0, sizeof(*sample));
    const uint8_t *joint = mv_dat_span(dat, anim_joint, 20);
    if (!joint) return -1;

    uint32_t aobj, robj;
    uint8_t has_aobj, has_robj;
    if (read_pointer(dat, anim_joint, &sample->child, &sample->has_child) ||
        read_pointer(dat, anim_joint + 4, &sample->next, &sample->has_next) ||
        read_pointer(dat, anim_joint + 8, &aobj, &has_aobj) ||
        read_pointer(dat, anim_joint + 12, &robj, &has_robj)) return -1;
    sample->flags = mv_be32(joint + 16);
    if (has_robj) return 1; /* RObj animation needs constraint/reference adapters. */
    if (!has_aobj) return 0;

    ++sample->aobj_count;
    return sample_aobj_desc(dat, aobj, frame, sample, apply_joint_channel,
                            &sample->fobj_count, NULL, NULL, NULL);
}

int mv_anim_joint_sample_frame0(const MvDat *dat, uint32_t anim_joint,
                                MvAnimJointSample *sample)
{
    return mv_anim_joint_sample(dat, anim_joint, 0.0f, sample);
}

int mv_aobj_sample(const MvDat *dat, uint32_t aobj_desc, float frame,
                   MvAObjSample *sample)
{
    if (!dat || !sample || !isfinite(frame)) return -1;
    memset(sample, 0, sizeof(*sample));
    return sample_aobj_desc(dat, aobj_desc, frame, sample, apply_aobj_channel,
                            &sample->fobj_count, &sample->flags, &sample->end_frame,
                            &sample->obj_id);
}
