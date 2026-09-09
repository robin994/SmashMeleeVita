#include "asset_viewer.h"
#include "asset_file.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MV_SCENE_TEXTURE_BUDGET (24u * 1024u * 1024u)

static const MvImage *find_image(const MvViewer *v, uint32_t image_desc, uint32_t tlut_desc)
{
    for (size_t i = 0; i < v->count; ++i)
        if (v->images[i].image_desc == image_desc && v->images[i].tlut_desc == tlut_desc)
            return &v->images[i];
    return NULL;
}

static vita2d_texture *create_texture(const MvViewer *v, const MvImage *im)
{
    vita2d_texture *t = vita2d_create_empty_texture_format(im->width, im->height,
                                                          SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!t) return NULL;
    size_t stride = vita2d_texture_get_stride(t);
    uint8_t *pixels = vita2d_texture_get_datap(t);
    if (!pixels || mv_image_decode(&v->dat, im, pixels, stride * im->height, stride)) {
        vita2d_free_texture(t); return NULL;
    }
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t;
}

static void release_page(MvViewer *v)
{
    vita2d_wait_rendering_done();
    for (unsigned i = 0; i < MV_PAGE_SIZE; ++i) {
        if (v->textures[i]) vita2d_free_texture(v->textures[i]);
        v->textures[i] = NULL;
    }
}
static void load_page(MvViewer *v)
{
    release_page(v);
    for (unsigned i = 0; i < MV_PAGE_SIZE; ++i) {
        size_t index = v->page * MV_PAGE_SIZE + i;
        if (index >= v->count) break;
        const MvImage *im = &v->images[index];
        vita2d_texture *t = create_texture(v, im);
        if (!t) {
            if (v->log) fprintf(v->log, "TEXTURE_READY_FAIL index=%u\n", (unsigned)index);
            continue;
        }
        v->textures[i] = t;
        if (v->log) fprintf(v->log, "TEXTURE_READY index=%u image=%lx size=%lux%lu fmt=%lu\n",
                            (unsigned)index, (unsigned long)im->image_desc,
                            (unsigned long)im->width, (unsigned long)im->height, (unsigned long)im->format);
    }
    if (v->log) fflush(v->log);
}

static vita2d_texture *scene_texture_for(const MvViewer *v, uint32_t image_desc, uint32_t tlut_desc)
{
    for (size_t i = 0; i < v->scene_texture_count; ++i)
        if (v->scene_textures[i].image_desc == image_desc &&
            v->scene_textures[i].tlut_desc == tlut_desc) return v->scene_textures[i].texture;
    return NULL;
}

static void load_scene_textures(MvViewer *v)
{
    if (!v->scene_ready) return;
    for (size_t i = 0; i < v->scene.mesh_count; ++i) {
        const MvSceneMesh *mesh = &v->scene.meshes[i];
        if (mesh->image_desc == UINT32_MAX ||
            scene_texture_for(v, mesh->image_desc, mesh->tlut_desc)) continue;
        if (v->scene_texture_count >= MV_SCENE_TEXTURE_LIMIT) {
            if (v->log) fprintf(v->log, "SCENE_TEXTURE_LIMIT meshes=%u\n", (unsigned)v->scene.mesh_count);
            break;
        }
        MvImage dynamic_image;
        const MvImage *im = find_image(v, mesh->image_desc, mesh->tlut_desc);
        if (!im) {
            if (mv_image_from_desc(&v->dat, mesh->image_desc, mesh->tlut_desc, &dynamic_image)) {
                if (v->log) fprintf(v->log, "SCENE_TEXTURE_DESCRIPTOR_MISSING image=%lx tlut=%lx\n",
                                    (unsigned long)mesh->image_desc, (unsigned long)mesh->tlut_desc);
                continue;
            }
            im = &dynamic_image;
            if (v->log) fprintf(v->log, "SCENE_TEXTURE_DYNAMIC image=%lx tlut=%lx size=%lux%lu fmt=%lu\n",
                                (unsigned long)im->image_desc, (unsigned long)im->tlut_desc,
                                (unsigned long)im->width, (unsigned long)im->height,
                                (unsigned long)im->format);
        }
        uint64_t estimate = (uint64_t)im->width * im->height * 4;
        if (estimate > MV_SCENE_TEXTURE_BUDGET - v->scene_texture_bytes) {
            if (v->log) fprintf(v->log, "SCENE_TEXTURE_BUDGET image=%lx bytes=%lu used=%u\n",
                                (unsigned long)mesh->image_desc, (unsigned long)estimate,
                                (unsigned)v->scene_texture_bytes);
            continue;
        }
        vita2d_texture *texture = create_texture(v, im);
        if (!texture) {
            if (v->log) fprintf(v->log, "SCENE_TEXTURE_FAIL image=%lx fmt=%lu size=%lux%lu\n",
                                (unsigned long)mesh->image_desc, (unsigned long)im->format,
                                (unsigned long)im->width, (unsigned long)im->height);
            continue;
        }
        MvGpuImage *gpu = &v->scene_textures[v->scene_texture_count++];
        gpu->image_desc = mesh->image_desc; gpu->tlut_desc = mesh->tlut_desc; gpu->texture = texture;
        v->scene_texture_bytes += (size_t)estimate;
    }
    if (v->log) {
        fprintf(v->log, "HSD_GEOMETRY_TEXTURES ready=%u bytes=%u limit=%u\n",
                (unsigned)v->scene_texture_count, (unsigned)v->scene_texture_bytes,
                (unsigned)MV_SCENE_TEXTURE_BUDGET);
        fflush(v->log);
    }
}

