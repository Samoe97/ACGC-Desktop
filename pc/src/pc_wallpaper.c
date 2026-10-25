/* pc_wallpaper.c - desktop wallpaper mode (Windows only)
 *
 * Idle: the SDL window is reparented behind the desktop icons, the same trick
 * Wallpaper Engine / Lively use. Explorer spawns a WorkerW window between the
 * icons (SHELLDLL_DefView) and the static wallpaper when Progman receives the
 * undocumented 0x052C message; we become a child of the right window there.
 *   - pre-24H2: the WorkerW is a top-level window right after the one that
 *     hosts SHELLDLL_DefView, and we parent into it.
 *   - 24H2+:    the WorkerW is a child of Progman. We parent into Progman and
 *     sit in z-order between SHELLDLL_DefView (above) and WorkerW (below).
 *
 * Play: the window goes back to a normal top-level borderless fullscreen
 * window with focus and input.
 *
 * Switching is driven by global hotkeys (RegisterHotKey works without focus,
 * which the wallpaper never has) and a tray icon. Both live on a hidden window
 * owned by the main thread, so SDL's event pump dispatches their messages. */
#include "pc_platform.h"
#include "pc_wallpaper.h"

int g_pc_wallpaper_enabled = 0;
int g_pc_wallpaper_active = 0;
int g_pc_wallpaper_volume = 0;

#ifdef _WIN32
#include <shellapi.h>
#include <SDL_syswm.h>

#define WP_HOTKEY_PLAY      1
#define WP_HOTKEY_WALLPAPER 2
#define WP_TRAY_ID          1
#define WP_WM_TRAY          (WM_APP + 1)
#define WP_MENU_PLAY        100
#define WP_MENU_WALLPAPER   101
#define WP_MENU_QUIT        102

enum { WP_REQ_NONE, WP_REQ_PLAY, WP_REQ_WALLPAPER };

static HWND  s_game_hwnd = NULL;
static HWND  s_msg_hwnd = NULL;
static UINT  s_taskbar_created_msg = 0;
static int   s_request = WP_REQ_NONE;
static int   s_tray_added = 0;
static LONG  s_saved_style = 0;
static LONG  s_saved_exstyle = 0;
static RECT  s_saved_rect;
static HWND  s_prev_foreground = NULL; /* what the user was in before Play */

static void wp_log(const char* msg) {
    if (g_pc_verbose) printf("[WALLPAPER] %s\n", msg);
}

/* ---- finding the desktop host ---- */

static BOOL CALLBACK wp_find_legacy_worker(HWND top, LPARAM out) {
    if (FindWindowExW(top, NULL, L"SHELLDLL_DefView", NULL) != NULL) {
        /* the WorkerW that follows the icon host is the empty one behind it */
        *(HWND*)out = FindWindowExW(NULL, top, L"WorkerW", NULL);
        return FALSE;
    }
    return TRUE;
}

/* Returns the window to parent into. *icons_above is set (24H2+) to the icon
 * view we must stay below, NULL when the host already sits under the icons. */
static HWND wp_find_desktop_host(HWND* icons_above) {
    HWND progman = FindWindowW(L"Progman", NULL);
    HWND defview, worker = NULL;
    DWORD_PTR result;

    *icons_above = NULL;
    if (progman == NULL) return NULL;

    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);

    defview = FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL);
    if (defview != NULL && FindWindowExW(progman, NULL, L"WorkerW", NULL) != NULL) {
        *icons_above = defview;
        return progman;
    }

    EnumWindows(wp_find_legacy_worker, (LPARAM)&worker);
    return worker;
}

/* ---- mode switches ---- */

