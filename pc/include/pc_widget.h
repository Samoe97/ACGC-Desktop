/* Desktop widget for wallpaper mode: an invisible click layer over the panel
 * the game draws in the bottom-right corner (pc_idle_ui.c: the in-game clock,
 * camera controls, settings and Play), plus the settings dialog. It sits
 * above the desktop icons but below every normal window. */
#ifndef PC_WIDGET_H
#define PC_WIDGET_H

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the widget as a child of the desktop icon host and shows it.
 * Safe to call again (e.g. after Explorer restarts): it re-attaches. */
void pc_widget_attach(void);

/* Shows/hides it (hidden while the player is in control). */
void pc_widget_set_visible(int visible);

void pc_widget_destroy(void);

/* Reads wallpaper.ini (the town clock offset). Call before the game boots. */
void pc_widget_load_settings(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_WIDGET_H */
