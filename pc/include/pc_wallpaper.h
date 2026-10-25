/* Desktop wallpaper mode: the game renders behind the desktop icons while idle,
 * and a global hotkey / tray icon hands control to the player and back. */
#ifndef PC_WALLPAPER_H
#define PC_WALLPAPER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Set by --wallpaper. Everything below is a no-op unless this is set. */
extern int g_pc_wallpaper_enabled;

/* 1 while the window is the desktop wallpaper (no input, audio scaled by
 * g_pc_wallpaper_volume). 0 while the player is in control. */
extern int g_pc_wallpaper_active;

/* Audio volume while in wallpaper mode, 0-100 percent of master (default 0). */
extern int g_pc_wallpaper_volume;

/* Registers hotkeys + tray icon and attaches the window to the desktop.
 * Call once after the SDL window exists. */
void pc_wallpaper_init(void);

/* Removes the tray icon and hotkeys and detaches the window. */
void pc_wallpaper_shutdown(void);

/* Applies mode switches requested by hotkeys/tray. Call once per frame after
 * the SDL event pump has run (the pump dispatches our window messages). */
void pc_wallpaper_update(void);

/* Request a mode switch; applied on the next pc_wallpaper_update(). */
void pc_wallpaper_request_play(void);
void pc_wallpaper_request_wallpaper(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_WALLPAPER_H */