static unsigned vita_color(uint32_t rgba)
{
    return RGBA8(rgba >> 24, rgba >> 16, rgba >> 8, rgba);
}

typedef struct {
    float x, y, depth, u, v;
    uint32_t rgba;
} MvCameraVertex;

typedef struct {
    float right[3], up[3], forward[3];
} MvCameraBasis;

static int normalize_vec3(float v[3])
{
    float length2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!isfinite(length2) || length2 < 1.0e-12f) return -1;
    float inv = 1.0f / sqrtf(length2);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
    return 0;
}

static void cross_vec3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int camera_basis(const MvCamera *camera, MvCameraBasis *basis)
{
    for (unsigned i = 0; i < 3; ++i)
        basis->forward[i] = camera->interest[i] - camera->eye[i];
    if (normalize_vec3(basis->forward)) return -1;
    cross_vec3(basis->forward, camera->up, basis->right);
    if (normalize_vec3(basis->right)) return -1;
    cross_vec3(basis->right, basis->forward, basis->up);
    return normalize_vec3(basis->up);
}

static MvCameraVertex camera_vertex(const MvCamera *camera, const MvCameraBasis *basis,
                                    const MvSceneVertex *source)
{
    float delta[3] = {source->x - camera->eye[0], source->y - camera->eye[1],
                      source->z - camera->eye[2]};
    MvCameraVertex out;
    out.x = delta[0] * basis->right[0] + delta[1] * basis->right[1] + delta[2] * basis->right[2];
    out.y = delta[0] * basis->up[0] + delta[1] * basis->up[1] + delta[2] * basis->up[2];
    out.depth = delta[0] * basis->forward[0] + delta[1] * basis->forward[1] + delta[2] * basis->forward[2];
    out.u = source->u; out.v = source->v; out.rgba = source->rgba;
    return out;
}

static uint32_t lerp_rgba(uint32_t a, uint32_t b, float t)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 24; shift += 8) {
        float av = (float)((a >> shift) & 0xff), bv = (float)((b >> shift) & 0xff);
        unsigned value = (unsigned)(av + (bv - av) * t + 0.5f);
        if (value > 255) value = 255;
        result |= value << shift;
    }
    return result;
}

static MvCameraVertex lerp_camera_vertex(const MvCameraVertex *a, const MvCameraVertex *b, float t)
{
    MvCameraVertex out;
    out.x = a->x + (b->x - a->x) * t; out.y = a->y + (b->y - a->y) * t;
    out.depth = a->depth + (b->depth - a->depth) * t;
    out.u = a->u + (b->u - a->u) * t; out.v = a->v + (b->v - a->v) * t;
    out.rgba = lerp_rgba(a->rgba, b->rgba, t);
    return out;
}

static size_t clip_depth(const MvCameraVertex *input, size_t count, MvCameraVertex *output,
                         float plane, int keep_greater)
{
    if (!count) return 0;
    size_t out_count = 0;
    MvCameraVertex previous = input[count - 1];
    int previous_inside = keep_greater ? previous.depth >= plane : previous.depth <= plane;
    for (size_t i = 0; i < count; ++i) {
        MvCameraVertex current = input[i];
        int current_inside = keep_greater ? current.depth >= plane : current.depth <= plane;
        if (current_inside != previous_inside) {
            float denominator = current.depth - previous.depth;
            if (fabsf(denominator) > 1.0e-12f) {
                float t = (plane - previous.depth) / denominator;
                output[out_count] = lerp_camera_vertex(&previous, &current, t);
                output[out_count++].depth = plane;
            }
        }
        if (current_inside) output[out_count++] = current;
        previous = current; previous_inside = current_inside;
    }
    return out_count;
}

static void scene_viewport(const MvCamera *camera, float *x, float *y, float *w, float *h)
{
    const float box_x = 35.0f, box_y = 88.0f, box_w = 890.0f, box_h = 390.0f;
    float aspect = camera->aspect;
    if (camera->projection_type != 1 || aspect <= 0.0f) {
        int width = camera->viewport_xmax - camera->viewport_xmin;
        int height = camera->viewport_ymax - camera->viewport_ymin;
        aspect = height > 0 ? (float)width / (float)height : 4.0f / 3.0f;
    }
    *w = box_w; *h = *w / aspect;
    if (*h > box_h) { *h = box_h; *w = *h * aspect; }
    *x = box_x + (box_w - *w) * 0.5f; *y = box_y + (box_h - *h) * 0.5f;
}

