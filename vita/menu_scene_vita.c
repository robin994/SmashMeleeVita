#include "menu_scene_vita.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MV_VITA_WIDTH 960.0f
#define MV_VITA_HEIGHT 544.0f

typedef struct {
    float x, y, depth, u, v;
    uint32_t rgba;
} MvMenuCameraVertex;

typedef struct {
    float right[3], up[3], forward[3];
} MvMenuCameraBasis;

static unsigned vita_color(uint32_t rgba)
{
    return RGBA8(rgba >> 24, rgba >> 16, rgba >> 8, rgba);
}

static int normalize3(float v[3])
{
    float length2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!isfinite(length2) || length2 < 1.0e-12f) return -1;
    float inv = 1.0f / sqrtf(length2);
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
    return 0;
}

static void cross3(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int camera_basis(const MvCamera *camera, MvMenuCameraBasis *basis)
{
    for (unsigned i = 0; i < 3; ++i)
        basis->forward[i] = camera->interest[i] - camera->eye[i];
    if (normalize3(basis->forward)) return -1;
    cross3(basis->forward, camera->up, basis->right);
    if (normalize3(basis->right)) return -1;
    cross3(basis->right, basis->forward, basis->up);
    return normalize3(basis->up);
}

static MvMenuCameraVertex camera_vertex(const MvCamera *camera,
                                        const MvMenuCameraBasis *basis,
                                        const MvSceneVertex *source)
{
    float delta[3] = {
        source->x - camera->eye[0], source->y - camera->eye[1],
        source->z - camera->eye[2]
    };
    MvMenuCameraVertex out;
    out.x = delta[0] * basis->right[0] + delta[1] * basis->right[1] +
            delta[2] * basis->right[2];
    out.y = delta[0] * basis->up[0] + delta[1] * basis->up[1] +
            delta[2] * basis->up[2];
    out.depth = delta[0] * basis->forward[0] + delta[1] * basis->forward[1] +
                delta[2] * basis->forward[2];
    out.u = source->u;
    out.v = source->v;
    out.rgba = source->rgba;
    return out;
}

static uint32_t lerp_rgba(uint32_t a, uint32_t b, float t)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 24; shift += 8) {
        float av = (float)((a >> shift) & 0xffu);
        float bv = (float)((b >> shift) & 0xffu);
        unsigned value = (unsigned)(av + (bv - av) * t + 0.5f);
        if (value > 255u) value = 255u;
        result |= value << shift;
    }
    return result;
}

static MvMenuCameraVertex lerp_vertex(const MvMenuCameraVertex *a,
                                      const MvMenuCameraVertex *b, float t)
{
    MvMenuCameraVertex out;
    out.x = a->x + (b->x - a->x) * t;
    out.y = a->y + (b->y - a->y) * t;
    out.depth = a->depth + (b->depth - a->depth) * t;
    out.u = a->u + (b->u - a->u) * t;
    out.v = a->v + (b->v - a->v) * t;
    out.rgba = lerp_rgba(a->rgba, b->rgba, t);
    return out;
}

static size_t clip_depth(const MvMenuCameraVertex *input, size_t count,
                         MvMenuCameraVertex *output, float plane,
                         int keep_greater)
{
    if (!count) return 0;
    size_t out_count = 0;
    MvMenuCameraVertex previous = input[count - 1];
    int previous_inside = keep_greater ? previous.depth >= plane : previous.depth <= plane;
    for (size_t i = 0; i < count; ++i) {
        MvMenuCameraVertex current = input[i];
        int current_inside = keep_greater ? current.depth >= plane : current.depth <= plane;
        if (current_inside != previous_inside) {
            float denominator = current.depth - previous.depth;
            if (fabsf(denominator) > 1.0e-12f) {
                float t = (plane - previous.depth) / denominator;
                output[out_count] = lerp_vertex(&previous, &current, t);
                output[out_count++].depth = plane;
            }
        }
        if (current_inside) output[out_count++] = current;
        previous = current;
        previous_inside = current_inside;
    }
    return out_count;
}

static void viewport(const MvCamera *camera, float *x, float *y,
                     float *w, float *h)
{
    float aspect = camera && camera->aspect > 0.0f ? camera->aspect : (4.0f / 3.0f);
    *h = MV_VITA_HEIGHT;
    *w = *h * aspect;
    if (*w > MV_VITA_WIDTH) {
        *w = MV_VITA_WIDTH;
        *h = *w / aspect;
    }
    *x = (MV_VITA_WIDTH - *w) * 0.5f;
    *y = (MV_VITA_HEIGHT - *h) * 0.5f;
}

