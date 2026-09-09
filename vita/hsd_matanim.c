#include "hsd_matanim.h"
#include "hsd_anim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MV_NONE UINT32_MAX
#define MV_MAX_NODES 32768u
#define MV_TEX_COORD_MASK 0x0fu
#define MV_TEX_COORD_UV 0u
#define MV_GX_MIRROR 2u

#define MV_A_T_TIMG 1u
#define MV_A_T_TRAU 2u
#define MV_A_T_TRAV 3u
#define MV_A_T_SCAU 4u
#define MV_A_T_SCAV 5u
#define MV_A_T_ROTX 6u
#define MV_A_T_ROTY 7u
#define MV_A_T_ROTZ 8u
#define MV_A_T_BLEND 9u
#define MV_A_T_TCLT 10u
#define MV_A_T_LOD_BIAS 11u
#define MV_A_T_KONST_R 12u
#define MV_A_T_TEV1_A 23u
#define MV_A_T_TS_BLEND 24u

typedef struct {
    uint32_t dobj;
    uint32_t matanim;
    uint8_t has_matanim;
} MvMatBinding;

typedef struct {
    uint32_t joint;
    uint32_t matjoint;
    uint8_t has_matjoint;
} MvMatJointWork;

typedef struct {
    float rotate[3];
    float scale[3];
    float translate[3];
    float blending;
    uint32_t image_desc;
    uint32_t tlut_desc;
    uint32_t flags;
    uint32_t wrap_t;
    uint8_t repeat_s, repeat_t;
} MvTObjState;

