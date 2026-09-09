/* Host harness for exactly the data/texture implementation used on Vita. */
#include "asset_file.h"
#include "hsd_anim.h"
#include "hsd_matanim.h"
#include "hsd_native.h"
#include "hsd_scene.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "Usage: asset_probe archive [output-directory | --validate-only]\n"); return 2; }
    unsigned char *bytes;
    size_t size;
    if (mv_read_file(argv[1], &bytes, &size)) return 3;
    MvDat dat;
    if (mv_dat_open(&dat, bytes, size)) { free(bytes); return 4; }
    if (argc > 2 && !strcmp(argv[2], "--validate-only")) {
        mv_dat_close(&dat); free(bytes); return 0;
    }
    MvImage *images = calloc(2048, sizeof(*images));
    size_t count = 0;
    if (!images || mv_menu_textures(&dat, images, 2048, &count)) {
        fprintf(stderr, "typed scene traversal failed: %s\n", argv[1]);
        free(images); mv_dat_close(&dat); free(bytes); return 5;
    }
    uint32_t scene_stats[MV_SCENE_STAT_COUNT] = {0};
    int scene_result = mv_scene_stats(&dat, "MenMainBack_Top_joint", scene_stats);
    uint32_t animated_stats[MV_ANIM_STAT_COUNT] = {0};
    int animated_result = mv_scene_animated_stats(&dat, "MenMainBack_Top_joint",
                                                  "MenMainBack_Top_animjoint", 0.0f,
                                                  animated_stats);
    MvScene matanim_scene;
    MvMatAnimStats matanim_stats;
    int matanim_result = mv_scene_build_frame0(&dat, "MenMainBack_Top_joint",
                                               "MenMainBack_Top_animjoint",
                                               &matanim_scene);
    if (!matanim_result) {
        matanim_result = mv_scene_apply_matanim_frame0(&dat, "MenMainBack_Top_joint",
                                                       "MenMainBack_Top_matanim_joint",
                                                       &matanim_scene, &matanim_stats);
    }
    size_t matanim_dynamic_images = 0;
    float mat_uv_min_u = FLT_MAX, mat_uv_min_v = FLT_MAX;
    float mat_uv_max_u = -FLT_MAX, mat_uv_max_v = -FLT_MAX;
    if (!matanim_result) {
        for (size_t m = 0; m < matanim_scene.mesh_count; ++m) {
            const MvSceneMesh *mesh = &matanim_scene.meshes[m];
            if (mesh->image_desc != UINT32_MAX) {
                size_t i;
                for (i = 0; i < count; ++i)
                    if (images[i].image_desc == mesh->image_desc &&
                        images[i].tlut_desc == mesh->tlut_desc) break;
                if (i == count) {
                    MvImage dynamic_image;
                    if (mv_image_from_desc(&dat, mesh->image_desc, mesh->tlut_desc,
                                           &dynamic_image)) {
                        matanim_result = -1;
                        break;
                    }
                    ++matanim_dynamic_images;
                }
            }
        }
        for (size_t i = 0; i < matanim_scene.vertex_count; ++i) {
            const MvSceneVertex *vertex = &matanim_scene.vertices[i];
            if (!isfinite(vertex->u) || !isfinite(vertex->v)) {
                matanim_result = -1;
                break;
            }
            if (vertex->u < mat_uv_min_u) mat_uv_min_u = vertex->u;
            if (vertex->u > mat_uv_max_u) mat_uv_max_u = vertex->u;
            if (vertex->v < mat_uv_min_v) mat_uv_min_v = vertex->v;
            if (vertex->v > mat_uv_max_v) mat_uv_max_v = vertex->v;
        }
    }
    MvAnimJointSample first_anim_sample;
    int sample_result = -1;
    uint32_t anim_root = UINT32_MAX;
    for (uint32_t i = 0; i < dat.public_count; ++i) {
        const char *name;
        uint32_t offset;
        if (mv_dat_public(&dat, i, &name, &offset)) break;
        if (!strcmp(name, "MenMainBack_Top_animjoint")) { anim_root = offset; break; }
    }
    if (anim_root != UINT32_MAX) {
        MvAnimJointSample root_sample;
        if (!mv_anim_joint_sample(&dat, anim_root, 0.0f, &root_sample) && root_sample.has_child)
            sample_result = mv_anim_joint_sample(&dat, root_sample.child, 0.0f, &first_anim_sample);
    }
    MvCamera camera;
    int camera_result = mv_camera_read(&dat, "ScMenMain_cam_int1_camera", &camera);
    MvScene visibility_scene;
    MvCameraVisibility visibility;
    int visibility_result = scene_result || camera_result ? -1 :
        mv_scene_build(&dat, "MenMainBack_Top_joint", &visibility_scene);
    if (!visibility_result && mv_camera_visibility(&camera, &visibility_scene, &visibility))
        visibility_result = -1;
    MvScene animated_visibility_scene;
    MvCameraVisibility animated_visibility;
    int animated_visibility_result = animated_result || camera_result ? -1 :
        mv_scene_build_animated(&dat, "MenMainBack_Top_joint", "MenMainBack_Top_animjoint",
                                0.0f, &animated_visibility_scene);
    if (!animated_visibility_result &&
        mv_camera_visibility(&camera, &animated_visibility_scene, &animated_visibility))
        animated_visibility_result = -1;
    float uv_min_u = FLT_MAX, uv_min_v = FLT_MAX, uv_max_u = -FLT_MAX, uv_max_v = -FLT_MAX;
    if (!visibility_result) {
        for (size_t i = 0; i < visibility_scene.vertex_count; ++i) {
            const MvSceneVertex *vertex = &visibility_scene.vertices[i];
            if (!isfinite(vertex->u) || !isfinite(vertex->v)) { visibility_result = -1; break; }
            if (vertex->u < uv_min_u) uv_min_u = vertex->u;
            if (vertex->u > uv_max_u) uv_max_u = vertex->u;
            if (vertex->v < uv_min_v) uv_min_v = vertex->v;
            if (vertex->v > uv_max_v) uv_max_v = vertex->v;
        }
    }
    uint32_t native_stats[MV_NATIVE_STAT_COUNT] = {0};
    int native_result = mv_hsd_native_stats(&dat, "MenMainBack_Top_joint", native_stats);
    int failed = scene_result != 0 || animated_result != 0 || matanim_result != 0 ||
                 sample_result != 0 || camera_result != 0 ||
                 visibility_result != 0 || animated_visibility_result != 0 || native_result != 0;
    if (scene_result) fprintf(stderr, "typed geometry traversal failed: %s\n", argv[1]);
    if (animated_result) fprintf(stderr, "frame-0 AnimJoint traversal failed: %s\n", argv[1]);
    if (matanim_result) fprintf(stderr, "frame-0 MatAnim/TexAnim traversal failed: %s result=%d\n",
                                argv[1], matanim_result);
    if (sample_result) fprintf(stderr, "frame-0 FObj known sample failed: %s\n", argv[1]);
    if (camera_result) fprintf(stderr, "typed camera conversion failed: %s\n", argv[1]);
    if (visibility_result) fprintf(stderr, "camera visibility classification failed: %s\n", argv[1]);
    if (animated_visibility_result) fprintf(stderr, "animated camera visibility classification failed: %s\n", argv[1]);
    if (native_result) fprintf(stderr, "native HSD descriptor conversion failed: %s result=%d\n",
                               argv[1], native_result);
    printf("{\"archive_bytes\":%zu,\"public_roots\":%u,\"relocations\":%u,",
           size, dat.public_count, dat.relocation_count);
    if (!scene_result) {
        printf("\"scene\":{\"root\":\"MenMainBack_Top_joint\",\"meshes\":%u,\"vertices\":%u,"
               "\"triangles\":%u,\"joints\":%u,\"dobjs\":%u,\"pobjs\":%u,"
               "\"textured_meshes\":%u,\"skipped_pobjs\":%u,\"skipped_primitives\":%u,"
               "\"unsupported_joints\":%u},",
               scene_stats[MV_SCENE_STAT_MESHES], scene_stats[MV_SCENE_STAT_VERTICES],
               scene_stats[MV_SCENE_STAT_TRIANGLES], scene_stats[MV_SCENE_STAT_JOINTS],
               scene_stats[MV_SCENE_STAT_DOBJS], scene_stats[MV_SCENE_STAT_POBJS],
               scene_stats[MV_SCENE_STAT_TEXTURED_MESHES], scene_stats[MV_SCENE_STAT_SKIPPED_POBJS],
               scene_stats[MV_SCENE_STAT_SKIPPED_PRIMITIVES], scene_stats[MV_SCENE_STAT_UNSUPPORTED_JOINTS]);
    } else {
        printf("\"scene\":null,");
    }
    if (!animated_result) {
        printf("\"animated_scene\":{\"frame\":0,\"meshes\":%u,\"triangles\":%u,"
               "\"joints\":%u,\"anim_joints\":%u,\"aobjs\":%u,\"fobjs\":%u,"
               "\"channels\":%u,\"unsupported\":%u},",
               animated_stats[MV_ANIM_STAT_MESHES], animated_stats[MV_ANIM_STAT_TRIANGLES],
               animated_stats[MV_ANIM_STAT_JOINTS], animated_stats[MV_ANIM_STAT_ANIM_JOINTS],
               animated_stats[MV_ANIM_STAT_AOBJS], animated_stats[MV_ANIM_STAT_FOBJS],
               animated_stats[MV_ANIM_STAT_CHANNELS], animated_stats[MV_ANIM_STAT_UNSUPPORTED]);
    } else {
        printf("\"animated_scene\":null,");
    }
    if (!matanim_result) {
        printf("\"matanim\":{\"frame\":0,\"joints\":%zu,\"matanims\":%zu,"
               "\"texanims\":%zu,\"aobjs\":%zu,\"fobjs\":%zu,\"channels\":%zu,"
               "\"image_channels\":%zu,\"uv_channels\":%zu,\"blend_channels\":%zu,"
               "\"transformed_meshes\":%zu,\"multitexture_meshes\":%zu,\"unsupported\":%zu,"
               "\"dynamic_images\":%zu,\"uv\":[%.9g,%.9g,%.9g,%.9g]},",
               matanim_stats.joint_count, matanim_stats.matanim_count,
               matanim_stats.texanim_count, matanim_stats.aobj_count,
               matanim_stats.fobj_count, matanim_stats.channel_count,
               matanim_stats.image_channel_count, matanim_stats.uv_channel_count,
               matanim_stats.blend_channel_count, matanim_stats.transformed_mesh_count,
               matanim_stats.multitexture_mesh_count, matanim_stats.unsupported_count,
               matanim_dynamic_images, mat_uv_min_u, mat_uv_max_u, mat_uv_min_v, mat_uv_max_v);
    } else {
        printf("\"matanim\":null,");
    }
    if (!sample_result) {
        printf("\"first_anim_sample\":{\"mask\":%u,\"rot\":[%.9g,%.9g,%.9g],"
               "\"translate\":[%.9g,%.9g,%.9g]},",
               (unsigned)first_anim_sample.channel_mask,
               first_anim_sample.channels[0], first_anim_sample.channels[1],
               first_anim_sample.channels[2], first_anim_sample.channels[4],
               first_anim_sample.channels[5], first_anim_sample.channels[6]);
    } else {
        printf("\"first_anim_sample\":null,");
    }
    if (!camera_result) {
        printf("\"camera\":{\"root\":\"ScMenMain_cam_int1_camera\",\"type\":%u,"
               "\"viewport\":[%d,%d,%d,%d],\"eye\":[%.9g,%.9g,%.9g],"
               "\"interest\":[%.9g,%.9g,%.9g],\"up\":[%.9g,%.9g,%.9g],"
               "\"near\":%.9g,\"far\":%.9g,\"fov\":%.9g,\"aspect\":%.9g},",
               (unsigned)camera.projection_type, camera.viewport_xmin, camera.viewport_xmax,
               camera.viewport_ymin, camera.viewport_ymax,
               camera.eye[0], camera.eye[1], camera.eye[2],
               camera.interest[0], camera.interest[1], camera.interest[2],
               camera.up[0], camera.up[1], camera.up[2], camera.near_z, camera.far_z,
               camera.fov_y, camera.aspect);
    } else {
        printf("\"camera\":null,");
    }
    if (!visibility_result) {
        printf("\"visibility\":{\"front\":%zu,\"partial\":%zu,\"behind\":%zu,\"far\":%zu,"
               "\"uv\":[%.9g,%.9g,%.9g,%.9g]},",
               visibility.front, visibility.partial, visibility.behind, visibility.far_count,
               uv_min_u, uv_max_u, uv_min_v, uv_max_v);
    } else {
        printf("\"visibility\":null,");
    }
    if (!animated_visibility_result) {
        printf("\"animated_visibility\":{\"front\":%zu,\"partial\":%zu,\"behind\":%zu,\"far\":%zu,"
               "\"bounds\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g]},",
               animated_visibility.front, animated_visibility.partial, animated_visibility.behind,
               animated_visibility.far_count, animated_visibility_scene.min_x,
               animated_visibility_scene.min_y, animated_visibility_scene.min_z,
               animated_visibility_scene.max_x, animated_visibility_scene.max_y,
               animated_visibility_scene.max_z);
    } else {
        printf("\"animated_visibility\":null,");
    }
    if (!native_result) {
        printf("\"native\":{\"joints\":%u,\"dobjs\":%u,\"mobjs\":%u,\"pobjs\":%u,"
               "\"tobjs\":%u,\"vtxdescs\":%u,\"images\":%u,\"tluts\":%u,"
               "\"materials\":%u,\"pedescs\":%u,\"lods\":%u,\"tevs\":%u,"
               "\"unsupported\":%u},",
               native_stats[MV_NATIVE_STAT_JOINTS], native_stats[MV_NATIVE_STAT_DOBJS],
               native_stats[MV_NATIVE_STAT_MOBJS], native_stats[MV_NATIVE_STAT_POBJS],
               native_stats[MV_NATIVE_STAT_TOBJS], native_stats[MV_NATIVE_STAT_VTXDESCS],
               native_stats[MV_NATIVE_STAT_IMAGES], native_stats[MV_NATIVE_STAT_TLUTS],
               native_stats[MV_NATIVE_STAT_MATERIALS], native_stats[MV_NATIVE_STAT_PEDESCS],
               native_stats[MV_NATIVE_STAT_LODS], native_stats[MV_NATIVE_STAT_TEVS],
               native_stats[MV_NATIVE_STAT_UNSUPPORTED]);
    } else {
        printf("\"native\":null,");
    }
    printf("\"textures\":[");
    for (size_t i = 0; i < count; ++i) {
        MvImage *im = &images[i];
        size_t length = (size_t)im->width * im->height * 4;
        uint8_t *rgba = malloc(length);
        if (!rgba || mv_image_decode(&dat, im, rgba, length, im->width * 4)) {
            fprintf(stderr, "decode failed: index=%zu image=%x format=%u\n", i, im->image_desc, im->format);
            free(rgba); failed = 1; break;
        }
        printf("%s{\"index\":%zu,\"image_desc\":%u,\"width\":%u,\"height\":%u,\"format\":%u}",
               i ? "," : "", i, im->image_desc, im->width, im->height, im->format);
        if (argc > 2) {
            char name[4096];
            int n = snprintf(name, sizeof(name), "%s/%04zu.rgba", argv[2], i);
            FILE *output = n > 0 && (size_t)n < sizeof(name) ? fopen(name, "wb") : NULL;
            if (!output) failed = 1;
            else { if (fwrite(rgba, 1, length, output) != length) failed = 1; if (fclose(output)) failed = 1; }
        }
        free(rgba);
        if (failed) break;
    }
    printf("]}\n");
    if (!visibility_result) mv_scene_free(&visibility_scene);
    if (!animated_visibility_result) mv_scene_free(&animated_visibility_scene);
    if (!matanim_result) mv_scene_free(&matanim_scene);
    free(images); mv_dat_close(&dat); free(bytes);
    if (!scene_result && !scene_stats[MV_SCENE_STAT_TRIANGLES]) failed = 1;
    return failed ? 6 : 0;
}