static int project(const MvCamera *camera, const MvMenuCameraVertex *source,
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
        nx = (2.0f * px - camera->right - camera->left) /
             (camera->right - camera->left);
        ny = (2.0f * py - camera->top - camera->bottom) /
             (camera->top - camera->bottom);
    } else {
        nx = (2.0f * source->x - camera->right - camera->left) /
             (camera->right - camera->left);
        ny = (2.0f * source->y - camera->top - camera->bottom) /
             (camera->top - camera->bottom);
    }
    if (!isfinite(nx) || !isfinite(ny)) return -1;
    *x = view_x + (nx * 0.5f + 0.5f) * view_w;
    *y = view_y + (0.5f - ny * 0.5f) * view_h;
    return 0;
}

static vita2d_texture *find_texture(const MvMenuSceneOverlay *overlay,
                                    uint32_t image_desc, uint32_t tlut_desc)
{
    for (size_t i = 0; i < overlay->texture_count; ++i) {
        const MvMenuOverlayTexture *entry = &overlay->textures[i];
        if (entry->image_desc == image_desc && entry->tlut_desc == tlut_desc)
            return entry->texture;
    }
    return NULL;
}

static int load_textures(MvMenuSceneOverlay *overlay, const MvDat *dat,
                         FILE *log)
{
    for (size_t i = 0; i < overlay->scene.mesh_count; ++i) {
        const MvSceneMesh *mesh = &overlay->scene.meshes[i];
        if (mesh->image_desc == UINT32_MAX ||
            find_texture(overlay, mesh->image_desc, mesh->tlut_desc))
            continue;
        if (overlay->texture_count >= MV_MENU_OVERLAY_TEXTURES) return -1;

        MvImage image;
        if (mv_image_from_desc(dat, mesh->image_desc, mesh->tlut_desc, &image)) return -2;
        vita2d_texture *texture = vita2d_create_empty_texture_format(
            image.width, image.height, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        if (!texture) return -3;
        size_t stride = vita2d_texture_get_stride(texture);
        uint8_t *pixels = vita2d_texture_get_datap(texture);
        if (!pixels || mv_image_decode(dat, &image, pixels, stride * image.height, stride)) {
            vita2d_free_texture(texture);
            return -4;
        }
        vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_LINEAR,
                                   SCE_GXM_TEXTURE_FILTER_LINEAR);
        MvMenuOverlayTexture *entry = &overlay->textures[overlay->texture_count++];
        entry->image_desc = mesh->image_desc;
        entry->tlut_desc = mesh->tlut_desc;
        entry->texture = texture;
    }
    if (log) {
        fprintf(log, "GAME_MENU_OVERLAY_TEXTURES ready=%u\n",
                (unsigned)overlay->texture_count);
        fflush(log);
    }
    return 0;
}

int mv_menu_scene_overlay_init(MvMenuSceneOverlay *overlay, const MvDat *dat,
                               const char *joint_root, const char *anim_root,
                               float frame, FILE *log)
{
    if (!overlay || !dat || !joint_root) return -1;
    memset(overlay, 0, sizeof(*overlay));
    int result = anim_root ?
        mv_scene_build_animated(dat, joint_root, anim_root, frame, &overlay->scene) :
        mv_scene_build(dat, joint_root, &overlay->scene);
    if (result) return -2;
    result = load_textures(overlay, dat, log);
    if (result) {
        mv_menu_scene_overlay_close(overlay);
        return -10 + result;
    }
    overlay->ready = 1;
    if (log) {
        fprintf(log,
                "GAME_MENU_OVERLAY_PASS root=%s frame=%.1f joints=%u meshes=%u "
                "triangles=%u textures=%u unsupported_anim=%u skipped_pobj=%u\n",
                joint_root, frame, (unsigned)overlay->scene.joint_count,
                (unsigned)overlay->scene.mesh_count,
                (unsigned)(overlay->scene.vertex_count / 3),
                (unsigned)overlay->texture_count,
                (unsigned)overlay->scene.unsupported_anim_count,
                (unsigned)overlay->scene.skipped_pobj_count);
        fflush(log);
    }
    return 0;
}

