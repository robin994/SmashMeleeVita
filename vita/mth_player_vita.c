#include "mth_player_vita.h"
#include "thp_jpeg.h"

#include <psp2/ctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include "render_vita.h"

#define MV_MTH_MAGIC 0x4d544850u
#define MV_VITA_WIDTH 960.0f
#define MV_VITA_HEIGHT 544.0f

typedef struct {
    uint32_t version;
    uint32_t buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t frames;
    uint32_t first_frame;
    uint32_t first_frame_size;
} MvMthHeader;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int read_header(FILE *fp, MvMthHeader *h)
{
    uint8_t raw[0x40];
    if (!fp || !h || fread(raw, 1, sizeof(raw), fp) != sizeof(raw)) return -1;
    if (be32(raw) != MV_MTH_MAGIC) return -2;
    h->version = be32(raw + 0x08);
    h->buffer_size = be32(raw + 0x0c);
    h->width = be32(raw + 0x10);
    h->height = be32(raw + 0x14);
    h->fps = be32(raw + 0x18);
    h->frames = be32(raw + 0x1c);
    h->first_frame = be32(raw + 0x20);
    h->first_frame_size = be32(raw + 0x28);
    if (h->version != 2 || h->width != 640 || h->height != 480 ||
        h->fps != 30 || !h->frames || h->buffer_size > 1024u * 1024u ||
        !h->buffer_size || h->first_frame < sizeof(raw) ||
        h->first_frame_size < 8 || h->first_frame_size > h->buffer_size + 32u) return -3;
    return 0;
}

static int input_action(void)
{
    SceCtrlData pad = {0};
    if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0) return 0;
    if ((pad.buttons & (SCE_CTRL_SELECT | SCE_CTRL_START)) ==
        (SCE_CTRL_SELECT | SCE_CTRL_START)) return 2;
    if (pad.buttons & (SCE_CTRL_START | SCE_CTRL_CROSS)) return 1;
    return 0;
}

typedef struct { GLuint id; unsigned char *pixels; unsigned width,height; int dirty; } MovieTexture;
static MovieTexture *movie_texture_create(unsigned w,unsigned h)
{
    MovieTexture *t=calloc(1,sizeof(*t));if(!t)return NULL;
    t->pixels=malloc((size_t)w*h*4);if(!t->pixels){free(t);return NULL;}
    t->width=w;t->height=h;
    glActiveTexture(GL_TEXTURE0);glGenTextures(1,&t->id);glBindTexture(GL_TEXTURE_2D,t->id);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    return t;
}
static void movie_texture_free(MovieTexture *t)
{glFinish();glDeleteTextures(1,&t->id);free(t->pixels);free(t);}
static unsigned char *movie_pixels(MovieTexture *t) {t->dirty=1;return t->pixels;}
static int movie_pitch(MovieTexture *t) {return (int)t->width*4;}
static void movie_wait(void) {glFinish();}
static void draw_movie_frame(MovieTexture *t,uint32_t width,uint32_t height)
{
    mv_render_begin();glViewport(0,0,960,544);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_ALPHA_TEST);
    glActiveTexture(GL_TEXTURE1);glDisable(GL_TEXTURE_2D);glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,t->id);
    if(t->dirty){glTexSubImage2D(GL_TEXTURE_2D,0,0,0,t->width,t->height,GL_RGBA,GL_UNSIGNED_BYTE,t->pixels);t->dirty=0;}
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
    glMatrixMode(GL_PROJECTION);glLoadIdentity();glOrtho(0,960,544,0,-1,1);
    glMatrixMode(GL_MODELVIEW);glLoadIdentity();
    float w=(float)width*544/(float)height,x=(960-w)*0.5f;
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0,0);glVertex2f(x,0);glTexCoord2f(0,1);glVertex2f(x,544);
    glTexCoord2f(1,0);glVertex2f(x+w,0);glTexCoord2f(1,1);glVertex2f(x+w,544);
    glEnd();mv_render_present();
}