static void wp_enter_wallpaper(void) {
    HWND icons_above;
    HWND host = wp_find_desktop_host(&icons_above);
    HMONITOR mon;
    MONITORINFO mi;
    POINT origin = { 0, 0 };

    if (host == NULL) {
        wp_log("no desktop host window found; staying in play mode");
        SDL_ShowWindow(g_pc_window); /* created hidden; don't leave it invisible */
        return;
    }

    /* leave SDL's fullscreen first so it isn't fighting the reparent */
    SDL_SetWindowFullscreen(g_pc_window, 0);

    if (!g_pc_wallpaper_active) {
        s_saved_style = GetWindowLongW(s_game_hwnd, GWL_STYLE);
        s_saved_exstyle = GetWindowLongW(s_game_hwnd, GWL_EXSTYLE);
        GetWindowRect(s_game_hwnd, &s_saved_rect);
    }

    SetWindowLongW(s_game_hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    SetWindowLongW(s_game_hwnd, GWL_EXSTYLE, s_saved_exstyle & ~(WS_EX_APPWINDOW | WS_EX_TOPMOST));
    SetParent(s_game_hwnd, host);
    /* --wallpaper creates the window hidden so it never flashes on screen;
     * it's a child of the desktop now, so showing it can't steal focus */
    SDL_ShowWindow(g_pc_window);

    /* cover the primary monitor, in the host's client coordinates */
    mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    MapWindowPoints(NULL, host, (POINT*)&mi.rcMonitor, 2);

    SetWindowPos(s_game_hwnd, icons_above != NULL ? icons_above : HWND_TOP,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    if (icons_above != NULL) {
        /* 24H2+: keep the static wallpaper renderer below us */
        HWND worker = FindWindowExW(host, NULL, L"WorkerW", NULL);
        if (worker != NULL) {
            SetWindowPos(worker, s_game_hwnd, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    /* give focus back to what the user had before Play, else the desktop */
    if (GetForegroundWindow() == s_game_hwnd || GetForegroundWindow() == s_msg_hwnd) {
        HWND back = (s_prev_foreground != NULL && IsWindow(s_prev_foreground))
                        ? s_prev_foreground : GetShellWindow();
        if (back != NULL) SetForegroundWindow(back);
    }
    s_prev_foreground = NULL;

    g_pc_wallpaper_active = 1;
    pc_platform_update_window_size();
    wp_log(icons_above != NULL ? "attached to Progman (24H2+ layout)" : "attached to WorkerW");
}

static void wp_enter_play(void) {
    HWND fg = GetForegroundWindow();

    if (fg != s_game_hwnd && fg != s_msg_hwnd) s_prev_foreground = fg;

    if (g_pc_wallpaper_active) {
        SetParent(s_game_hwnd, NULL);
        SetWindowLongW(s_game_hwnd, GWL_STYLE, s_saved_style);
        SetWindowLongW(s_game_hwnd, GWL_EXSTYLE, s_saved_exstyle);
        SetWindowPos(s_game_hwnd, HWND_TOP, s_saved_rect.left, s_saved_rect.top,
                     s_saved_rect.right - s_saved_rect.left,
                     s_saved_rect.bottom - s_saved_rect.top,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    }

    g_pc_wallpaper_active = 0;
    SDL_SetWindowFullscreen(g_pc_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_RaiseWindow(g_pc_window);
    SetForegroundWindow(s_game_hwnd);
    pc_platform_update_window_size();
    wp_log("play mode");
}

/* ---- tray icon ---- */

static void wp_tray_add(void) {
    NOTIFYICONDATAW nid;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = s_msg_hwnd;
    nid.uID = WP_TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WP_WM_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (nid.hIcon == NULL) nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcscpy(nid.szTip, L"Animal Crossing wallpaper (Ctrl+Alt+P play, Ctrl+Alt+W return)");
    s_tray_added = Shell_NotifyIconW(NIM_ADD, &nid);
}

static void wp_tray_remove(void) {
    NOTIFYICONDATAW nid;

    if (!s_tray_added) return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = s_msg_hwnd;
    nid.uID = WP_TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    s_tray_added = 0;
}

static void wp_tray_menu(void) {
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int cmd;

    AppendMenuW(menu, MF_STRING | (g_pc_wallpaper_active ? 0 : MF_GRAYED),
                WP_MENU_PLAY, L"Play\tCtrl+Alt+P");
    AppendMenuW(menu, MF_STRING | (g_pc_wallpaper_active ? MF_GRAYED : 0),
                WP_MENU_WALLPAPER, L"Return to wallpaper\tCtrl+Alt+W");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, WP_MENU_QUIT, L"Quit");

    GetCursorPos(&pt);
    SetForegroundWindow(s_msg_hwnd); /* required so the menu closes on click-away */
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                         pt.x, pt.y, 0, s_msg_hwnd, NULL);
    DestroyMenu(menu);

    switch (cmd) {
        case WP_MENU_PLAY:      pc_wallpaper_request_play(); break;
        case WP_MENU_WALLPAPER: pc_wallpaper_request_wallpaper(); break;
        case WP_MENU_QUIT:      g_pc_running = 0; break;
    }
}

static LRESULT CALLBACK wp_msg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_HOTKEY) {
        if (wparam == WP_HOTKEY_PLAY) pc_wallpaper_request_play();
        else if (wparam == WP_HOTKEY_WALLPAPER) pc_wallpaper_request_wallpaper();
        return 0;
    }
    if (msg == WP_WM_TRAY) {
        switch (LOWORD(lparam)) {
            case WM_LBUTTONDBLCLK:
                if (g_pc_wallpaper_active) pc_wallpaper_request_play();
                else pc_wallpaper_request_wallpaper();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                wp_tray_menu();
                break;
        }
        return 0;
    }
    if (s_taskbar_created_msg != 0 && msg == s_taskbar_created_msg) {
        /* Explorer restarted: the tray and the desktop host windows are new */
        s_tray_added = 0;
        wp_tray_add();
        if (g_pc_wallpaper_active) s_request = WP_REQ_WALLPAPER;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ---- public API ---- */

void pc_wallpaper_request_play(void) {
    if (g_pc_wallpaper_enabled) s_request = WP_REQ_PLAY;
}

void pc_wallpaper_request_wallpaper(void) {
    if (g_pc_wallpaper_enabled) s_request = WP_REQ_WALLPAPER;
}

void pc_wallpaper_init(void) {
    SDL_SysWMinfo info;
    WNDCLASSW wc;

    if (!g_pc_wallpaper_enabled) return;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(g_pc_window, &info)) {
        wp_log("SDL_GetWindowWMInfo failed; wallpaper mode disabled");
        g_pc_wallpaper_enabled = 0;
        return;
    }
    s_game_hwnd = info.info.win.window;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wp_msg_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ACWallpaperMsg";
    RegisterClassW(&wc);
    /* a hidden top-level window, not HWND_MESSAGE: message-only windows don't
     * get the TaskbarCreated broadcast */
    s_msg_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                 NULL, NULL, wc.hInstance, NULL);
    s_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

    if (!RegisterHotKey(s_msg_hwnd, WP_HOTKEY_PLAY, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P')) {
        wp_log("Ctrl+Alt+P is taken by another program");
    }
    if (!RegisterHotKey(s_msg_hwnd, WP_HOTKEY_WALLPAPER, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W')) {
        wp_log("Ctrl+Alt+W is taken by another program");
    }
    wp_tray_add();

    wp_enter_wallpaper();
}

void pc_wallpaper_update(void) {
    int req = s_request;

    if (!g_pc_wallpaper_enabled || req == WP_REQ_NONE) return;
    s_request = WP_REQ_NONE;

    if (req == WP_REQ_PLAY && g_pc_wallpaper_active) {
        wp_enter_play();
    } else if (req == WP_REQ_WALLPAPER) {
        /* also handles re-attaching after an Explorer restart */
        wp_enter_wallpaper();
    }
}

void pc_wallpaper_shutdown(void) {
    if (!g_pc_wallpaper_enabled) return;

    if (g_pc_wallpaper_active) {
        /* hand the window back so Explorer doesn't keep a dead child around */
        SetParent(s_game_hwnd, NULL);
        g_pc_wallpaper_active = 0;
    }
    wp_tray_remove();
    UnregisterHotKey(s_msg_hwnd, WP_HOTKEY_PLAY);
    UnregisterHotKey(s_msg_hwnd, WP_HOTKEY_WALLPAPER);
    if (s_msg_hwnd != NULL) {
        DestroyWindow(s_msg_hwnd);
        s_msg_hwnd = NULL;
    }
    /* Re-apply the user's current wallpaper (not persisted) so the desktop
     * repaints over our last frame. NULL here would reset it to the default. */
    {
        WCHAR path[MAX_PATH];
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0)) {
            SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, 0);
        }
    }
}

#else /* !_WIN32 */

void pc_wallpaper_init(void) { g_pc_wallpaper_enabled = 0; }
void pc_wallpaper_shutdown(void) {}
void pc_wallpaper_update(void) {}
void pc_wallpaper_request_play(void) {}
void pc_wallpaper_request_wallpaper(void) {}

#endif
