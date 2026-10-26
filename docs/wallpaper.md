# Desktop wallpaper mode: technical documentation

This document explains how `--wallpaper` works and lists every change it makes to the PC port and the decompiled game code, with the reason for each. For usage, see the [README](../README.md).

All game-code changes sit behind `#ifdef TARGET_PC`. The behaviour that only matters for the wallpaper is also gated at runtime, either on `g_pc_wallpaper_enabled` (the `--wallpaper` flag) or on `pc_idle_active()` (the wallpaper is idle and the title demo is running). Without `--wallpaper`, the program behaves like the upstream PC port. The only exceptions are the shared bug fixes and the larger PC buffers listed in [Changes outside wallpaper mode](#changes-outside-wallpaper-mode).

## Contents

- [Overview](#overview)
- [Modules](#modules)
- [Putting the window on the desktop](#putting-the-window-on-the-desktop)
- [Idle mode](#idle-mode)
- [The town clock](#the-town-clock)
- [The desktop widget](#the-desktop-widget)
- [Game-code changes, file by file](#game-code-changes-file-by-file)
- [Changes outside wallpaper mode](#changes-outside-wallpaper-mode)
- [Troubleshooting](#troubleshooting)
- [Ideas for later](#ideas-for-later)

## Overview

```
                 Ctrl+Alt+P / Play button / tray
   ┌───────────────────────────────────────────────────┐
   │                                                   ▼
┌──┴───────────────────────────┐        ┌──────────────────────────────┐
│ Wallpaper (idle)             │        │ Playing                      │
│ window: child of the desktop │        │ window: top-level, borderless│
│   behind the icons           │        │   fullscreen, focused        │
│ input: none, audio: muted    │        │ input + audio: normal        │
│ scene: title demo + autopilot│        │ scene: title screen → game   │
│ widget: shown                │        │ widget: hidden               │
└──────────────────────────────┘        └──────────────┬───────────────┘
   ▲               Ctrl+Alt+W / tray                   │
   └───────────────────────────────────────────────────┘
```

The game's title screen already plays a "title demo": a live play scene in the player's own town, loaded from the save. Pressing *Start Game* reloads the save from disk (`aAL_title_game_data_init_start_select` → `pc_save_reload`), so nothing that happens during the demo is ever written to the memory card. Idle mode is that demo with several things changed:
- **Input:** the recorded controller input is replaced by an autopilot.
- **Timing:** the 60-second demo timer is frozen.
- **Camera:** a new one replaces the demo's.
- **Overlays:** the logo, menu and player character are hidden.
- **Clock:** the clock, season and weather come from the real world instead of the demo's fixed days.

## Modules

| File | Purpose |
|---|---|
| `pc/src/pc_wallpaper.c` | Parents the game window into the desktop. Handles the hotkeys, the tray icon, switching modes, focus hand-off, and the clock resync timer. |
| `pc/src/pc_idle.c` | Idle mode: the autopilot walker, the idle camera (including the user's rotate and zoom, and the map-bounds clamp), scene reload requests, and the clock getter for the widget. |
| `pc/src/pc_widget.c` | The desktop clock widget: per-pixel-alpha rendering, hit testing, and the settings dialog with `wallpaper.ini`. |
| `pc/src/pc_os.c` | The game clock (`osGetTime`): PC clock plus an offset, the overflow fix, and resync. |
| `pc/include/pc_wallpaper.h`, `pc_idle.h`, `pc_widget.h` | The APIs of the three modules above. |
| `pc/res/app.manifest` | Declares Windows 8+ support and Common Controls v6. |

Everything runs on the game's main thread. The hidden message window, the widget and the settings dialog are created on that thread. SDL's event pump (called every frame from `VIWaitForRetrace` → `pc_platform_poll_events`) dispatches their messages. Mode switches requested from window procedures are queued and applied in `pc_wallpaper_update()`, after the pump.

## Putting the window on the desktop

`wp_find_desktop_host()` in `pc_wallpaper.c`:

1. It sends Progman the undocumented message `0x052C`. That makes Explorer create a `WorkerW` window between the icons (`SHELLDLL_DefView`) and the static wallpaper.
2. **Windows 11 24H2 and later:** `WorkerW` is a child of Progman. The game window becomes a child of Progman, stacked below `SHELLDLL_DefView` and above `WorkerW`.
3. **Older Windows:** `WorkerW` is a top-level window right after the one that hosts `SHELLDLL_DefView`. The game window becomes its child.

The window's style is changed to `WS_CHILD`, and it's sized to the primary monitor in the host's client coordinates. With `--wallpaper` the SDL window is created hidden, so it never flashes on screen before attaching.

**Switching to play:** `wp_enter_play()` restores the saved style and parent, switches to borderless fullscreen and takes focus.
- **Focus:** Windows only lets the process that received the last input take focus. If `SetForegroundWindow` fails, which can happen after a click on the desktop widget, it taps Alt with `keybd_event` and tries again.
- **Remembered window:** the window that had focus before Play is remembered. On return it gets focus back, or the desktop does.

**Explorer restarts** are caught through the `TaskbarCreated` broadcast. The tray icon and widget are recreated and the window re-attached.

**Shutdown:** the window is detached, and the current wallpaper is re-applied without being saved (`SPI_SETDESKWALLPAPER` with the current path), so the desktop repaints over the last frame.

While idle, `PADRead` returns neutral input (`pc_pad.c`), keyboard and controller events are dropped in `pc_platform_poll_events` (`pc_main.c`), and audio is scaled by `--wallpaper-volume` in `pc_audio.c`.

## Idle mode

`pc_idle_active()` is true when the wallpaper is idle and the title demo is running (`mEv_IsTitleDemo()`).

### The autopilot walker (`pc_idle_title_demo_move`)

The title demo already drives its stand-in player with fake controller data (`mPlib_SetData1_controller_data_for_title_demo`). The autopilot feeds that same channel. Movement, collision, water and acre crossings stay entirely in the game's hands.

- **Targets:**
  - **About 45% of the time, a villager** (`ACTOR_PART_NPC`) at least 250 units away. The walker stops 90 units short and waits 5–11 seconds.
  - **Otherwise, a random spot** in a town block, usually a neighbouring one, avoiding ocean and border blocks. It waits 1.5–4.5 seconds there.
- **Stick input:** the player's world direction is `270° + stick angle + camera yaw` (`Player_actor_Get_ControllerAngle`), so the stick angle is worked out backwards from the direction to the target.
- **Acre crossings:** the game only starts the scripted "wade" into the next acre when the stick is pushed hard (above 0.65) and nearly straight at the border (`Player_actor_CheckAbleMoveWadeBlock`). Within 60 units of a border the autopilot squares up and pushes at full tilt.
- **Stuck detection:** if the walker gets no closer by 40 units in 3 seconds (a river, a cliff, a building), it gives up on that target.
- **Hidden player:** `Player_actor_draw` returns early, and the shadow is turned off (`shape_info.draw_shadow`), then restored when idle mode ends.
- **No buildings:** `Player_actor_request_main_door` refuses while idle. Otherwise the walker could wander into a shop and get stuck in a conversation that waits for a button press.
- **Frozen demo timer:** `title_demo_move` returns before advancing `S_tdemo_time`, so the demo never fades to the next one.
- **Camera speed** (`pc_idle_set_speed`, the *Camera speed* setting): Very slow 0.35, Slow 0.6 (default), Normal 1.0, Fast 1.5. It scales the walk (stick 34 × speed, at least 14, just above the dead zone), the pauses and the stuck timer (both ÷ speed), the camera's follow lag, and the clock that drives the sway and zoom drift. Acre crossings still push at full tilt, because the game requires it; the camera's lag smooths that out.

### The idle camera (`pc_idle_camera`)

`Camera2_process` asks `pc_idle_camera` first. If it returns TRUE, the camera's eye, center, field of view and near/far planes are set directly, and the view matrices are built as in `Camera2_SetView`, minus the gameplay override of near/far. The game's camera state machine is bypassed while idle and takes over again on Play.

- **Following:** the center follows the walker with a 2.5-second exponential lag.
- **Framing:** distance 1500 (breathing ±150 over 170 s), pitch 57° (±3° over 230 s), yaw sway ±14° over 300 s, field of view 28°, far plane 4000.
- **Zoom level:** 0 is exactly the gameplay camera (1240 units back, 45° pitch, 20° field of view; see `Init_Camera2`), and 1 is the default idle view. Values in between interpolate distance, pitch and field of view together, so zooming in also tilts the camera up. Values above 1 (up to 1.25) only move the camera further back.
- **User rotation:** ±35°. Acre crossings need a roughly north-up camera.
- **Map bounds:** `idle_view_footprint()` intersects the four corner rays of the view with the ground plane. The top corners are stretched by 1.7× to allow for the world's "rolling log" bend, which lets the camera see further than a flat plane would. If the footprint passes x = 0, x = 7 × 640 or z = 0, the whole view is slid back inside.

### Loading the full 3×3 around the walker

The stock game creates trees, items and buildings only for the **4** blocks (2×2) nearest the player. That's fine for the gameplay camera, but the idle camera sees about 2½ blocks across.

- **`mFM_VISIBLE_BLOCK_NUM` is 9 on PC** (`m_field_make.h`). Arrays sized by it grow, but only the title-demo field in wallpaper mode actually allocates 9 BG slots (`mFM_FieldInit`). Every other scene keeps its own slot count.
- **`mFI_BGDisplayListRefresh`** loads the whole 3×3 when the field has 9 slots. `mFI_GetFreeRegisterBgInfo` never hands out a slot beyond the field's own count, because those have no display-list buffer.
- **`mFI_GetItemTable_NoReset`:** its first pass pins the player's row. It was written for at most 2 blocks per row and gave the left and right neighbours the same table index, so with a 3×3 one of them lost its trees. With 9 slots, only the player's own block is pinned.
- **Slot bitfields** grew from `u8` to `u16`: `remove_cut_tree_info_bitfield` in `common_data` and the matching locals.
- **Buildings and other actors:** `ac_birth_control` spawns all 8 neighbours when the field has 9 slots, instead of the 3 on the player's quadrant side.
- **Ground:** `ac_field_draw` draws a 5×5 instead of a 3×3 when the field has 9 slots. Every block's mesh is resident; the outer ring only draws blocks that exist.
- **Culling:** `Actor_draw_actor_no_culling_check2` allows 2500 more units of depth while idle. The idle camera is about 1.5× further away, so without this the top of the screen culled (and froze) buildings.
- **Train:** `ac_train0` normally deletes itself as soon as the player leaves its row of acres. While idle it tolerates one row of distance.

### Weather particles

Leaves, cherry petals, snow and rain live in a small box (about 200 × 380 units) centred on the camera target, wrapping around when they leave it. From the idle camera that's a narrow column that follows the camera.
- **Bigger box:** while idle the box is scaled 9× wide and 4.5× deep (`aWEATHER_AREA_X/Z`), which is larger than the visible ground. On-screen particles stay where they are in the world, and only off-screen ones wrap.
- **Spawning:** `make()` runs 16 times per tick to keep a similar density, and the pool is 600 on PC (100 originally). `aWeather_Priv::id` became `u16` so it can index the bigger pool.
- **Widescreen:** the draw check uses the widescreen width (`aWEATHER_ON_SCREEN_X`) instead of the 4:3 one.

### Scene reloads

`pc_idle_request_reload()` (after a clock change in the settings) and a change of date (midnight) make `title_demo_move` call `mTD_game_end_init`. The scene fades out, and a fresh title-demo scene loads with the new date: season, trees, events and weather. `mTD_player_keydata_init` calls `pc_idle_scene_start()`, which re-centres the camera on the new scene.

## The town clock

The stock PC port reads the PC clock once at startup and then advances it with the performance counter (`osGetTime`). Two problems for a wallpaper that runs for days:

1. **Overflow:** `(now - start) * GC_TIMER_CLOCK` overflowed 64 bits after about 12.5 hours of uptime at a 10 MHz counter. It's now split into whole seconds and a remainder.
2. **Drift:** the performance counter may stop while the PC sleeps. `pc_os_resync_clock()` pulls the clock back onto the PC's when it's more than 2 seconds off. It runs every minute (`pc_wallpaper_update`) and on `WM_POWERBROADCAST` resume.

The game clock is **PC clock + offset**. The `--time`/`--date` options and the custom clock in the settings are both stored as an offset, so an overridden clock keeps ticking.

**Using real time in the demo:** the title demo normally pins each demo to a fixed day, hour and weather (`mTM_demotime_set`: April 6, June 16, August 1, November 1, February 1) and stops the clock. In wallpaper mode `mTM_demotime_set` reads the real clock and keeps it running. `mEnv_NowWeather` and the weather actor's constructor skip their per-demo weather tables, so the town's own weather is used.

## The desktop widget

- **Rendering:** the widget draws itself on the CPU into a premultiplied 32-bit bitmap, shown with `UpdateLayeredWindow`.
  - **Shapes:** anti-aliased with 4×4 supersampling, using coverage functions for rounded rectangles and icons.
  - **Text:** GDI draws it white-on-black into a mask, which is then tinted.
  - **Look:** colours and shapes are taken from `m_banti.c`, the in-game clock: lime date ovals with a brown rim, a red (Sunday) or brown weekday badge, white time coins with a flip-card seam, the orange colon blinking with the seconds, and a dark am/pm badge.
- **Window:** a top-level `WS_POPUP` tool window, **owned by** the desktop host (Progman on 24H2). An owned window always stays above its owner, so it sits right on the desktop, under every normal window. Transparent pixels let clicks through.
  > A *child* of Progman was tried first. 24H2's desktop composition accepted the image (`UpdateLayeredWindow` succeeded) but never showed it.
- **Layout:** the Play pill sits left of the time, as tall as the time coins. Under the time is the drawer:
  - **Closed:** a thin bar in the am/pm badge's dark brown.
  - **Hovered:** it thickens and fades to the buttons' tan, with a down chevron.
  - **Open (click):** it becomes a tan panel holding rotate, zoom, reset and settings.
  - **Animation:** `s_hover_t` and `s_open_t` ease on a 15 ms timer; `wg_get_layout` turns them into positions. The window is sized for the open drawer and anchored at its bottom edge, so opening pushes the clock and calendar up. A nearly transparent pad (alpha 3/255) under the thin bar makes the hover target taller, since layered windows ignore the mouse over fully transparent pixels.
- **Two frames** (`wg_frame_def`):
  - **With the clock:** 266 units wide. The drawer holds seven buttons in one row.
  - **Hide the clock:** 106 units wide. Only Play and a settings badge, with a 62-unit drawer holding two rows of three.
  - **Shared code:** `wg_btn_center` gives every button's position for drawing, hit testing and tooltips.
- **Music button:** toggles `music_muted` and sets `g_pc_wallpaper_volume`, the idle scale in `pc_audio.c`, to 0 or `music_volume`. `--wallpaper-volume` only sets the starting state; `wallpaper.ini` overrides it.
- **Position** (`wg_target_rect`): a corner or edge middle of the primary monitor's work area, 16 px in, or a free offset from the work area's top-left. The rect is clamped on screen, which covers resolution changes. Along the top edge, `wg_get_layout` pins the clock and the drawer opens downward.
- **Free placement** (`wg_begin_placing`): the widget turns `HWND_TOPMOST`, closes its drawer, and draws a translucent tan sheet with a dashed border, so every pixel can be grabbed. Accept and Cancel sit in the empty top-left corner.
  - **Drag:** dragging moves the window by the cursor's delta from where the drag began.
  - **Accept or Cancel:** Accept saves `widget_position = free` and `widget_free_x/y`. Cancel, or switching to play, restores the previous position. Either one drops the widget back to `HWND_BOTTOM`.
- **Tooltips:** one tracking tooltip (`TTF_TRACK | TTF_ABSOLUTE`), shown 450 ms after the pointer settles on a button and placed above it by hand, kept on screen. The widget never has focus, which rules out automatic tooltips.
- **Redraw:** a 250 ms timer redraws only when the minute, seconds parity, hover or press state changes. Animation frames always redraw. Rotate and zoom repeat while held.
- **Settings dialog** (`wg_open_settings`): radio buttons for the PC or a custom clock, date and time pickers, clock size, clock position (with **Move...** for free placement) and camera speed. Applying it saves `wallpaper.ini` (`clock_offset_seconds`, `widget_scale`, `widget_position`, `widget_free_x/y`, `camera_speed`). Only a change to the clock requests a scene reload.

An earlier attempt drew the widget inside the game, using the real `m_banti` clock models. It was dropped: the emulated GX path maps 2D in widescreen differently from what the UI code assumes, and loading custom matrices partway through the font display list doesn't take reliably.

## Game-code changes, file by file

| File | Change | Why |
|---|---|---|
| `src/game/m_camera2.c` | `Camera2_pc_idle_process` runs before the camera state machine | The idle camera |
| `src/game/m_titledemo.c` | Autopilot instead of recorded input; frozen timer; scene reload; scene-start hook | Endless attract mode |
| `src/game/m_trademark.c` | `mTM_demotime_set` uses the real clock in wallpaper mode | Real time and seasons |
| `src/game/m_kankyo_weather.c_inc` | `mEnv_NowWeather` skips demo weather in wallpaper mode | Real weather |
| `src/actor/ac_weather.c` | No demo weather table in wallpaper mode; idle area scale, spawn multiplier, widescreen check | Real, screen-wide weather |
| `src/actor/ac_weather_{leaf,sakura,snow,rain}.c` | Spawn and wrap bounds use `aWEATHER_AREA_X/Z`; widescreen draw check | Screen-wide weather |
| `include/ac_weather.h` | Pool 600 on PC; `u16` particle id; area macros | Bigger weather pool |
| `src/game/m_player.c` | `Player_actor_draw` skipped while idle | Hidden walker |
| `src/game/m_player_main_door.c_inc` | Door requests refused while idle | Walker stays outdoors |
| `src/actor/ac_animal_logo.c` | Logo and menu not drawn while idle; *Quit Game* becomes *Return to Desktop* in wallpaper mode | Clean wallpaper; don't close the program from the title menu |
| `include/m_field_make.h` | `mFM_VISIBLE_BLOCK_NUM` 9 on PC | 3×3 loading |
| `src/game/m_field_make.c` | Title demo gets 9 BG slots in wallpaper mode | 3×3 loading |
| `src/game/m_field_info.c` | 3×3 slot selection; slot bound; `u16` bitfields; item-table row fix | 3×3 loading, vanishing trees |
| `include/m_common_data.h`, `src/bg_item/bg_item_common.c_inc` | `remove_cut_tree_info_bitfield` widened to `u16` | 9 slots |
| `src/actor/ac_birth_control.c` | Spawn all 8 neighbours with 9 slots | Buildings at the edges |
| `include/ac_field_draw.h`, `src/actor/ac_field_draw.c` | 5×5 ground with 9 slots | No void within the town |
| `src/game/m_actor.c` | Larger cull depth while idle | Distant actors draw and move |
| `src/actor/ac_train0_move.c_inc` | Train tolerates one row off while idle | Train visible from further away |
| `include/sys_dynamic.h` | Bigger per-frame display-list buffers on PC | See below |

PC layer: `pc_main.c` (flags, hidden startup window, input gating, init/shutdown hooks), `pc_pause_menu.c` (*Quit Game* becomes *Return to Desktop* in wallpaper mode), `pc_pad.c` (neutral input while idle), `pc_audio.c` (idle volume), `pc_os.c` (clock), `pc_platform.h` (clock API), `CMakeLists.txt` (new sources; links `shell32`, `comctl32`).

## Changes outside wallpaper mode

These apply on PC with or without `--wallpaper`:

- **Clock** (`pc_os.c`): the overflow fix, and `--time`/`--date` stored as an offset so the clock keeps running. The resync only runs in wallpaper mode.
- **Display-list buffers** (`sys_dynamic.h`): `POLY_OPA` 9952 → 65536, `POLY_XLU` 2048 → 16384, `FONT` 1792 → 4096 commands. `GRAPH_ALLOC` (per-frame matrices and vertices) carves its space out of the tail of `poly_opa`, and nothing stops an overflow from corrupting memory. The bigger weather pool overran it, which showed up as a frozen black screen.
- **Array sizes:** arrays sized by `mFM_VISIBLE_BLOCK_NUM` are larger, but other scenes still use only their own slot count.
- **Manifest** (`app.manifest`): Windows 8+ support and Common Controls v6, needed for the settings dialog's themed controls.

## Troubleshooting

| Symptom | What to check |
|---|---|
| The game appears as a normal window instead of the wallpaper | Explorer's desktop host wasn't found. Restarting Explorer, or the game, usually fixes it. Check with `--verbose`. |
| No clock widget | It sits just above the desktop, under all windows. Press <kbd>Win</kbd>+<kbd>D</kbd> to see it. |
| The hotkeys do nothing | Another program may own <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>P</kbd> or <kbd>W</kbd> (logged with `--verbose`). Use the widget or the tray menu. |
| The wallpaper shows the wrong time | Open ⚙ and choose *Use this PC's date and time*, or delete `wallpaper.ini`. |
| Black corners at the edge of the map | The bend allowance (`IDLE_BEND_REACH` in `pc_idle.c`) is approximate; raise it a little. |

## Ideas for later

- Save when returning to the wallpaper (run the game's own save, then reset to the title screen).
- A camera "director" that seeks out events (K.K. Slider, fireworks, the Bug-Off) and villager conversations.
- Pause or throttle rendering while a fullscreen app is in front or on battery power.
- Multi-monitor support.