static int project_vertex(const MvCamera *camera, const MvCameraVertex *source,
                          float view_x, float view_y, float view_w, float view_h,
                          float *x, float *y)
{
    if (source->depth < camera->near_z || source->depth > camera->far_z) return -1;
    float nx, ny;
    if (camera->projection_type == 1) {
        float tangent = tanf(camera->fov_y * (3.14159265358979323846f / 360.0f));
        if (!isfinite(tangent) || tangent <= 0.0f) return -1;
        nx = source->x / (source->depth * tangent * camera->aspect);
        ny = source->y / (source->depth * tangent);
    } else if (camera->projection_type == 2) {
        float px = camera->near_z * source->x / source->depth;
        float py = camera->near_z * source->y / source->depth;
        nx = (2.0f * px - camera->right - camera->left) / (camera->right - camera->left);
        ny = (2.0f * py - camera->top - camera->bottom) / (camera->top - camera->bottom);
    } else {
        nx = (2.0f * source->x - camera->right - camera->left) / (camera->right - camera->left);
        ny = (2.0f * source->y - camera->top - camera->bottom) / (camera->top - camera->bottom);
    }
    if (!isfinite(nx) || !isfinite(ny)) return -1;
    *x = view_x + (nx * 0.5f + 0.5f) * view_w;
    *y = view_y + (0.5f - ny * 0.5f) * view_h;
    return 0;
}

static void camera_visibility(const MvViewer *v, size_t *front, size_t *partial,
                              size_t *behind, size_t *far_count)
{
    *front = *partial = *behind = *far_count = 0;
    MvCameraBasis basis;
    if (!v->camera_ready || camera_basis(&v->camera, &basis)) return;
    for (size_t m = 0; m < v->scene.mesh_count; ++m) {
        const MvSceneMesh *mesh = &v->scene.meshes[m];
        const MvSceneVertex *source = &v->scene.vertices[mesh->first_vertex];
        for (size_t i = 0; i + 2 < mesh->vertex_count; i += 3) {
            unsigned inside = 0, too_near = 0, too_far = 0;
            for (unsigned j = 0; j < 3; ++j) {
                MvCameraVertex cv = camera_vertex(&v->camera, &basis, &source[i + j]);
                if (cv.depth < v->camera.near_z) ++too_near;
                else if (cv.depth > v->camera.far_z) ++too_far;
                else ++inside;
            }
            if (inside == 3) ++*front;
            else if (too_near == 3) ++*behind;
            else if (too_far == 3) ++*far_count;
            else ++*partial;
        }
    }
}

