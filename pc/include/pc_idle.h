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

#ifdef __cplusplus
}
#endif

#endif /* PC_IDLE_H */
