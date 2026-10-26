/* Wallpaper idle mode: while the desktop wallpaper is idle, the title demo
 * (which already runs the player's own town from the save, and never saves)
 * becomes an endless attract mode. An invisible autopilot walks the demo
 * player around town so terrain/actors stream in around it, and a slow,
 * zoomed-out camera follows. */
#ifndef PC_IDLE_H
#define PC_IDLE_H

#include "m_play.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TRUE while the wallpaper is idle and the title demo is running. */
int pc_idle_active(void);

/* Replaces the recorded title-demo input for this frame. Call from
 * title_demo_move() instead of feeding recorded keys when pc_idle_active(). */
void pc_idle_title_demo_move(GAME_PLAY* play);

/* Computes this frame's idle camera. Returns FALSE (and touches nothing)
 * when idle mode is off, so the game's own camera runs as usual. */
int pc_idle_camera(GAME_PLAY* play, xyz_t* center, xyz_t* eye, f32* fov_y, f32* near, f32* far);

/* User camera controls (desktop widget). Limited and eased; the view is
 * still kept inside the map. */
void pc_idle_user_rotate(f32 degrees);
void pc_idle_user_zoom(f32 delta);   /* zoom level: 0 = gameplay camera, 1 = default */
void pc_idle_user_reset(void);

/* "Camera speed" setting (1.0 = the original pace; default 0.6). Scales the
 * walker's pace, its pauses, the camera's follow lag and drift together. */
void pc_idle_set_speed(f32 speed);
f32  pc_idle_get_speed(void);

/* Restart the idle scene (fade to a fresh title-demo scene) so a changed
 * clock's season, trees and lighting take effect. */
void pc_idle_request_reload(void);
int  pc_idle_take_reload_request(void);

/* Called when a title-demo scene starts: re-centre the camera, new targets. */
void pc_idle_scene_start(void);

/* TRUE once the walker has reached a villager in this scene (the timelapse
 * recorder waits for it so the video opens on someone). */
int pc_idle_met_villager(void);

/* The game's clock (the town's RTC), for the desktop widget. */
void pc_idle_get_clock(int* hour, int* min, int* sec, int* weekday, int* month, int* day);

#ifdef __cplusplus
}
#endif

#endif /* PC_IDLE_H */
