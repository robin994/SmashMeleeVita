#include "render_vita.h"
#include "audio_boot_vita.h"
#include "gc_runtime_vita.h"
static int initialized;
static int compiler_configured;
int mv_render_init(void)
{
    if (!initialized) {
        if (!compiler_configured) {
            /* Fixed-function vitaGL paths synthesize shaders at runtime.  Set
             * the compiler policy explicitly before the first context init;
             * relying on an implicit/default compiler setup made init fail on
             * hardware after the vitaGL splash. */
            vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE,
                                          SHARK_ENABLE, SHARK_ENABLE);
            compiler_configured = 1;
        }

        /* Reserve the renderer before Melee consumes its large HSD/audio
         * heaps.  The first argument is vitaGL's GL1 immediate-mode vertex
         * pool.  The movie quad and the GX replay both use glBegin/glVertex*,
         * so this must be non-zero or legacy_pool_ptr stays NULL. */
        /* vitaGL's return value is not a success flag.  GL_TRUE means the
         * requested framebuffer size had to be clamped to the display's
         * maximum resolution; GL_FALSE is the normal 960x544 case. */
        GLboolean resolution_fallback =
            vglInitExtended(4 * 1024 * 1024, 960, 544, 16 * 1024 * 1024,
                            SCE_GXM_MULTISAMPLE_NONE);
        (void) resolution_fallback;

        /* At this point vitaGL has set its internal vgl_inited flag.  Use an
         * actual GL query to validate that the context is usable instead of
         * interpreting vglInitExtended's resolution-fallback result. */
        if (!glGetString(GL_VERSION) || !glGetString(GL_RENDERER))
            return -1;
        initialized=1;
    }
    glClearColor(0,0,0,1);
    return 0;
}
/* One shared context survives movie/title/menu transitions. */
void mv_render_fini(void) { glFinish(); }
void mv_render_begin(void)
{
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    /* The GL context is shared by movie/title/menu/gameplay.  Do not inherit
     * a stale depth clear/range from a previous renderer path: retail GX uses
     * a 0..1 viewport depth range and clears the Z buffer to the far plane. */
    glDepthRangef(0.0f,1.0f);
    glClearDepthf(1.0f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
}
void mv_render_present(void)
{
    /* Native title/menu/CSS loops do not pass through VIWaitForRetrace().
     * Advance one host-side async device boundary and the time-gated AX control
     * engine here.  Retail gameplay may hit both this and VIWaitForRetrace;
     * AX is elapsed-time gated, while an extra async completion is harmless
     * and mirrors independent DVD/ARAM interrupt progress on GameCube. */
    mv_gc_async_pump();
    mv_gc_alarm_pump();
    mv_ax_vblank_pump();
    vglSwapBuffers(GL_FALSE);
}
