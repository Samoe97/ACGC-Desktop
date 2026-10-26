/* Timelapse recorder for the wallpaper's idle scene: runs the town's
 * calendar clock fast, holds the camera on an extremely slow pan, and pipes
 * one frame per game minute into ffmpeg. */
#ifndef PC_TIMELAPSE_H
#define PC_TIMELAPSE_H

#ifdef __cplusplus
extern "C" {
#endif

extern int    g_pc_timelapse;           /* --timelapse */
extern double g_pc_timelapse_speed;     /* clock speed-up (default 360) */
extern int    g_pc_timelapse_start;     /* start hour (default 0) */
extern int    g_pc_timelapse_hours;     /* game hours to record (default 24) */
extern char   g_pc_timelapse_out[512];  /* output file (default timelapse.mp4) */
extern char   g_pc_timelapse_ffmpeg[512];

/* Called once per frame, right before the buffer swap. */
void pc_timelapse_frame(void);

/* Pan progress for the idle camera: < 0 before recording starts, then 0..1. */
float pc_timelapse_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_TIMELAPSE_H */
