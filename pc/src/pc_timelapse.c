/* pc_timelapse.c - record a sped-up day of the wallpaper's idle scene
 *
 * With --timelapse (and --wallpaper), once the idle scene has settled the
 * town's calendar clock starts running g_pc_timelapse_speed times faster
 * from the start hour. Every game minute the finished frame is read back
 * and written to ffmpeg's stdin as raw RGBA, so the video is one game minute
 * per frame at 30 fps (a full day = 48 s). The idle camera holds still apart
 * from a very slow pan driven by pc_timelapse_progress(). When the last game
 * minute is written the program exits. */
#include "pc_platform.h"
#include "pc_timelapse.h"
#include "pc_idle.h"

int    g_pc_timelapse = 0;
double g_pc_timelapse_speed = 360.0; /* 6 game minutes per real second */
int    g_pc_timelapse_start = 3;
int    g_pc_timelapse_hours = 24;
char   g_pc_timelapse_out[512] = "timelapse.mp4";
char   g_pc_timelapse_ffmpeg[512] = "ffmpeg";

#define TL_SETTLE_FRAMES 300   /* let the idle scene load and fade in first */
#define TL_FIND_VILLAGER_FRAMES 3600
#define TL_FPS           30
#define TL_GAME_SECS_PER_FRAME 60.0
#define TL_WARMUP_SECS   600.0 /* game time run before the first frame, so the
                                * lighting has caught up with the clock jump */

static int    s_settle = 0;
static int    s_recording = FALSE;
static FILE*  s_pipe = NULL;
static u8*    s_pixels = NULL;
static int    s_w = 0, s_h = 0;
static int    s_frames = 0, s_total_frames = 0;
static float  s_progress = -1.0f;

float pc_timelapse_progress(void) {
    return s_progress;
}

static int tl_open(void) {
    char cmd[2048];

    s_w = g_pc_window_w & ~1;
    s_h = g_pc_window_h & ~1;
    s_pixels = (u8*)malloc((size_t)s_w * s_h * 4);
    if (s_pixels == NULL) return FALSE;
    /* glReadPixels rows are bottom-up: vflip. 1080p H.264, fine quality. */
    snprintf(cmd, sizeof(cmd),
             "\"\"%s\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
             "-vf vflip,scale=-2:1080:flags=lanczos -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p "
             "-movflags +faststart \"%s\"\"",
             g_pc_timelapse_ffmpeg, s_w, s_h, TL_FPS, g_pc_timelapse_out);
    s_pipe = _popen(cmd, "wb");
    if (s_pipe == NULL) {
        fprintf(stderr, "[TIMELAPSE] Couldn't start ffmpeg: %s\n", cmd);
        return FALSE;
    }
    printf("[TIMELAPSE] Recording %d game hours from %02d:00 at %.0fx into %s\n", g_pc_timelapse_hours,
           g_pc_timelapse_start, g_pc_timelapse_speed, g_pc_timelapse_out);
    return TRUE;
}

static void tl_capture(void) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s_w, s_h, GL_RGBA, GL_UNSIGNED_BYTE, s_pixels);
    fwrite(s_pixels, 1, (size_t)s_w * s_h * 4, s_pipe);
    s_frames++;
    if (s_frames % 60 == 0) {
        printf("[TIMELAPSE] %d / %d frames\n", s_frames, s_total_frames);
        fflush(stdout);
    }
}

static void tl_finish(void) {
    if (s_pipe != NULL) _pclose(s_pipe);
    s_pipe = NULL;
    free(s_pixels);
    s_pixels = NULL;
    s_recording = FALSE;
    printf("[TIMELAPSE] Done: %d frames in %s\n", s_frames, g_pc_timelapse_out);
    g_pc_running = 0;
}

void pc_timelapse_frame(void) {
    double elapsed;

    if (!g_pc_timelapse) return;

    if (!s_recording) {
        if (!pc_idle_active()) {
            s_settle = 0;
            return;
        }
        /* open on a villager: wait (up to a minute) for the walker to reach one */
        if (++s_settle < TL_SETTLE_FRAMES) return;
        if (!pc_idle_met_villager() && s_settle < TL_SETTLE_FRAMES + TL_FIND_VILLAGER_FRAMES) return;
        s_progress = 0.0f;
        if (!tl_open()) {
            g_pc_timelapse = 0;
            return;
        }
        s_total_frames = (int)(g_pc_timelapse_hours * 3600.0 / TL_GAME_SECS_PER_FRAME);
        pc_os_start_fast_clock(g_pc_timelapse_speed, g_pc_timelapse_start * 3600 - (int)TL_WARMUP_SECS);
        s_recording = TRUE;
        s_frames = 0;
    }

    /* capture every game minute; the clock is read before this frame was
     * simulated, so a frame lands slightly after its minute */
    elapsed = pc_os_fast_clock_elapsed() - TL_WARMUP_SECS;
    if (elapsed < 0.0) return;
    s_progress = (float)(elapsed / (g_pc_timelapse_hours * 3600.0));
    if (s_progress > 1.0f) s_progress = 1.0f;
    while (s_frames < s_total_frames && elapsed >= s_frames * TL_GAME_SECS_PER_FRAME) {
        tl_capture();
    }
    if (s_frames >= s_total_frames) tl_finish();
}