static void draw_scene(const MvViewer *v, vita2d_pgf *font)
{
    const unsigned white = RGBA8(235, 240, 245, 255);
    vita2d_pgf_draw_text(font, 28, 38, white, 1.3f, "Melee Vita v2.1 - materialized comparison");
    vita2d_pgf_draw_textf(font, 28, 70, white, 0.82f,
        "%u meshes | %u triangles | %u/%u textures | upstream GX capture: %s",
        (unsigned)v->scene.mesh_count, (unsigned)(v->scene.vertex_count / 3),
        (unsigned)v->scene_texture_count, (unsigned)v->scene.textured_mesh_count,
        v->gx_capture_ready ? "PASS" : "FAIL");
    vita2d_draw_rectangle(35, 88, 890, 390, RGBA8(8, 12, 18, 255));
    if (!v->camera_ready) {
        vita2d_pgf_draw_text(font, 56, 140, white, 0.9f, "Original camera descriptor unavailable.");
        return;
    }
    MvCameraBasis basis;
    if (camera_basis(&v->camera, &basis)) return;
    float view_x, view_y, view_w, view_h;
    scene_viewport(&v->camera, &view_x, &view_y, &view_w, &view_h);
    vita2d_draw_rectangle(view_x, view_y, view_w, view_h, RGBA8(4, 7, 11, 255));
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle((int)view_x, (int)view_y, (int)(view_x + view_w), (int)(view_y + view_h));
    for (size_t i = 0; i < v->scene.mesh_count; ++i) {
        const MvSceneMesh *mesh = &v->scene.meshes[i];
        const MvSceneVertex *source = &v->scene.vertices[mesh->first_vertex];
        vita2d_texture *texture = mesh->image_desc == UINT32_MAX ? NULL :
            scene_texture_for(v, mesh->image_desc, mesh->tlut_desc);
        if (texture) {
            size_t capacity = mesh->vertex_count * 3;
            vita2d_texture_vertex *out = vita2d_pool_malloc((unsigned)(capacity * sizeof(*out)));
            if (!out) continue;
            size_t out_count = 0;
            for (size_t j = 0; j + 2 < mesh->vertex_count; j += 3) {
                MvCameraVertex tri[3], near_clip[6], far_clip[8];
                for (unsigned k = 0; k < 3; ++k)
                    tri[k] = camera_vertex(&v->camera, &basis, &source[j + k]);
                size_t clipped = clip_depth(tri, 3, near_clip, v->camera.near_z, 1);
                clipped = clip_depth(near_clip, clipped, far_clip, v->camera.far_z, 0);
                for (size_t k = 1; k + 1 < clipped; ++k) {
                    const MvCameraVertex *fan[3] = {&far_clip[0], &far_clip[k], &far_clip[k + 1]};
                    if (out_count + 3 > capacity) break;
                    int valid = 1;
                    for (unsigned n = 0; n < 3; ++n) {
                        vita2d_texture_vertex *dst = &out[out_count + n];
                        if (project_vertex(&v->camera, fan[n], view_x, view_y, view_w, view_h,
                                           &dst->x, &dst->y)) { valid = 0; break; }
                        dst->z = 0.5f; dst->u = fan[n]->u; dst->v = fan[n]->v;
                    }
                    if (valid) out_count += 3;
                }
            }
            if (out_count)
                vita2d_draw_array_textured(texture, SCE_GXM_PRIMITIVE_TRIANGLES, out,
                                           out_count, vita_color(mesh->material_rgba));
        } else {
            size_t capacity = mesh->vertex_count * 3;
            vita2d_color_vertex *out = vita2d_pool_malloc((unsigned)(capacity * sizeof(*out)));
            if (!out) continue;
            size_t out_count = 0;
            for (size_t j = 0; j + 2 < mesh->vertex_count; j += 3) {
                MvCameraVertex tri[3], near_clip[6], far_clip[8];
                for (unsigned k = 0; k < 3; ++k)
                    tri[k] = camera_vertex(&v->camera, &basis, &source[j + k]);
                size_t clipped = clip_depth(tri, 3, near_clip, v->camera.near_z, 1);
                clipped = clip_depth(near_clip, clipped, far_clip, v->camera.far_z, 0);
                for (size_t k = 1; k + 1 < clipped; ++k) {
                    const MvCameraVertex *fan[3] = {&far_clip[0], &far_clip[k], &far_clip[k + 1]};
                    if (out_count + 3 > capacity) break;
                    int valid = 1;
                    for (unsigned n = 0; n < 3; ++n) {
                        vita2d_color_vertex *dst = &out[out_count + n];
                        if (project_vertex(&v->camera, fan[n], view_x, view_y, view_w, view_h,
                                           &dst->x, &dst->y)) { valid = 0; break; }
                        dst->z = 0.5f; dst->color = vita_color(fan[n]->rgba);
                    }
                    if (valid) out_count += 3;
                }
            }
            if (out_count) vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, out, out_count);
        }
    }
    vita2d_disable_clipping();
    vita2d_pgf_draw_text(font, 28, 515, white, 0.82f,
        "Square: next renderer   Triangle: diagnostics   SELECT+START: exit   |   PS+START screenshot safe");
}