static float be_float(const uint8_t *p)
{
    uint32_t bits = mv_be32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int read_pointer(const MvDat *dat, uint32_t field, uint32_t *target,
                        uint8_t *present)
{
    int result = mv_dat_pointer(dat, field, target);
    if (result < 0) return -1;
    *present = result != 0;
    if (!result) *target = MV_NONE;
    return 0;
}

static int find_public_root(const MvDat *dat, const char *name, uint32_t *root)
{
    for (uint32_t i = 0; i < dat->public_count; ++i) {
        const char *public_name;
        uint32_t offset;
        if (mv_dat_public(dat, i, &public_name, &offset)) return -1;
        if (!strcmp(public_name, name)) {
            *root = offset;
            return 0;
        }
    }
    return -1;
}

static int append_binding(MvMatBinding *bindings, size_t capacity, size_t *count,
                          uint32_t dobj, uint32_t matanim, uint8_t has_matanim)
{
    if (*count >= capacity) return -1;
    bindings[*count].dobj = dobj;
    bindings[*count].matanim = has_matanim ? matanim : MV_NONE;
    bindings[*count].has_matanim = has_matanim;
    ++*count;
    return 0;
}

static const MvMatBinding *find_binding(const MvMatBinding *bindings, size_t count,
                                        uint32_t dobj)
{
    for (size_t i = 0; i < count; ++i)
        if (bindings[i].dobj == dobj) return &bindings[i];
    return NULL;
}

static int collect_bindings(const MvDat *dat, uint32_t joint_root, uint32_t mat_root,
                            MvMatBinding *bindings, size_t capacity, size_t *binding_count,
                            MvMatAnimStats *stats)
{
    MvMatJointWork *todo = malloc(MV_MAX_NODES * sizeof(*todo));
    uint8_t *visited = calloc((size_t)dat->data_size + 1, 1);
    if (!todo || !visited) {
        free(todo); free(visited);
        return -1;
    }
    size_t pending = 1, processed = 0;
    todo[0] = (MvMatJointWork){joint_root, mat_root, 1};
    *binding_count = 0;
    while (pending) {
        MvMatJointWork work = todo[--pending];
        if (work.joint > dat->data_size || visited[work.joint]) goto fail;
        const uint8_t *joint = mv_dat_span(dat, work.joint, 64);
        if (!joint || ++processed > MV_MAX_NODES) goto fail;
        visited[work.joint] = 1;
        ++stats->joint_count;

        uint32_t child, next, dobj;
        uint8_t has_child, has_next, has_dobj;
        if (read_pointer(dat, work.joint + 8, &child, &has_child) ||
            read_pointer(dat, work.joint + 12, &next, &has_next) ||
            read_pointer(dat, work.joint + 16, &dobj, &has_dobj)) goto fail;

        uint32_t mat_child = MV_NONE, mat_next = MV_NONE, matanim = MV_NONE;
        uint8_t has_mat_child = 0, has_mat_next = 0, has_matanim = 0;
        if (work.has_matjoint) {
            if (!mv_dat_span(dat, work.matjoint, 12) ||
                read_pointer(dat, work.matjoint, &mat_child, &has_mat_child) ||
                read_pointer(dat, work.matjoint + 4, &mat_next, &has_mat_next) ||
                read_pointer(dat, work.matjoint + 8, &matanim, &has_matanim)) goto fail;
        }

        if (has_next) {
            if (pending >= MV_MAX_NODES) goto fail;
            todo[pending++] = (MvMatJointWork){next, mat_next,
                (uint8_t)(work.has_matjoint && has_mat_next)};
        }
        if (has_child) {
            if (pending >= MV_MAX_NODES) goto fail;
            todo[pending++] = (MvMatJointWork){child, mat_child,
                (uint8_t)(work.has_matjoint && has_mat_child)};
        }

        uint32_t current_dobj = dobj;
        uint8_t current_has_dobj = has_dobj;
        uint32_t current_mat = matanim;
        uint8_t current_has_mat = has_matanim;
        for (unsigned guard = 0; current_has_dobj && guard < MV_MAX_NODES; ++guard) {
            if (!mv_dat_span(dat, current_dobj, 16) ||
                append_binding(bindings, capacity, binding_count, current_dobj,
                               current_mat, current_has_mat)) goto fail;
            if (current_has_mat) ++stats->matanim_count;

            uint32_t next_dobj;
            uint8_t has_next_dobj;
            if (read_pointer(dat, current_dobj + 4, &next_dobj, &has_next_dobj)) goto fail;
            current_dobj = next_dobj;
            current_has_dobj = has_next_dobj;

            if (current_has_mat) {
                uint32_t next_mat;
                uint8_t has_next_mat;
                if (!mv_dat_span(dat, current_mat, 16) ||
                    read_pointer(dat, current_mat, &next_mat, &has_next_mat)) goto fail;
                current_mat = next_mat;
                current_has_mat = has_next_mat;
            }
            if (guard + 1 == MV_MAX_NODES) goto fail;
        }
    }
    free(todo); free(visited);
    return 0;
fail:
    free(todo); free(visited);
    return -1;
}

static int read_tobj_state(const MvDat *dat, uint32_t tobj, MvTObjState *state,
                           uint32_t *id)
{
    const uint8_t *p = mv_dat_span(dat, tobj, 92);
    if (!p) return -1;
    memset(state, 0, sizeof(*state));
    *id = mv_be32(p + 8);
    for (unsigned i = 0; i < 3; ++i) {
        state->rotate[i] = be_float(p + 16 + i * 4);
        state->scale[i] = be_float(p + 28 + i * 4);
        state->translate[i] = be_float(p + 40 + i * 4);
        if (!isfinite(state->rotate[i]) || !isfinite(state->scale[i]) ||
            !isfinite(state->translate[i])) return -1;
    }
    state->wrap_t = mv_be32(p + 56);
    state->repeat_s = p[60]; state->repeat_t = p[61];
    state->flags = mv_be32(p + 64);
    state->blending = be_float(p + 68);
    if (!isfinite(state->blending) || !state->repeat_s || !state->repeat_t) return -1;
    uint8_t has_image, has_tlut;
    if (read_pointer(dat, tobj + 76, &state->image_desc, &has_image) || !has_image ||
        read_pointer(dat, tobj + 80, &state->tlut_desc, &has_tlut)) return -1;
    if (!has_tlut) state->tlut_desc = MV_NONE;
    return 0;
}

static int find_texanim(const MvDat *dat, uint32_t matanim, uint32_t id,
                        uint32_t *texanim, uint8_t *found, MvMatAnimStats *stats)
{
    *found = 0; *texanim = MV_NONE;
    if (matanim == MV_NONE) return 0;
    uint32_t current;
    uint8_t has_current;
    if (read_pointer(dat, matanim + 8, &current, &has_current)) return -1;
    uint32_t seen[256]; size_t seen_count = 0;
    while (has_current) {
        if (!mv_dat_span(dat, current, 24)) return -1;
        for (size_t i = 0; i < seen_count; ++i) if (seen[i] == current) return -1;
        if (seen_count >= 256) return -1;
        seen[seen_count++] = current;
        ++stats->texanim_count;
        if (mv_be32(mv_dat_span(dat, current + 4, 4)) == id) {
            *texanim = current; *found = 1;
            return 0;
        }
        uint32_t next; uint8_t has_next;
        if (read_pointer(dat, current, &next, &has_next)) return -1;
        current = next; has_current = has_next;
    }
    return 0;
}

static int table_pointer(const MvDat *dat, uint32_t table, uint16_t count, int index,
                         uint32_t *target, uint8_t *present)
{
    if (index < 0 || index >= (int)count) return -1;
    uint64_t field = (uint64_t)table + (uint64_t)(unsigned)index * 4u;
    if (field > UINT32_MAX) return -1;
    return read_pointer(dat, (uint32_t)field, target, present);
}

static int apply_texanim(const MvDat *dat, uint32_t texanim, float frame,
                         MvTObjState *state, MvMatAnimStats *stats)
{
    const uint8_t *ta = mv_dat_span(dat, texanim, 24);
    if (!ta) return -1;
    uint32_t aobj, imagetbl, tluttbl;
    uint8_t has_aobj, has_images, has_tluts;
    if (read_pointer(dat, texanim + 8, &aobj, &has_aobj) ||
        read_pointer(dat, texanim + 12, &imagetbl, &has_images) ||
        read_pointer(dat, texanim + 16, &tluttbl, &has_tluts)) return -1;
    uint16_t n_images = mv_be16(ta + 20), n_tluts = mv_be16(ta + 22);
    if ((n_images != 0) != (has_images != 0) || (n_tluts != 0) != (has_tluts != 0)) return -1;
    if (!has_aobj) return 0;

    MvAObjSample sample;
    int result = mv_aobj_sample(dat, aobj, frame, &sample);
    if (result) return result;
    ++stats->aobj_count;
    stats->fobj_count += sample.fobj_count;
    stats->channel_count += sample.applied_channel_count;

    for (unsigned type = 1; type <= 32; ++type) {
        if (!(sample.channel_mask & (1u << (type - 1)))) continue;
        float value = sample.channels[type - 1];
        switch (type) {
        case MV_A_T_TIMG: {
            if (!has_images || !n_images || !isfinite(value)) return -1;
            int index = (int)value;
            uint32_t image; uint8_t present;
            if (table_pointer(dat, imagetbl, n_images, index, &image, &present)) return -1;
            if (present) state->image_desc = image;
            ++stats->image_channel_count;
            break;
        }
        case MV_A_T_TRAU: state->translate[0] = value; ++stats->uv_channel_count; break;
        case MV_A_T_TRAV: state->translate[1] = value; ++stats->uv_channel_count; break;
        case MV_A_T_SCAU: state->scale[0] = value; ++stats->uv_channel_count; break;
        case MV_A_T_SCAV: state->scale[1] = value; ++stats->uv_channel_count; break;
        case MV_A_T_ROTX: state->rotate[0] = value; ++stats->uv_channel_count; break;
        case MV_A_T_ROTY: state->rotate[1] = value; ++stats->uv_channel_count; break;
        case MV_A_T_ROTZ: state->rotate[2] = value; ++stats->uv_channel_count; break;
        case MV_A_T_BLEND:
        case MV_A_T_TS_BLEND:
            state->blending = value; ++stats->blend_channel_count; break;
        case MV_A_T_TCLT: {
            if (!has_tluts || !n_tluts || !isfinite(value)) return -1;
            int index = (int)value;
            uint32_t tlut; uint8_t present;
            if (table_pointer(dat, tluttbl, n_tluts, index, &tlut, &present)) return -1;
            if (present) state->tlut_desc = tlut;
            break;
        }
        case MV_A_T_LOD_BIAS:
        case MV_A_T_KONST_R ... MV_A_T_TEV1_A:
            /* These update LOD or TEV runtime state, which the vita2d adapter
               does not yet model. Keep them explicit instead of dropping them. */
            ++stats->unsupported_count;
            return 1;
        default:
            ++stats->unsupported_count;
            return 1;
        }
    }
    return 0;
}

static void concat_mtx(const float a[12], const float b[12], float out[12])
{
    float r[12];
    for (unsigned row = 0; row < 3; ++row) {
        for (unsigned col = 0; col < 3; ++col) {
            r[row * 4 + col] = a[row * 4] * b[col] + a[row * 4 + 1] * b[4 + col] +
                               a[row * 4 + 2] * b[8 + col];
        }
        r[row * 4 + 3] = a[row * 4] * b[3] + a[row * 4 + 1] * b[7] +
                           a[row * 4 + 2] * b[11] + a[row * 4 + 3];
    }
    memcpy(out, r, sizeof(r));
}

static void rotation_mtx(float m[12], const float rotate[3])
{
    float sx = sinf(rotate[0]), cx = cosf(rotate[0]);
    float sy = sinf(rotate[1]), cy = cosf(rotate[1]);
    float sz = sinf(rotate[2]), cz = cosf(rotate[2]);
    float t1 = sx * sy, t2 = cx * sy;
    m[0] = cy * cz; m[4] = cy * sz; m[8] = -sy;
    m[1] = cz * t1 - cx * sz; m[5] = sz * t1 + cx * cz; m[9] = sx * cy;
    m[2] = cz * t2 + sx * sz; m[6] = sz * t2 - sx * cz; m[10] = cx * cy;
    m[3] = m[7] = m[11] = 0.0f;
}

static int apply_uv_state(MvScene *scene, MvSceneMesh *mesh, const MvTObjState *state)
{
    if ((state->flags & MV_TEX_COORD_MASK) != MV_TEX_COORD_UV) return 1;
    if (!isfinite(state->scale[0]) || !isfinite(state->scale[1]) ||
        fabsf(state->scale[0]) < 1.0e-12f || fabsf(state->scale[1]) < 1.0e-12f) return -1;

    float trans_y_extra = 0.0f;
    if (state->wrap_t == MV_GX_MIRROR) {
        float repeat_over_scale = (float)state->repeat_t / state->scale[1];
        if (fabsf(repeat_over_scale) < 1.0e-12f) return -1;
        trans_y_extra = 1.0f / repeat_over_scale;
    }
    float translation[12] = {
        1, 0, 0, -state->translate[0],
        0, 1, 0, -(state->translate[1] + trans_y_extra),
        0, 0, 1, state->translate[2]
    };
    float rotation[12];
    float r[3] = {state->rotate[0], state->rotate[1], -state->rotate[2]};
    rotation_mtx(rotation, r);
    float rt[12];
    concat_mtx(rotation, translation, rt);
    float sx = (float)state->repeat_s / state->scale[0];
    float sy = (float)state->repeat_t / state->scale[1];
    float scale[12] = {sx,0,0,0, 0,sy,0,0, 0,0,state->scale[2],0};
    float m[12];
    concat_mtx(scale, rt, m);

    if (mesh->first_vertex > scene->vertex_count ||
        mesh->vertex_count > scene->vertex_count - mesh->first_vertex) return -1;
    for (size_t i = 0; i < mesh->vertex_count; ++i) {
        MvSceneVertex *v = &scene->vertices[mesh->first_vertex + i];
        float u = v->u, vv = v->v;
        v->u = m[0] * u + m[1] * vv + m[3];
        v->v = m[4] * u + m[5] * vv + m[7];
        if (!isfinite(v->u) || !isfinite(v->v)) return -1;
    }
    return 0;
}

int mv_scene_apply_matanim(const MvDat *dat, const char *joint_root_name,
                           const char *matanim_root_name, float frame,
                           MvScene *scene, MvMatAnimStats *stats)
{
    if (!dat || !joint_root_name || !matanim_root_name || !scene || !stats ||
        !isfinite(frame) || !scene->meshes || !scene->mesh_count) return -1;
    memset(stats, 0, sizeof(*stats));
    uint32_t joint_root, mat_root;
    if (find_public_root(dat, joint_root_name, &joint_root) ||
        find_public_root(dat, matanim_root_name, &mat_root)) return -1;

    size_t binding_capacity = scene->dobj_count ? scene->dobj_count : scene->mesh_count;
    MvMatBinding *bindings = calloc(binding_capacity, sizeof(*bindings));
    if (!bindings) return -1;
    size_t binding_count = 0;
    if (collect_bindings(dat, joint_root, mat_root, bindings, binding_capacity,
                         &binding_count, stats)) {
        free(bindings); return -1;
    }

    for (size_t i = 0; i < scene->mesh_count; ++i) {
        MvSceneMesh *mesh = &scene->meshes[i];
        if (mesh->tobj_desc == MV_NONE) continue;
        MvTObjState state;
        uint32_t tobj_id;
        if (read_tobj_state(dat, mesh->tobj_desc, &state, &tobj_id)) {
            free(bindings); return -1;
        }
        if (mesh->texture_count > 1) ++stats->multitexture_mesh_count;

        const MvMatBinding *binding = find_binding(bindings, binding_count, mesh->dobj_desc);
        if (!binding) { free(bindings); return -1; }
        if (binding->has_matanim) {
            uint32_t texanim; uint8_t found;
            if (find_texanim(dat, binding->matanim, tobj_id, &texanim, &found, stats)) {
                free(bindings); return -1;
            }
            if (found) {
                int result = apply_texanim(dat, texanim, frame, &state, stats);
                if (result) { free(bindings); return result; }
            }
        }

        int uv_result = apply_uv_state(scene, mesh, &state);
        if (uv_result) {
            ++stats->unsupported_count;
            free(bindings);
            return uv_result < 0 ? -1 : 1;
        }
        mesh->image_desc = state.image_desc;
        mesh->tlut_desc = state.tlut_desc;
        mesh->tobj_flags = state.flags;
        mesh->texture_blending = state.blending;
        ++stats->transformed_mesh_count;
    }
    free(bindings);
    return 0;
}

int mv_scene_apply_matanim_frame0(const MvDat *dat, const char *joint_root_name,
                                  const char *matanim_root_name, MvScene *scene,
                                  MvMatAnimStats *stats)
{
    return mv_scene_apply_matanim(dat, joint_root_name, matanim_root_name, 0.0f,
                                  scene, stats);
}

int mv_matanim_frame0_stats(const MvDat *dat, const char *joint_root_name,
                            const char *anim_root_name,
                            uint32_t out[MV_MATANIM_STAT_COUNT])
{
    if (!out) return -1;
    memset(out, 0, MV_MATANIM_STAT_COUNT * sizeof(*out));
    MvScene scene;
    if (mv_scene_build_frame0(dat, joint_root_name, "MenMainBack_Top_animjoint", &scene))
        return -1;
    MvMatAnimStats stats;
    int result = mv_scene_apply_matanim_frame0(dat, joint_root_name, anim_root_name,
                                               &scene, &stats);
    if (!result) {
        out[MV_MATANIM_STAT_JOINTS] = (uint32_t)stats.joint_count;
        out[MV_MATANIM_STAT_MATANIMS] = (uint32_t)stats.matanim_count;
        out[MV_MATANIM_STAT_TEXANIMS] = (uint32_t)stats.texanim_count;
        out[MV_MATANIM_STAT_AOBJS] = (uint32_t)stats.aobj_count;
        out[MV_MATANIM_STAT_FOBJS] = (uint32_t)stats.fobj_count;
        out[MV_MATANIM_STAT_CHANNELS] = (uint32_t)stats.channel_count;
        out[MV_MATANIM_STAT_IMAGES] = (uint32_t)stats.image_channel_count;
        out[MV_MATANIM_STAT_UV_CHANNELS] = (uint32_t)stats.uv_channel_count;
        out[MV_MATANIM_STAT_BLEND_CHANNELS] = (uint32_t)stats.blend_channel_count;
        out[MV_MATANIM_STAT_TRANSFORMED_MESHES] = (uint32_t)stats.transformed_mesh_count;
        out[MV_MATANIM_STAT_MULTITEXTURE_MESHES] = (uint32_t)stats.multitexture_mesh_count;
        out[MV_MATANIM_STAT_UNSUPPORTED] = (uint32_t)stats.unsupported_count;
    }
    mv_scene_free(&scene);
    return result;
}