int mv_opening_movie_run(FILE *log)
{
    const char *path = "ux0:data/SmashMeleeVita/files/MvOpen.mth";
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (log) { fprintf(log, "GAME_OPENING_FAIL stage=open path=%s\n", path); fflush(log); }
        return -10;
    }

    MvMthHeader h = {0};
    int result = read_header(fp, &h);
    if (result) {
        if (log) { fprintf(log, "GAME_OPENING_FAIL stage=header code=%d\n", result); fflush(log); }
        fclose(fp);
        return -11;
    }
    if (log) {
        fprintf(log, "GAME_OPENING_HEADER_PASS format=MTHP version=%u width=%u height=%u fps=%u frames=%u first=%u first_size=%u max=%u\n",
                h.version, h.width, h.height, h.fps, h.frames, h.first_frame,
                h.first_frame_size, h.buffer_size);
        fflush(log);
    }

    uint8_t *packed = malloc((size_t)h.buffer_size + 32u);
    if (!packed) { fclose(fp); return -12; }
    uint8_t *standard = malloc(((size_t)h.buffer_size + 32u) * 2u);
    if (!standard) { free(packed); fclose(fp); return -12; }
    tjhandle jpeg = tjInitDecompress();
    if (!jpeg) { free(standard); free(packed); fclose(fp); return -13; }
    if (mv_render_init() < 0) { tjDestroy(jpeg); free(standard); free(packed); fclose(fp); return -14; }

    MovieTexture *texture = movie_texture_create(h.width,h.height);
    if (!texture) {
        mv_render_fini(); tjDestroy(jpeg); free(standard); free(packed); fclose(fp); return -15;
    }


    uint32_t offset = h.first_frame;
    uint32_t packed_size = h.first_frame_size;
    int exit_reason = 0;
    if (log) { fprintf(log, "GAME_OPENING_BEGIN asset=MvOpen.mth decoder=thp_unstuffed_to_jpeg+turbojpeg output=RGBA8888 aspect=4:3\n"); fflush(log); }

    for (uint32_t frame = 0; frame < h.frames; ++frame) {
        if (packed_size < 8 || packed_size > h.buffer_size + 32u ||
            fseek(fp, (long)offset, SEEK_SET) != 0 ||
            fread(packed, 1, packed_size, fp) != packed_size) {
            result = -20;
            if (log) { fprintf(log, "GAME_OPENING_FAIL stage=read frame=%u offset=%u size=%u\n", frame, offset, packed_size); fflush(log); }
            break;
        }
        uint32_t next_size = be32(packed);
        size_t jpeg_size = 0;
        int unpack = mv_thp_jpeg_unpack(packed + 4, packed_size - 4u,
                                        standard, ((size_t)h.buffer_size + 32u) * 2u, &jpeg_size);
        if (unpack) {
            result = -23;
            if (log) { fprintf(log, "GAME_OPENING_FAIL stage=thp_unpack frame=%u code=%d\n", frame, unpack); fflush(log); }
            break;
        }
        /* The same texture is CPU-written on the next iteration. Wait for
         * GXM reads before reusing its storage, not only when freeing it. */
        movie_wait();
        unsigned char *pixels = movie_pixels(texture);
        int pitch = movie_pitch(texture);
        if (!pixels || tjDecompress2(jpeg, standard, (unsigned long)jpeg_size, pixels,
                                     (int)h.width, pitch, (int)h.height,
                                     TJPF_RGBA, TJFLAG_FASTDCT) != 0) {
            result = -21;
            if (log) { fprintf(log, "GAME_OPENING_FAIL stage=jpeg frame=%u size=%u error=%s\n", frame, packed_size, tjGetErrorStr2(jpeg)); fflush(log); }
            break;
        }
        if (log && frame == 0) {
            fprintf(log,
                    "GAME_OPENING_FRAME0_DECODE_PASS jpeg=%u rgba=%ux%u pitch=%d\n",
                    (unsigned)jpeg_size, h.width, h.height, pitch);
            fflush(log);
        }

        /* MvOpen is 30 fps while the Vita presenter is 60 Hz. Present each
         * decoded frame twice, matching the original two-retrace cadence. */
        for (unsigned present = 0; present < 2; ++present) {
            if (log && frame == 0 && present == 0) {
                fprintf(log,
                        "GAME_OPENING_FRAME0_DRAW_BEGIN renderer=" MV_RENDER_NAME "\n");
                fflush(log);
            }
            draw_movie_frame(texture, h.width, h.height);
            if (log && frame == 0 && present == 0) {
                fprintf(log, "GAME_OPENING_FRAME0_DRAW_PASS\n");
                fflush(log);
            }
            int action = input_action();
            if (action) { exit_reason = action; break; }
        }
        if (exit_reason) { result = 0; break; }
        if (log && (frame == 0 || ((frame + 1u) % 300u) == 0u)) {
            fprintf(log, "GAME_OPENING_FRAME frame=%u/%u packed=%u next=%u\n", frame + 1u, h.frames, packed_size, next_size);
            fflush(log);
        }
        offset += packed_size;
        packed_size = next_size;
        if (!packed_size && frame + 1u < h.frames) { result = -22; break; }
    }

    movie_wait();
    movie_texture_free(texture);
    mv_render_fini();
    tjDestroy(jpeg);
    free(standard);
    free(packed);
    fclose(fp);

    if (log) {
        fprintf(log, "GAME_OPENING_END result=%d reason=%s\n", result,
                exit_reason == 2 ? "app_exit" : exit_reason == 1 ? "user_skip" :
                result == 0 ? "eof" : "decode_error");
        fflush(log);
    }
    if (result < 0) return result;
    return exit_reason;
}