static void draw_gx_replay(MvViewer *v, vita2d_pgf *font)
{
    const unsigned white = RGBA8(235, 240, 245, 255);
    vita2d_pgf_draw_text(font, 28, 38, white, 1.3f, "Melee Vita v2.5 - GX PE blend + culling");
    vita2d_pgf_draw_textf(font, 28, 70, white, 0.82f,
        "%u/%u commands | %u/%u triangles | %u textures | GX replay: %s",
        (unsigned)v->gx_replay_stats.supported_commands,
        (unsigned)v->gx_capture_stats.commands,
        (unsigned)v->gx_replay_stats.supported_triangles,
        (unsigned)v->gx_capture_stats.triangles,
        (unsigned)v->gx_replay.texture_count,
        v->gx_replay.ready ? "PASS" : "FAIL");
    vita2d_draw_rectangle(35, 88, 890, 390, RGBA8(8, 12, 18, 255));
    if (v->camera_ready && v->gx_replay.ready) {
        mv_gx_replay_draw(&v->gx_replay, &v->camera);
    } else {
        vita2d_pgf_draw_text(font, 56, 140, white, 0.9f,
                            "GX replay or original camera unavailable.");
    }
    vita2d_pgf_draw_text(font, 28, 515, white, 0.82f,
        "Square: comparison/textures   HSD PE blend/cull active   PS+START screenshot safe");
}
int mv_viewer_init(MvViewer *v, FILE *log)
{
    memset(v, 0, sizeof(*v)); v->log = log;
    size_t size;
    const char *path = "ux0:data/SmashMeleeVita/files/MnMaAll.usd";
    if (mv_read_file(path, &v->bytes, &size)) {
        v->error = "Copy MnMaAll.usd into ux0:data/SmashMeleeVita/files/";
        if (log) { fprintf(log, "ASSET_FILE_MISSING path=%s\n", path); fflush(log); }
        return -1;
    }
    if (mv_dat_open(&v->dat, v->bytes, size)) {
        v->error = "Invalid HSD archive. See runtime.log.";
        if (log) { fprintf(log, "HSD_ARCHIVE_FAIL\n"); fflush(log); }
        return -1;
    }
    v->images = calloc(2048, sizeof(*v->images));
    if (!v->images || mv_menu_textures(&v->dat, v->images, 2048, &v->count) || !v->count) {
        v->error = "Unsupported menu data. See runtime.log.";
        if (log) { fprintf(log, "HSD_SCENE_FAIL\n"); fflush(log); }
        return -1;
    }
    if (log) {
        fprintf(log, "HSD_ARCHIVE_PASS bytes=%u relocations=%lu roots=%lu textures=%u\n",
                (unsigned)size, (unsigned long)v->dat.relocation_count,
                (unsigned long)v->dat.public_count, (unsigned)v->count);
        fflush(log);
    }
    if (!mv_scene_build_animated(&v->dat, "MenMainBack_Top_joint",
                                 "MenMainBack_Top_animjoint", 0.0f, &v->scene)) {
        int matanim_result = mv_scene_apply_matanim_frame0(
            &v->dat, "MenMainBack_Top_joint", "MenMainBack_Top_matanim_joint",
            &v->scene, &v->matanim_stats);
        if (!matanim_result) { v->scene_ready = 1; v->scene_mode = 1; }
        if (log) {
            fprintf(log, "HSD_ANIMJOINT_PASS root=MenMainBack_Top_animjoint frame=0 anim_joints=%u "
                         "aobjs=%u fobjs=%u channels=%u unsupported=%u\n",
                    (unsigned)v->scene.anim_joint_count, (unsigned)v->scene.anim_aobj_count,
                    (unsigned)v->scene.anim_fobj_count, (unsigned)v->scene.anim_channel_count,
                    (unsigned)v->scene.unsupported_anim_count);
            if (!matanim_result) {
                fprintf(log, "HSD_MATANIM_PASS root=MenMainBack_Top_matanim_joint frame=0 "
                             "joints=%u matanims=%u texanims=%u aobjs=%u fobjs=%u channels=%u "
                             "images=%u uv=%u blend=%u transformed=%u multitex=%u unsupported=%u\n",
                        (unsigned)v->matanim_stats.joint_count,
                        (unsigned)v->matanim_stats.matanim_count,
                        (unsigned)v->matanim_stats.texanim_count,
                        (unsigned)v->matanim_stats.aobj_count,
                        (unsigned)v->matanim_stats.fobj_count,
                        (unsigned)v->matanim_stats.channel_count,
                        (unsigned)v->matanim_stats.image_channel_count,
                        (unsigned)v->matanim_stats.uv_channel_count,
                        (unsigned)v->matanim_stats.blend_channel_count,
                        (unsigned)v->matanim_stats.transformed_mesh_count,
                        (unsigned)v->matanim_stats.multitexture_mesh_count,
                        (unsigned)v->matanim_stats.unsupported_count);
            } else {
                fprintf(log, "HSD_MATANIM_FAIL root=MenMainBack_Top_matanim_joint frame=0 result=%d\n",
                        matanim_result);
            }
            fprintf(log, "%s root=MenMainBack_Top_joint joints=%u dobjs=%u pobjs=%u "
                         "meshes=%u triangles=%u textured=%u skipped_pobj=%u skipped_prim=%u unsupported_joint=%u "
                         "bounds=%g,%g,%g..%g,%g,%g\n",
                    !matanim_result ? "HSD_MATERIALIZED_GEOMETRY_PASS" : "HSD_ANIMATED_GEOMETRY_PASS",
                    (unsigned)v->scene.joint_count, (unsigned)v->scene.dobj_count,
                    (unsigned)v->scene.pobj_count, (unsigned)v->scene.mesh_count,
                    (unsigned)(v->scene.vertex_count / 3), (unsigned)v->scene.textured_mesh_count,
                    (unsigned)v->scene.skipped_pobj_count, (unsigned)v->scene.skipped_primitive_count,
                    (unsigned)v->scene.unsupported_joint_count, v->scene.min_x, v->scene.min_y,
                    v->scene.min_z, v->scene.max_x, v->scene.max_y, v->scene.max_z);
            fflush(log);
        }
    } else if (log) {
        fprintf(log, "HSD_ANIMATED_GEOMETRY_FAIL root=MenMainBack_Top_joint anim=MenMainBack_Top_animjoint frame=0\n");
        fflush(log);
    }
    if (!mv_camera_read(&v->dat, "ScMenMain_cam_int1_camera", &v->camera)) {
        v->camera_ready = 1;
        if (log) {
            fprintf(log, "HSD_CAMERA_PASS root=ScMenMain_cam_int1_camera type=%u viewport=%d,%d,%d,%d "
                         "eye=%g,%g,%g interest=%g,%g,%g up=%g,%g,%g near=%g far=%g fov=%g aspect=%g\n",
                    (unsigned)v->camera.projection_type, v->camera.viewport_xmin, v->camera.viewport_xmax,
                    v->camera.viewport_ymin, v->camera.viewport_ymax,
                    v->camera.eye[0], v->camera.eye[1], v->camera.eye[2],
                    v->camera.interest[0], v->camera.interest[1], v->camera.interest[2],
                    v->camera.up[0], v->camera.up[1], v->camera.up[2], v->camera.near_z,
                    v->camera.far_z, v->camera.fov_y, v->camera.aspect);
            if (v->scene_ready) {
                size_t front, partial, behind, far_count;
                camera_visibility(v, &front, &partial, &behind, &far_count);
                fprintf(log, "HSD_CAMERA_VISIBILITY front=%u partial=%u behind=%u far=%u total=%u\n",
                        (unsigned)front, (unsigned)partial, (unsigned)behind, (unsigned)far_count,
                        (unsigned)(v->scene.vertex_count / 3));
            }
            fflush(log);
        }
    } else if (log) {
        fprintf(log, "HSD_CAMERA_FAIL root=ScMenMain_cam_int1_camera\n"); fflush(log);
    }
    int native_result = mv_hsd_native_build(&v->dat, "MenMainBack_Top_joint", &v->native);
    if (!native_result) {
        v->native_ready = 1;
        if (log) {
            fprintf(log, "HSD_NATIVE_DESC_PASS root=MenMainBack_Top_joint joints=%u dobjs=%u mobjs=%u "
                         "pobjs=%u tobjs=%u vtxdescs=%u images=%u tluts=%u materials=%u pedescs=%u "
                         "lods=%u tevs=%u unsupported=%u\n",
                    (unsigned)v->native.joint_count, (unsigned)v->native.dobj_count,
                    (unsigned)v->native.mobj_count, (unsigned)v->native.pobj_count,
                    (unsigned)v->native.tobj_count, (unsigned)v->native.vtxdesc_count,
                    (unsigned)v->native.image_count, (unsigned)v->native.tlut_count,
                    (unsigned)v->native.material_count, (unsigned)v->native.pedesc_count,
                    (unsigned)v->native.lod_count, (unsigned)v->native.tev_count,
                    (unsigned)v->native.unsupported_count);
            fflush(log);
        }
        int runtime_result = mv_hsd_runtime_probe(&v->native, &v->runtime_stats);
        if (!runtime_result) {
            v->runtime_ready = 1;
            if (log) {
                fprintf(log, "HSD_RUNTIME_JOBJ_PASS root=MenMainBack_Top_joint joints=%u dobjs=%u "
                             "mobjs=%u pobjs=%u tobjs=%u constructor=HSD_JObjLoadJoint\n",
                        (unsigned)v->runtime_stats.joints, (unsigned)v->runtime_stats.dobjs,
                        (unsigned)v->runtime_stats.mobjs, (unsigned)v->runtime_stats.pobjs,
                        (unsigned)v->runtime_stats.tobjs);
                fflush(log);
            }
            int capture_result = mv_hsd_gx_capture_probe(&v->native, &v->gx_capture_stats);
            if (!capture_result) {
                v->gx_capture_ready = 1;
                if (log) {
                    fprintf(log,
                            "HSD_GX_CAPTURE_PASS source=HSD_PObjCaptureRigid/GXCallDisplayList "
                            "display_lists=%u commands=%u vertices=%u triangles=%u linepoint=%u "
                            "arrays=%u formats=%u pos_mtx=%u nrm_mtx=%u tex_mtx=%u cull=%u "
                            "errors=%u backend=VitaCommandBuffer stop=before_MObj_TObj_TEV_submit\n",
                            (unsigned)v->gx_capture_stats.display_lists,
                            (unsigned)v->gx_capture_stats.commands,
                            (unsigned)v->gx_capture_stats.vertices,
                            (unsigned)v->gx_capture_stats.triangles,
                            (unsigned)v->gx_capture_stats.line_point_commands,
                            (unsigned)v->gx_capture_stats.array_binds,
                            (unsigned)v->gx_capture_stats.attr_formats,
                            (unsigned)v->gx_capture_stats.pos_mtx_loads,
                            (unsigned)v->gx_capture_stats.nrm_mtx_loads,
                            (unsigned)v->gx_capture_stats.tex_mtx_loads,
                            (unsigned)v->gx_capture_stats.cull_changes,
                            (unsigned)v->gx_capture_stats.errors);
                    fflush(log);
                }
                int replay_class_result = mv_gx_capture_replay_classify(&v->gx_replay_stats);
                if (!replay_class_result) {
                    v->gx_replay_class_ready = 1;
                    if (log) {
                        fprintf(log,
                                "HSD_GX_REPLAY_CLASS_PASS supported_commands=%u supported_triangles=%u "
                                "textured=%u untextured=%u skipped_multitex=%u skipped_tev=%u "
                                "skipped_texcoord=%u skipped_mapmode=%u skipped_vtxcolor=%u "
                                "backend=Vita2DSubsetPlan\n",
                                (unsigned)v->gx_replay_stats.supported_commands,
                                (unsigned)v->gx_replay_stats.supported_triangles,
                                (unsigned)v->gx_replay_stats.textured_commands,
                                (unsigned)v->gx_replay_stats.untextured_commands,
                                (unsigned)v->gx_replay_stats.skipped_multitex,
                                (unsigned)v->gx_replay_stats.skipped_custom_tev,
                                (unsigned)v->gx_replay_stats.skipped_texcoord,
                                (unsigned)v->gx_replay_stats.skipped_mapmode,
                                (unsigned)v->gx_replay_stats.skipped_vertex_color);
                        MvGxPeStats pe_stats;
                        if (!mv_gx_capture_pe_stats(&pe_stats)) {
                            fprintf(log,
                                    "HSD_GX_PE_CAPTURE_PASS commands=%u blend_alpha=%u blend_additive=%u "
                                    "blend_other=%u z_lequal=%u z_write=%u alpha_always=%u custom_pe=%u "
                                    "cull_none=%u cull_front=%u cull_back=%u cull_all=%u\n",
                                    pe_stats.commands, pe_stats.blend_alpha,
                                    pe_stats.blend_additive, pe_stats.blend_other,
                                    pe_stats.z_lequal, pe_stats.z_write,
                                    pe_stats.alpha_compare_always, pe_stats.custom_pe,
                                    pe_stats.cull_none, pe_stats.cull_front,
                                    pe_stats.cull_back, pe_stats.cull_all);
                        } else {
                            fprintf(log, "HSD_GX_PE_CAPTURE_FAIL\n");
                        }
                        if (v->gx_replay_stats.skipped_custom_tev) {
                            uint32_t tev_records[10u * 9u];
                            uint32_t tev_count = mv_gx_capture_custom_tev_records(tev_records, 10);
                            for (uint32_t i = 0; i < tev_count; ++i) {
                                const uint32_t *r = &tev_records[i * 9u];
                                fprintf(log,
                                        "HSD_GX_CUSTOM_TEV command=%u payload="
                                        "%08x,%08x,%08x,%08x,%08x,%08x,%08x active=%08x\n",
                                        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
                            }
                        }
                        if (v->gx_replay_stats.skipped_multitex) {
                            uint32_t multitex_records[4u * 28u];
                            uint32_t multitex_count =
                                mv_gx_capture_multitex_records(multitex_records, 4);
                            for (uint32_t i = 0; i < multitex_count; ++i) {
                                const uint32_t *r = &multitex_records[i * 28u];
                                fprintf(log,
                                        "HSD_GX_MULTITEX command=%u count=%u rendermode=%08x "
                                        "l0_flags=%08x l0_image=%08x l0_size=%08x l0_fmtwrap=%08x "
                                        "l0_blend=%08x l0_uv=%08x,%08x,%08x,%08x,%08x,%08x l0_active=%08x "
                                        "l1_flags=%08x l1_image=%08x l1_size=%08x l1_fmtwrap=%08x "
                                        "l1_blend=%08x l1_uv=%08x,%08x,%08x,%08x,%08x,%08x l1_active=%08x "
                                        "valid=%08x\n",
                                        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                                        r[8], r[9], r[10], r[11], r[12], r[13], r[14],
                                        r[15], r[16], r[17], r[18], r[19],
                                        r[20], r[21], r[22], r[23], r[24], r[25], r[26], r[27]);
                            }
                        }
                        fflush(log);
                    }
                    if (v->camera_ready && !mv_gx_replay_init(&v->gx_replay, &v->camera, log)) {
                        v->gx_replay_mode = 1;
                        v->scene_mode = 1;
                    }
                    if (log && v->scene_ready) {
                        float bounds[6];
                        int bounds_result = mv_gx_capture_world_bounds(bounds);
                        if (!bounds_result) {
                            const float epsilon = 0.01f;
                            int parity = fabsf(bounds[0] - v->scene.min_x) <= epsilon &&
                                         fabsf(bounds[1] - v->scene.min_y) <= epsilon &&
                                         fabsf(bounds[2] - v->scene.min_z) <= epsilon &&
                                         fabsf(bounds[3] - v->scene.max_x) <= epsilon &&
                                         fabsf(bounds[4] - v->scene.max_y) <= epsilon &&
                                         fabsf(bounds[5] - v->scene.max_z) <= epsilon;
                            fprintf(log,
                                    "HSD_GX_WORLD_BOUNDS_%s captured=%.6g,%.6g,%.6g..%.6g,%.6g,%.6g "
                                    "reference=%.6g,%.6g,%.6g..%.6g,%.6g,%.6g epsilon=%.3g\n",
                                    parity ? "PASS" : "FAIL",
                                    bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5],
                                    v->scene.min_x, v->scene.min_y, v->scene.min_z,
                                    v->scene.max_x, v->scene.max_y, v->scene.max_z, epsilon);
                        } else {
                            fprintf(log, "HSD_GX_WORLD_BOUNDS_FAIL result=%d\n", bounds_result);
                        }
                        fflush(log);
                    }
                } else if (log) {
                    fprintf(log, "HSD_GX_REPLAY_CLASS_FAIL result=%d\n", replay_class_result);
                    fflush(log);
                }
            } else if (log) {
                fprintf(log,
                        "HSD_GX_CAPTURE_FAIL result=%d display_lists=%u commands=%u vertices=%u "
                        "triangles=%u errors=%u stop=before_MObj_TObj_TEV_submit\n",
                        capture_result, (unsigned)v->gx_capture_stats.display_lists,
                        (unsigned)v->gx_capture_stats.commands,
                        (unsigned)v->gx_capture_stats.vertices,
                        (unsigned)v->gx_capture_stats.triangles,
                        (unsigned)v->gx_capture_stats.errors);
                fflush(log);
            }
        } else if (log) {
            fprintf(log, "HSD_RUNTIME_JOBJ_FAIL root=MenMainBack_Top_joint result=%d\n",
                    runtime_result);
            fflush(log);
        }
    } else if (log) {
        fprintf(log, "HSD_NATIVE_DESC_FAIL root=MenMainBack_Top_joint result=%d\n", native_result);
        fflush(log);
    }
    load_page(v);
    load_scene_textures(v);
    return 0;
}
void mv_viewer_page(MvViewer *v, int direction)
{
    if (v->error || !v->count) return;
    size_t pages = (v->count + MV_PAGE_SIZE - 1) / MV_PAGE_SIZE;
    if (direction > 0) v->page = (v->page + 1) % pages;
    else v->page = (v->page + pages - 1) % pages;
    load_page(v);
}
void mv_viewer_toggle_scene(MvViewer *v)
{
    if (!v->scene_ready) return;
    if (v->gx_replay_mode) {
        v->gx_replay_mode = 0;
        v->scene_mode = 1;
    } else if (v->scene_mode) {
        v->scene_mode = 0;
    } else {
        v->scene_mode = 1;
        v->gx_replay_mode = v->gx_replay.ready != 0;
    }
}
void mv_viewer_draw(MvViewer *v, vita2d_pgf *font)
{
    unsigned white = RGBA8(235, 240, 245, 255);
    if (v->gx_replay_mode && v->gx_replay.ready) { draw_gx_replay(v, font); return; }
    if (v->scene_mode && v->scene_ready) { draw_scene(v, font); return; }
    vita2d_pgf_draw_text(font, 28, 38, white, 1.3f, "Melee Vita - original menu textures");
    if (v->error) {
        vita2d_pgf_draw_text(font, 28, 125, white, 1.0f, v->error);
        vita2d_pgf_draw_text(font, 28, 165, white, 1.0f, "Install the local asset ZIP under ux0:data and restart.");
    } else {
        vita2d_pgf_draw_textf(font, 28, 72, white, 0.9f, "MnMaAll.usd  |  %u textures  |  Page %u/%u",
            (unsigned)v->count, (unsigned)(v->page + 1), (unsigned)((v->count + MV_PAGE_SIZE - 1) / MV_PAGE_SIZE));
        for (unsigned i = 0; i < MV_PAGE_SIZE; ++i) {
            size_t index = v->page * MV_PAGE_SIZE + i;
            if (index >= v->count) break;
            const MvImage *im = &v->images[index];
            float x = 28.0f + (i % 4) * 232.0f, y = 90.0f + (i / 4) * 132.0f;
            vita2d_draw_rectangle(x, y, 216, 100, RGBA8(42, 47, 56, 255));
            if (v->textures[i]) {
                float scale = 204.0f / im->width;
                if (scale > 92.0f / im->height) scale = 92.0f / im->height;
                if (scale > 2.0f) scale = 2.0f;
                vita2d_draw_texture_scale(v->textures[i], x + (216 - im->width * scale) / 2,
                                           y + (100 - im->height * scale) / 2, scale, scale);
            }
            vita2d_pgf_draw_textf(font, (int)x, (int)y + 120, white, 0.7f, "%u  %lux%lu  GX%lu%s",
                (unsigned)index, (unsigned long)im->width, (unsigned long)im->height,
                (unsigned long)im->format, v->textures[i] ? "" : " FAILED");
        }
    }
    vita2d_pgf_draw_text(font, 28, 515, white, 0.82f,
                         "L/R: page   Square: GX replay   Triangle: diagnostics   SELECT+START: exit");
}
void mv_viewer_close(MvViewer *v)
{
    release_page(v);
    mv_gx_replay_close(&v->gx_replay);
    for (size_t i = 0; i < v->scene_texture_count; ++i)
        if (v->scene_textures[i].texture) vita2d_free_texture(v->scene_textures[i].texture);
    mv_hsd_native_free(&v->native);
    mv_scene_free(&v->scene);
    mv_dat_close(&v->dat); free(v->bytes); free(v->images);
    memset(v, 0, sizeof(*v));
}