void mv_menu_scene_overlay_draw(const MvMenuSceneOverlay *overlay,
                                const MvCamera *camera)
{
    if (!overlay || !overlay->ready || !camera) return;
    MvMenuCameraBasis basis;
    if (camera_basis(camera, &basis)) return;
    float view_x, view_y, view_w, view_h;
    viewport(camera, &view_x, &view_y, &view_w, &view_h);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle((int)view_x, (int)view_y,
                              (int)(view_x + view_w), (int)(view_y + view_h));

    for (size_t mi = 0; mi < overlay->scene.mesh_count; ++mi) {
        const MvSceneMesh *mesh = &overlay->scene.meshes[mi];
        const MvSceneVertex *source = &overlay->scene.vertices[mesh->first_vertex];
        vita2d_texture *texture = mesh->image_desc == UINT32_MAX ? NULL :
            find_texture(overlay, mesh->image_desc, mesh->tlut_desc);
        size_t capacity = mesh->vertex_count * 3u;
        if (!capacity) continue;

        if (texture) {
            vita2d_texture_vertex *out =
                vita2d_pool_malloc((unsigned)(capacity * sizeof(*out)));
            if (!out) continue;
            size_t out_count = 0;
            for (size_t i = 0; i + 2 < mesh->vertex_count; i += 3) {
                MvMenuCameraVertex tri[3], near_clip[6], far_clip[8];
                for (unsigned k = 0; k < 3; ++k)
                    tri[k] = camera_vertex(camera, &basis, &source[i + k]);
                size_t clipped = clip_depth(tri, 3, near_clip, camera->near_z, 1);
                clipped = clip_depth(near_clip, clipped, far_clip, camera->far_z, 0);
                for (size_t k = 1; k + 1 < clipped; ++k) {
                    const MvMenuCameraVertex *fan[3] = {
                        &far_clip[0], &far_clip[k], &far_clip[k + 1]
                    };
                    if (out_count + 3 > capacity) break;
                    int valid = 1;
                    for (unsigned n = 0; n < 3; ++n) {
                        vita2d_texture_vertex *dst = &out[out_count + n];
                        if (project(camera, fan[n], view_x, view_y, view_w, view_h,
                                    &dst->x, &dst->y)) {
                            valid = 0;
                            break;
                        }
                        dst->z = 0.0f;
                        dst->u = fan[n]->u;
                        dst->v = fan[n]->v;
                    }
                    if (valid) out_count += 3;
                }
            }
            if (out_count) {
                vita2d_draw_array_textured(texture, SCE_GXM_PRIMITIVE_TRIANGLES,
                                           out, out_count,
                                           vita_color(mesh->material_rgba));
            }
        } else {
            vita2d_color_vertex *out =
                vita2d_pool_malloc((unsigned)(capacity * sizeof(*out)));
            if (!out) continue;
            size_t out_count = 0;
            for (size_t i = 0; i + 2 < mesh->vertex_count; i += 3) {
                MvMenuCameraVertex tri[3], near_clip[6], far_clip[8];
                for (unsigned k = 0; k < 3; ++k)
                    tri[k] = camera_vertex(camera, &basis, &source[i + k]);
                size_t clipped = clip_depth(tri, 3, near_clip, camera->near_z, 1);
                clipped = clip_depth(near_clip, clipped, far_clip, camera->far_z, 0);
                for (size_t k = 1; k + 1 < clipped; ++k) {
                    const MvMenuCameraVertex *fan[3] = {
                        &far_clip[0], &far_clip[k], &far_clip[k + 1]
                    };
                    if (out_count + 3 > capacity) break;
                    int valid = 1;
                    for (unsigned n = 0; n < 3; ++n) {
                        vita2d_color_vertex *dst = &out[out_count + n];
                        if (project(camera, fan[n], view_x, view_y, view_w, view_h,
                                    &dst->x, &dst->y)) {
                            valid = 0;
                            break;
                        }
                        dst->z = 0.0f;
                        dst->color = vita_color(fan[n]->rgba);
                    }
                    if (valid) out_count += 3;
                }
            }
            if (out_count)
                vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, out, out_count);
        }
    }
    vita2d_disable_clipping();
}

void mv_menu_scene_overlay_close(MvMenuSceneOverlay *overlay)
{
    if (!overlay) return;
    vita2d_wait_rendering_done();
    for (size_t i = 0; i < overlay->texture_count; ++i) {
        if (overlay->textures[i].texture)
            vita2d_free_texture(overlay->textures[i].texture);
    }
    mv_scene_free(&overlay->scene);
    memset(overlay, 0, sizeof(*overlay));
}
