/* pc_widget.c - wallpaper-mode desktop widget (Windows only)
 *
 * A small panel in the bottom-right of the desktop drawn in the style of the
 * game's own clock (m_banti): lime date ovals and a weekday badge, white time
 * coins with blinking orange colon dots and an am/pm badge, each with a dark
 * brown rim and a flip-card seam. A green Play pill sits left of the time.
 * Under the time is a drawer: a thin brown bar that thickens and turns tan
 * on hover, and on click slides open (pushing the clock up) to show the
 * camera controls and settings as cream badges with brown icons. Every
 * button has a tooltip.
 *
 * The wallpaper sits under the desktop icons, so this is a separate window:
 * a per-pixel-alpha window owned by the window hosting the icon view, which
 * keeps it just above the desktop and under normal windows. Transparent
 * pixels let clicks through to the desktop.
 * Shapes are anti-aliased by supersampling; text is rasterized by GDI into a
 * coverage mask and tinted.
 *
 * The settings button opens a small dialog for the town clock: the PC's date
 * and time, or a custom one (kept as an offset, so it keeps ticking), saved
 * to wallpaper.ini next to the executable. */
#include "pc_platform.h"
#include "pc_widget.h"
#include "pc_wallpaper.h"
#include "pc_idle.h"

#ifdef _WIN32
#include <commctrl.h>

/* layout, in 96-DPI units. The window is sized for the open drawer and its
 * bottom edge is the anchor, so opening the drawer pushes the clock up; the
 * unused space above is transparent (clicks go through to the desktop).
 * With the clock hidden the window is narrower (see wg_frame). */
#define WG_H      130
#define WG_MARGIN 16    /* from the work area's bottom-right corner */

#define WG_TIMER_TICK   1
#define WG_TIMER_REPEAT 2
#define WG_TIMER_ANIM   3
#define WG_TIMER_TIP    4
#define WG_TICK_MS         250
#define WG_REPEAT_DELAY_MS 350
#define WG_REPEAT_MS       90
#define WG_ANIM_MS         15
#define WG_TIP_DELAY_MS    450

#define WG_ROTATE_STEP 5.0f   /* degrees per click/repeat */
#define WG_ZOOM_STEP   0.05f  /* zoom level per click/repeat (0 = gameplay camera, 1 = default) */

#define WG_INI_NAME "wallpaper.ini"

enum {
    WG_BTN_NONE = -1,
    /* in the drawer */
    WG_BTN_ROT_L,
    WG_BTN_ROT_R,
    WG_BTN_ZOOM_OUT,
    WG_BTN_ZOOM_IN,
    WG_BTN_RESET,
    WG_BTN_SOUND,
    WG_BTN_SETTINGS, /* in the drawer, or beside Play when the clock is hidden */
    WG_BTN_PLAY,
    WG_BTN_DRAWER, /* the drawer itself: click to open/close */
    WG_BTN_ACCEPT, /* free placement */
    WG_BTN_CANCEL,
    WG_BTN_NUM
};

/* Two arrangements. With the clock: Play left of the time, and the drawer
 * under the time with its buttons in one row. Without: just Play and a
 * settings badge, with a narrow drawer under them holding two rows. */
typedef struct {
    f32 w;                    /* window width */
    f32 play_cx, gear_cx;     /* gear_cx < 0: no settings badge in the top row */
    f32 drawer_x0, drawer_x1;
    f32 open_h;               /* drawer height when open */
    f32 top_drawer_top;       /* drawer top when anchored to the top edge */
    int cols;                 /* drawer buttons per row */
    f32 btn_x0;               /* first drawer button's center */
    int nbtn;
    int btns[7];
} wg_frame_def;

static const wg_frame_def s_frame_clock = {
    266, 34, -1, 74, 258, 38, 87, 7, 91, 7,
    { WG_BTN_ROT_L, WG_BTN_ROT_R, WG_BTN_ZOOM_OUT, WG_BTN_ZOOM_IN, WG_BTN_RESET, WG_BTN_SOUND, WG_BTN_SETTINGS },
};
static const wg_frame_def s_frame_compact = {
    106, 37, 87, 3, 103, 62, 41, 3, 28, 6,
    { WG_BTN_ROT_L, WG_BTN_ROT_R, WG_BTN_RESET, WG_BTN_ZOOM_OUT, WG_BTN_ZOOM_IN, WG_BTN_SOUND },
};

#define WG_PLAY_HW 30.0f
#define WG_TIME_R  15.0f   /* time coins; Play and the settings badge match */

/* the drawer's height animates from a thin bar (closed), to a thicker one
 * (hovered), to the open panel */
#define WG_DRAWER_THIN  5.0f
#define WG_DRAWER_HOVER 12.0f
#define WG_BTN_STEP 25.0f
#define WG_BTN_R    11.0f

/* colors from m_banti's prim/env colors */
#define RGBc(r, g, b) ((u32)(((r) << 16) | ((g) << 8) | (b)))
#define COL_DATE_FILL  RGBc(235, 255, 120)
#define COL_DATE_RIM   RGBc(80, 40, 40)
#define COL_TIME_FILL  RGBc(255, 255, 255)
#define COL_TIME_RIM   RGBc(60, 25, 10)
#define COL_WEEK_SUN   RGBc(165, 30, 30)
#define COL_WEEK_DAY   RGBc(80, 40, 0)
#define COL_WEEK_TEXT  RGBc(255, 255, 225)
#define COL_AMPM_FILL  RGBc(70, 10, 10)
#define COL_AMPM_TEXT  RGBc(255, 255, 120)
#define COL_DOT        RGBc(255, 230, 0)
#define COL_COLON      RGBc(215, 120, 0)
#define COL_DOT_RIM    RGBc(70, 50, 50)
#define COL_BTN_FILL   RGBc(255, 250, 215)
#define COL_BTN_HOT    RGBc(255, 240, 150)
#define COL_BTN_RIM    RGBc(80, 40, 0)
#define COL_BTN_ICON   RGBc(90, 50, 20)
#define COL_PLAY_FILL  RGBc(120, 200, 80)
#define COL_PLAY_HOT   RGBc(150, 225, 95)
#define COL_PLAY_RIM   RGBc(40, 70, 20)
#define COL_WHITE      RGBc(255, 255, 255)
#define COL_BLACK      RGBc(0, 0, 0)

static HWND s_hwnd = NULL;
static HWND s_parent = NULL;
static HWND s_settings = NULL;
static int  s_visible = FALSE;
static int  s_dpi = 96;
static int  s_hot = WG_BTN_NONE;
static int  s_down = WG_BTN_NONE;
static int  s_tracking = FALSE;
static int  s_drawn_key = -1; /* what's on screen, to skip identical redraws */
static HFONT s_font_digits = NULL, s_font_label = NULL;
static f32  s_scale = 1.2f;   /* the "Clock size" setting */
static int  s_open = FALSE;   /* drawer open (target of the animation) */
static f32  s_open_t = 0.0f, s_hover_t = 0.0f; /* animated 0..1 */
static DWORD s_anim_last = 0;
static f32  s_paint_alpha = 1.0f; /* multiplies everything drawn (fading the drawer's buttons) */
static HWND s_tip = NULL;
static int  s_tip_for = WG_BTN_NONE;
static int  s_hide_clock = FALSE; /* only Play and settings, no date/time */
static int  s_muted = TRUE;       /* the sound button: idle audio off */
static int  s_music_volume = 50;  /* idle volume when not muted, % of master */

static const wg_frame_def* wg_frame(void) {
    return s_hide_clock ? &s_frame_compact : &s_frame_clock;
}

static void wg_apply_volume(void) {
    g_pc_wallpaper_volume = s_muted ? 0 : s_music_volume;
}

/* "Clock position": a corner, the middle of an edge, or free (dragged into
 * place; stored as the window's offset from the work area's top-left) */
enum {
    WG_POS_TOP_LEFT,
    WG_POS_TOP_CENTER,
    WG_POS_TOP_RIGHT,
    WG_POS_LEFT_CENTER,
    WG_POS_RIGHT_CENTER,
    WG_POS_BOTTOM_LEFT,
    WG_POS_BOTTOM_CENTER,
    WG_POS_BOTTOM_RIGHT,
    WG_POS_FREE,
    WG_POS_CHOICES
};
static const char* s_pos_keys[WG_POS_CHOICES] = { "top-left",     "top-center",    "top-right",
                                                  "left-center",  "right-center",  "bottom-left",
                                                  "bottom-center", "bottom-right", "free" };
static const WCHAR* s_pos_names[WG_POS_CHOICES] = { L"Top left",     L"Top center",    L"Top right",
                                                    L"Left center",  L"Right center",  L"Bottom left",
                                                    L"Bottom center", L"Bottom right", L"Free place (drag it)" };
static const int s_pos_col[WG_POS_CHOICES] = { 0, 1, 2, 0, 2, 0, 1, 2, 0 }; /* 0 left, 1 center, 2 right */
static const int s_pos_row[WG_POS_CHOICES] = { 0, 0, 0, 1, 1, 2, 2, 2, 2 }; /* 0 top, 1 middle, 2 bottom */
static int s_pos = WG_POS_BOTTOM_RIGHT;
static int s_free_x = 0, s_free_y = 0;

/* free placement: the widget floats above everything with Accept/Cancel
 * until the user accepts; Cancel restores the saved position */
static int s_placing = FALSE;
static int s_saved_pos, s_saved_free_x, s_saved_free_y;
static int s_dragging = FALSE;
static POINT s_drag_cursor, s_drag_origin; /* cursor and free offset when the drag began */

static const f32 s_scale_choices[] = { 0.8f, 1.0f, 1.2f, 1.5f };
static const WCHAR* s_scale_names[] = { L"Small", L"Normal", L"Large", L"Extra large" };
#define WG_SCALE_CHOICES 4

/* "Camera speed" choices (pc_idle_set_speed); 1.0 was the original pace */
static const f32 s_speed_choices[] = { 0.35f, 0.6f, 1.0f, 1.5f };
static const WCHAR* s_speed_names[] = { L"Very slow", L"Slow", L"Normal", L"Fast" };
#define WG_SPEED_CHOICES 4

static void wg_open_settings(void);

/* ---- settings file ---- */

static void wg_ini_path(char* out, int size) {
    char* base = SDL_GetBasePath();
    snprintf(out, size, "%s%s", base ? base : "", WG_INI_NAME);
    if (base) SDL_free(base);
}

void pc_widget_load_settings(void) {
    char path[MAX_PATH];
    char line[256];
    FILE* f;

    /* --wallpaper-volume sets the starting sound; the ini overrides it */
    s_muted = g_pc_wallpaper_volume == 0;
    if (g_pc_wallpaper_volume > 0) s_music_volume = g_pc_wallpaper_volume;

    wg_ini_path(path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f)) {
        long long v;
        float scale, speed;
        char word[32];
        int i, n;
        if (sscanf(line, " music_muted = %d", &n) == 1) {
            s_muted = n != 0;
        } else if (sscanf(line, " music_volume = %d", &n) == 1 && n >= 1 && n <= 100) {
            s_music_volume = n;
        } else if (sscanf(line, " hide_clock = %d", &n) == 1) {
            s_hide_clock = n != 0;
        } else if (sscanf(line, " widget_position = %31s", word) == 1) {
            for (i = 0; i < WG_POS_CHOICES; i++) {
                if (strcmp(word, s_pos_keys[i]) == 0) s_pos = i;
            }
        } else if (sscanf(line, " widget_free_x = %d", &n) == 1) {
            s_free_x = n;
        } else if (sscanf(line, " widget_free_y = %d", &n) == 1) {
            s_free_y = n;
        } else if (sscanf(line, " clock_offset_seconds = %lld", &v) == 1) {
            pc_os_set_clock_offset((s64)v);
        } else if (sscanf(line, " widget_scale = %f", &scale) == 1 && scale >= 0.5f && scale <= 2.0f) {
            s_scale = scale;
        } else if (sscanf(line, " camera_speed = %f", &speed) == 1) {
            pc_idle_set_speed(speed);
        }
    }
    fclose(f);
    wg_apply_volume();
}

static void wg_save_settings(void) {
    char path[MAX_PATH];
    FILE* f;

    wg_ini_path(path, sizeof(path));
    f = fopen(path, "w");
    if (f == NULL) return;
    fprintf(f, "# Animal Crossing desktop wallpaper\n");
    fprintf(f, "# Town clock = this PC's clock + clock_offset_seconds (0 = the PC's date and time)\n");
    fprintf(f, "clock_offset_seconds = %lld\n", (long long)pc_os_get_clock_offset());
    fprintf(f, "# Size of the clock/controls widget (1.0 = normal)\n");
    fprintf(f, "widget_scale = %.2f\n", s_scale);
    fprintf(f, "# Where the widget sits: top-left, top-center, top-right, left-center, right-center,\n");
    fprintf(f, "# bottom-left, bottom-center, bottom-right, or free (at widget_free_x/y pixels from the\n");
    fprintf(f, "# top-left of the screen's work area)\n");
    fprintf(f, "widget_position = %s\n", s_pos_keys[s_pos]);
    fprintf(f, "widget_free_x = %d\n", s_free_x);
    fprintf(f, "widget_free_y = %d\n", s_free_y);
    fprintf(f, "# 1 = hide the date and time, leaving Play, settings and the drawer\n");
    fprintf(f, "hide_clock = %d\n", s_hide_clock);
    fprintf(f, "# Camera/walker pace while idle (1.0 = original pace; Slow = 0.6)\n");
    fprintf(f, "camera_speed = %.2f\n", pc_idle_get_speed());
    fprintf(f, "# Music and sound while idle: the widget's sound button toggles music_muted;\n");
    fprintf(f, "# music_volume is the level when on, as %% of the game's master volume\n");
    fprintf(f, "music_muted = %d\n", s_muted);
    fprintf(f, "music_volume = %d\n", s_music_volume);
    fclose(f);
}

/* ---- canvas: premultiplied BGRA, top-down ---- */

typedef struct {
    u32* px;
    int w, h;
    HDC dc;
    HBITMAP bmp;
} wg_canvas;

#define WG_SS 4 /* supersamples per axis */

/* layout units -> pixels: DPI-aware times the user's size setting */
static f32 wg_s(f32 v) {
    return v * s_scale * (f32)s_dpi / 96.0f;
}

static void cv_blend(wg_canvas* cv, int x, int y, u32 rgb, f32 a) {
    u32* d;
    f32 ia;
    u32 dr, dg, db, da;

    a *= s_paint_alpha;
    if (a <= 0.0f || x < 0 || y < 0 || x >= cv->w || y >= cv->h) return;
    if (a > 1.0f) a = 1.0f;
    d = &cv->px[y * cv->w + x];
    ia = 1.0f - a;
    da = (*d >> 24) & 0xFF;
    dr = (*d >> 16) & 0xFF;
    dg = (*d >> 8) & 0xFF;
    db = *d & 0xFF;
    dr = (u32)(((rgb >> 16) & 0xFF) * a + dr * ia + 0.5f);
    dg = (u32)(((rgb >> 8) & 0xFF) * a + dg * ia + 0.5f);
    db = (u32)((rgb & 0xFF) * a + db * ia + 0.5f);
    da = (u32)(255.0f * a + da * ia + 0.5f);
    *d = (da << 24) | (dr << 16) | (dg << 8) | db;
}

/* coverage callback: is (x, y) (pixels) inside the shape? */
typedef int (*wg_inside_fn)(f32 x, f32 y, const f32* p);

static void cv_shape(wg_canvas* cv, wg_inside_fn inside, const f32* p, f32 x0, f32 y0, f32 x1, f32 y1, u32 rgb,
                     f32 alpha) {
    int x, y, sx, sy;
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0) - 1;
    int ix1 = (int)ceilf(x1) + 1, iy1 = (int)ceilf(y1) + 1;

    for (y = iy0; y <= iy1; y++) {
        for (x = ix0; x <= ix1; x++) {
            int hits = 0;
            for (sy = 0; sy < WG_SS; sy++) {
                for (sx = 0; sx < WG_SS; sx++) {
                    hits += inside(x + (sx + 0.5f) / WG_SS, y + (sy + 0.5f) / WG_SS, p);
                }
            }
            if (hits) cv_blend(cv, x, y, rgb, alpha * hits / (f32)(WG_SS * WG_SS));
        }
    }
}

/* p: cx, cy, half width, half height, corner radius (an ellipse-ish pill) */
static int in_rrect(f32 x, f32 y, const f32* p) {
    f32 dx = fabsf(x - p[0]) - (p[2] - p[4]);
    f32 dy = fabsf(y - p[1]) - (p[3] - p[4]);
    if (dx < 0.0f) dx = 0.0f;
    if (dy < 0.0f) dy = 0.0f;
    return dx * dx + dy * dy <= p[4] * p[4];
}

/* p: x0, y0, x1, y1 */
static int in_rect(f32 x, f32 y, const f32* p) {
    return x >= p[0] && x < p[2] && y >= p[1] && y < p[3];
}

static void cv_rrect(wg_canvas* cv, f32 cx, f32 cy, f32 hw, f32 hh, f32 r, u32 rgb, f32 a) {
    f32 p[5] = { cx, cy, hw, hh, r };
    cv_shape(cv, in_rrect, p, cx - hw, cy - hh, cx + hw, cy + hh, rgb, a);
}

static void cv_rect(wg_canvas* cv, f32 x0, f32 y0, f32 x1, f32 y1, u32 rgb, f32 a) {
    f32 p[4] = { x0, y0, x1, y1 };
    cv_shape(cv, in_rect, p, x0, y0, x1, y1, rgb, a);
}

/* A clock badge (a circle when hw == hh, else a pill): drop shadow, rim,
 * face, and optionally the flip-card seam across the middle. */
static void cv_badge(wg_canvas* cv, f32 cx, f32 cy, f32 hw, f32 hh, u32 fill, u32 rim, int seam) {
    f32 rimw = wg_s(2.2f);
    f32 r = hh;

    cv_rrect(cv, cx + wg_s(1.0f), cy + wg_s(2.0f), hw, hh, r, COL_BLACK, 0.30f);
    cv_rrect(cv, cx, cy, hw, hh, r, rim, 1.0f);
    cv_rrect(cv, cx, cy, hw - rimw, hh - rimw, r - rimw, fill, 1.0f);
    if (seam) {
        cv_rect(cv, cx - hw + rimw * 2.0f, cy - wg_s(0.6f), cx + hw - rimw * 2.0f, cy + wg_s(0.6f), rim, 0.3f);
    }
}

/* text: GDI draws white-on-black into a mask, which tints onto the canvas */
static void cv_text(wg_canvas* cv, const WCHAR* s, HFONT font, f32 cx, f32 cy, u32 rgb, f32 a) {
    int mw = (int)wg_s(70), mh = (int)wg_s(40);
    BITMAPINFO bi;
    u32* mask;
    HDC dc = CreateCompatibleDC(cv->dc);
    HBITMAP bmp;
    RECT rc = { 0, 0, mw, mh };
    int x, y;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = mw;
    bi.bmiHeader.biHeight = -mh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&mask, NULL, 0);
    if (bmp == NULL) {
        DeleteDC(dc);
        return;
    }
    SelectObject(dc, bmp);
    memset(mask, 0, (size_t)mw * mh * 4);
    SelectObject(dc, font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    for (y = 0; y < mh; y++) {
        for (x = 0; x < mw; x++) {
            u32 m = mask[y * mw + x] & 0xFF;
            if (m) cv_blend(cv, (int)(cx - mw * 0.5f) + x, (int)(cy - mh * 0.5f) + y, rgb, a * m / 255.0f);
        }
    }
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- icons (32x32 design box, center origin, y down) ---- */

enum {
    ICON_ROT_L,
    ICON_ROT_R,
    ICON_MINUS,
    ICON_PLUS,
    ICON_HOME,
    ICON_GEAR,
    ICON_PLAY,
    ICON_CHEVRON,
    ICON_SOUND_ON,
    ICON_SOUND_OFF
};

static f32 seg_dist(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
    f32 dx = bx - ax, dy = by - ay;
    f32 t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    dx = px - (ax + dx * t);
    dy = py - (ay + dy * t);
    return sqrtf(dx * dx + dy * dy);
}

static int in_triangle(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy) {
    f32 d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    f32 d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    f32 d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

static int icon_inside(f32 x, f32 y, int icon) {
    const f32 stroke = 1.9f;
    f32 r = sqrtf(x * x + y * y);

    switch (icon) {
        case ICON_ROT_L:
        case ICON_ROT_R: {
            /* 3/4 circle with the gap at the top, arrowhead on one end */
            f32 sx = (icon == ICON_ROT_R) ? -x : x;
            f32 ang = atan2f(y, sx);
            int on_arc = fabsf(r - 8.5f) < stroke && !(ang < -F_PI * 0.3f && ang > -F_PI * 0.75f);
            int head = in_triangle(sx, y, -12.5f, -6.5f, -3.0f, -12.0f, -3.5f, -1.5f);
            return on_arc || head;
        }
        case ICON_MINUS:
            return fabsf(y) < stroke && fabsf(x) < 9.0f;
        case ICON_PLUS:
            return (fabsf(y) < stroke && fabsf(x) < 9.0f) || (fabsf(x) < stroke && fabsf(y) < 9.0f);
        case ICON_HOME: {
            int roof = seg_dist(x, y, -10.0f, -0.5f, 0.0f, -10.0f) < stroke ||
                       seg_dist(x, y, 10.0f, -0.5f, 0.0f, -10.0f) < stroke;
            int walls = (fabsf(x + 6.5f) < stroke || fabsf(x - 6.5f) < stroke) && y > -4.0f && y < 9.5f;
            int base = fabsf(y - 9.0f) < stroke && fabsf(x) < 8.0f;
            return roof || walls || base;
        }
        case ICON_GEAR: {
            f32 ang = atan2f(y, x);
            f32 outer = cosf(ang * 8.0f) > 0.25f ? 11.0f : 8.2f;
            return r < outer && r > 3.8f;
        }
        case ICON_PLAY:
            return in_triangle(x, y, -5.5f, -9.0f, -5.5f, 9.0f, 9.5f, 0.0f);
        case ICON_SOUND_ON:
        case ICON_SOUND_OFF: {
            /* speaker, then sound waves or a cross */
            int body = x >= -12.0f && x <= -6.0f && fabsf(y) <= 3.8f;
            int cone = in_triangle(x, y, -6.0f, -3.8f, 0.0f, -9.5f, 0.0f, 9.5f) ||
                       in_triangle(x, y, -6.0f, -3.8f, 0.0f, 9.5f, -6.0f, 3.8f);
            if (body || cone) return TRUE;
            if (icon == ICON_SOUND_OFF) {
                return seg_dist(x, y, 4.5f, -5.0f, 12.5f, 5.0f) < stroke || seg_dist(x, y, 4.5f, 5.0f, 12.5f, -5.0f) < stroke;
            } else {
                f32 wr = sqrtf((x - 1.0f) * (x - 1.0f) + y * y);
                int in_fan = x > 2.0f && fabsf(atan2f(y, x - 1.0f)) < 0.85f;
                return in_fan && (fabsf(wr - 5.5f) < 1.5f || fabsf(wr - 10.5f) < 1.5f);
            }
        }
        case ICON_CHEVRON: /* pointing down */
            return seg_dist(x, y, -10.0f, -4.5f, 0.0f, 4.5f) < 3.0f || seg_dist(x, y, 10.0f, -4.5f, 0.0f, 4.5f) < 3.0f;
    }
    return FALSE;
}

/* p: cx, cy, scale (pixels per design unit), icon */
static int in_icon(f32 x, f32 y, const f32* p) {
    return icon_inside((x - p[0]) / p[2], (y - p[1]) / p[2], (int)p[3]);
}

static void cv_icon(wg_canvas* cv, int icon, f32 cx, f32 cy, f32 size, u32 rgb) {
    f32 p[4] = { cx, cy, size / 32.0f, (f32)icon };
    cv_shape(cv, in_icon, p, cx - size * 0.5f, cy - size * 0.5f, cx + size * 0.5f, cy + size * 0.5f, rgb, 1.0f);
}

/* ---- the panel ---- */

static void wg_draw_digit_coin(wg_canvas* cv, f32 cx, f32 cy, f32 r, int digit) {
    WCHAR s[2];

    s[0] = (WCHAR)(L'0' + digit);
    s[1] = 0;
    cv_badge(cv, cx, cy, r, r, COL_TIME_FILL, COL_TIME_RIM, TRUE);
    if (digit < 0) {
        /* m_banti shows a blank card (a dash) for a leading zero */
        cv_rect(cv, cx - r * 0.55f, cy - wg_s(1.2f), cx + r * 0.55f, cy + wg_s(1.2f), COL_TIME_RIM, 1.0f);
    } else {
        cv_text(cv, s, s_font_digits, cx, cy, COL_TIME_RIM, 1.0f);
    }
}

/* where things are right now (96-DPI units), from the drawer's animation */
typedef struct {
    f32 open, hover;          /* eased 0..1 */
    f32 drawer_top, drawer_h;
    f32 time_cy, date_cy;
} wg_layout;

static void wg_get_layout(wg_layout* l) {
    const wg_frame_def* fr = wg_frame();
    f32 o = s_open_t * s_open_t * (3.0f - 2.0f * s_open_t);
    f32 d = WG_DRAWER_THIN + (WG_DRAWER_HOVER - WG_DRAWER_THIN) * s_hover_t;

    d += (fr->open_h - d) * o;
    l->open = o;
    l->hover = s_hover_t;
    l->drawer_h = d;
    if (s_pos_row[s_pos] == 0 && !s_placing) {
        /* along the top edge the clock stays put and the drawer opens downward */
        l->drawer_top = fr->top_drawer_top;
    } else {
        /* the closed bar sits at the top of a strip as tall as the hovered
         * one, so hovering grows it downward and only opening moves the clock */
        l->drawer_top = WG_H - 5.0f - (d > WG_DRAWER_HOVER ? d : WG_DRAWER_HOVER);
    }
    l->time_cy = l->drawer_top - 21.0f;
    l->date_cy = l->time_cy - 42.0f;
}

/* where a button is right now (96-DPI units); FALSE if it isn't shown */
static int wg_btn_center(int btn, const wg_layout* l, f32* cx, f32* cy) {
    const wg_frame_def* fr = wg_frame();
    int i;

    if (btn == WG_BTN_PLAY || (btn == WG_BTN_SETTINGS && fr->gear_cx >= 0.0f)) {
        *cx = btn == WG_BTN_PLAY ? fr->play_cx : fr->gear_cx;
        *cy = l->time_cy;
        return TRUE;
    }
    for (i = 0; i < fr->nbtn; i++) {
        if (fr->btns[i] == btn) {
            int rows = (fr->nbtn + fr->cols - 1) / fr->cols;
            *cx = fr->btn_x0 + WG_BTN_STEP * (i % fr->cols);
            *cy = l->drawer_top + l->drawer_h * 0.5f + ((i / fr->cols) - (rows - 1) * 0.5f) * 24.0f;
            return TRUE;
        }
    }
    return FALSE;
}

static int wg_btn_icon(int btn) {
    switch (btn) {
        case WG_BTN_ROT_L:    return ICON_ROT_L;
        case WG_BTN_ROT_R:    return ICON_ROT_R;
        case WG_BTN_ZOOM_OUT: return ICON_MINUS;
        case WG_BTN_ZOOM_IN:  return ICON_PLUS;
        case WG_BTN_RESET:    return ICON_HOME;
        case WG_BTN_SOUND:    return s_muted ? ICON_SOUND_OFF : ICON_SOUND_ON;
    }
    return ICON_GEAR;
}

static u32 wg_mix(u32 a, u32 b, f32 t) {
    u32 r = (u32)(((a >> 16) & 0xFF) + ((f32)((b >> 16) & 0xFF) - ((a >> 16) & 0xFF)) * t + 0.5f);
    u32 g = (u32)(((a >> 8) & 0xFF) + ((f32)((b >> 8) & 0xFF) - ((a >> 8) & 0xFF)) * t + 0.5f);
    u32 bl = (u32)((a & 0xFF) + ((f32)(b & 0xFF) - (a & 0xFF)) * t + 0.5f);
    return RGBc(r, g, bl);
}

static void wg_render_drawer(wg_canvas* cv, const wg_layout* l) {
    const wg_frame_def* fr = wg_frame();
    f32 x0 = wg_s(fr->drawer_x0), x1 = wg_s(fr->drawer_x1);
    f32 top = wg_s(l->drawer_top), hh = wg_s(l->drawer_h) * 0.5f;
    f32 cx = (x0 + x1) * 0.5f, cy = top + hh, hw = (x1 - x0) * 0.5f;
    f32 r = hh < wg_s(10) ? hh : wg_s(10);
    f32 rimw = wg_s(2.2f);
    f32 tan = l->hover > l->open ? l->hover : l->open;
    f32 a;
    int i;

    /* an all-but-invisible pad under the thin bar: layered windows ignore the
     * mouse over fully transparent pixels, and the bar alone is a small target */
    cv_rect(cv, x0, top, x1, top + wg_s(WG_DRAWER_HOVER), COL_BLACK, 0.012f);

    cv_rrect(cv, cx + wg_s(1.0f), cy + wg_s(2.0f), hw, hh, r, COL_BLACK, 0.30f);
    cv_rrect(cv, cx, cy, hw, hh, r, COL_AMPM_FILL, 1.0f);
    if (hh > rimw) {
        cv_rrect(cv, cx, cy, hw - rimw, hh - rimw, r - rimw, wg_mix(COL_AMPM_FILL, COL_BTN_FILL, tan), 1.0f);
    }

    /* hint: a down chevron on the hovered bar */
    a = l->hover * (1.0f - l->open * 3.0f);
    if (a > 0.0f) {
        s_paint_alpha = a;
        cv_icon(cv, ICON_CHEVRON, cx, cy, wg_s(13), COL_BTN_ICON);
        s_paint_alpha = 1.0f;
    }

    /* the buttons fade in over the last part of the slide */
    a = (l->open - 0.6f) / 0.4f;
    if (a <= 0.0f) return;
    s_paint_alpha = a > 1.0f ? 1.0f : a;
    for (i = 0; i < fr->nbtn; i++) {
        int b = fr->btns[i];
        f32 grow = (s_down == b) ? 0.92f : (s_hot == b) ? 1.1f : 1.0f;
        f32 bx, by, br = wg_s(WG_BTN_R) * grow;

        wg_btn_center(b, l, &bx, &by);
        cv_badge(cv, wg_s(bx), wg_s(by), br, br, s_hot == b ? COL_BTN_HOT : COL_TIME_FILL, COL_BTN_RIM, FALSE);
        cv_icon(cv, wg_btn_icon(b), wg_s(bx), wg_s(by), br * 1.45f, COL_BTN_ICON);
    }
    s_paint_alpha = 1.0f;
}

/* free placement: a translucent tan sheet with a dashed brown border, so the
 * whole window can be grabbed, and Accept/Cancel in the empty top-left */
#define WG_PLACE_CX  36.0f
#define WG_PLACE_HW  30.0f
#define WG_PLACE_HH  12.0f
#define WG_ACCEPT_CY 20.0f
#define WG_CANCEL_CY 48.0f

static void wg_render_place_backdrop(wg_canvas* cv) {
    f32 w = (f32)cv->w, h = (f32)cv->h, t = wg_s(2.0f), dash = wg_s(8.0f), gap = wg_s(5.0f), p;

    cv_rrect(cv, w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, wg_s(10), COL_BTN_FILL, 0.55f);
    for (p = wg_s(8); p < w - wg_s(8); p += dash + gap) {
        f32 e = p + dash < w - wg_s(8) ? p + dash : w - wg_s(8);
        cv_rect(cv, p, 0, e, t, COL_BTN_RIM, 0.9f);
        cv_rect(cv, p, h - t, e, h, COL_BTN_RIM, 0.9f);
    }
    for (p = wg_s(8); p < h - wg_s(8); p += dash + gap) {
        f32 e = p + dash < h - wg_s(8) ? p + dash : h - wg_s(8);
        cv_rect(cv, 0, p, t, e, COL_BTN_RIM, 0.9f);
        cv_rect(cv, w - t, p, w, e, COL_BTN_RIM, 0.9f);
    }
}

static void wg_render_place_buttons(wg_canvas* cv);

static void wg_render(wg_canvas* cv) {
    static const WCHAR* week[7] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    int hour, min, sec, weekday, month, day, h12;
    f32 dy, ty, tr = wg_s(WG_TIME_R);
    wg_layout l;
    WCHAR buf[8];

    wg_get_layout(&l);
    dy = wg_s(l.date_cy);
    ty = wg_s(l.time_cy);
    memset(cv->px, 0, (size_t)cv->w * cv->h * 4);
    pc_idle_get_clock(&hour, &min, &sec, &weekday, &month, &day);
    if (month < 1 || month > 12) {
        /* the game hasn't set its clock yet (still booting) */
        hour = 12; min = 0; sec = 0; weekday = 0; month = 1; day = 1;
    }
    h12 = hour % 12 == 0 ? 12 : hour % 12;

    if (s_placing) wg_render_place_backdrop(cv);

    /* Play, as tall as the time */
    {
        f32 grow = (s_down == WG_BTN_PLAY) ? 0.95f : (s_hot == WG_BTN_PLAY) ? 1.04f : 1.0f;
        f32 cx = wg_s(wg_frame()->play_cx), hw = wg_s(WG_PLAY_HW) * grow, hh = tr * grow;

        cv_badge(cv, cx, ty, hw, hh, s_hot == WG_BTN_PLAY ? COL_PLAY_HOT : COL_PLAY_FILL, COL_PLAY_RIM, FALSE);
        cv_icon(cv, ICON_PLAY, cx - hw + hh, ty, hh * 1.2f, COL_WHITE);
        cv_text(cv, L"Play", s_font_label, cx + hh * 0.55f, ty, COL_WHITE, 1.0f);
    }

    if (s_hide_clock) {
        /* just a settings badge beside Play */
        f32 grow = (s_down == WG_BTN_SETTINGS) ? 0.92f : (s_hot == WG_BTN_SETTINGS) ? 1.06f : 1.0f;
        f32 cx = wg_s(wg_frame()->gear_cx), r = tr * grow;

        cv_badge(cv, cx, ty, r, r, s_hot == WG_BTN_SETTINGS ? COL_BTN_HOT : COL_TIME_FILL, COL_BTN_RIM, FALSE);
        cv_icon(cv, ICON_GEAR, cx, ty, r * 1.45f, COL_BTN_ICON);
        wg_render_drawer(cv, &l);
        wg_render_place_buttons(cv);
        return;
    }

    /* date: month . day  weekday */
    swprintf(buf, 8, L"%d", month);
    cv_badge(cv, wg_s(106), dy, wg_s(30), wg_s(20), COL_DATE_FILL, COL_DATE_RIM, TRUE);
    cv_text(cv, buf, s_font_digits, wg_s(106), dy, COL_DATE_RIM, 1.0f);
    cv_badge(cv, wg_s(144), dy + wg_s(12), wg_s(4.5f), wg_s(4.5f), COL_DOT, COL_DOT_RIM, FALSE);
    swprintf(buf, 8, L"%d", day);
    cv_badge(cv, wg_s(182), dy, wg_s(30), wg_s(20), COL_DATE_FILL, COL_DATE_RIM, TRUE);
    cv_text(cv, buf, s_font_digits, wg_s(182), dy, COL_DATE_RIM, 1.0f);
    cv_badge(cv, wg_s(238), dy, wg_s(21), wg_s(21), weekday == 0 ? COL_WEEK_SUN : COL_WEEK_DAY,
             COL_DATE_RIM, FALSE);
    cv_text(cv, week[weekday % 7], s_font_label, wg_s(238), dy, COL_WEEK_TEXT, 1.0f);

    /* time: hh : mm  am/pm, the colon blinking with the seconds like m_banti */
    wg_draw_digit_coin(cv, wg_s(92), ty, tr, h12 / 10 == 0 ? -1 : h12 / 10);
    wg_draw_digit_coin(cv, wg_s(125), ty, tr, h12 % 10);
    if (sec & 1) {
        cv_badge(cv, wg_s(147), ty - wg_s(6), wg_s(3.2f), wg_s(3.2f), COL_COLON, COL_DOT_RIM, FALSE);
        cv_badge(cv, wg_s(147), ty + wg_s(7), wg_s(3.2f), wg_s(3.2f), COL_COLON, COL_DOT_RIM, FALSE);
    }
    wg_draw_digit_coin(cv, wg_s(169), ty, tr, min / 10);
    wg_draw_digit_coin(cv, wg_s(202), ty, tr, min % 10);
    cv_badge(cv, wg_s(238), ty, wg_s(16), wg_s(16), COL_AMPM_FILL, COL_DATE_RIM, FALSE);
    cv_text(cv, hour < 12 ? L"am" : L"pm", s_font_label, wg_s(238), ty - wg_s(1), COL_AMPM_TEXT, 1.0f);

    wg_render_drawer(cv, &l);
    wg_render_place_buttons(cv);
}

static void wg_render_place_buttons(wg_canvas* cv) {
    if (s_placing) {
        f32 grow = (s_down == WG_BTN_ACCEPT) ? 0.95f : (s_hot == WG_BTN_ACCEPT) ? 1.05f : 1.0f;
        cv_badge(cv, wg_s(WG_PLACE_CX), wg_s(WG_ACCEPT_CY), wg_s(WG_PLACE_HW) * grow, wg_s(WG_PLACE_HH) * grow,
                 s_hot == WG_BTN_ACCEPT ? COL_PLAY_HOT : COL_PLAY_FILL, COL_PLAY_RIM, FALSE);
        cv_text(cv, L"Accept", s_font_label, wg_s(WG_PLACE_CX), wg_s(WG_ACCEPT_CY), COL_WHITE, 1.0f);
        grow = (s_down == WG_BTN_CANCEL) ? 0.95f : (s_hot == WG_BTN_CANCEL) ? 1.05f : 1.0f;
        cv_badge(cv, wg_s(WG_PLACE_CX), wg_s(WG_CANCEL_CY), wg_s(WG_PLACE_HW) * grow, wg_s(WG_PLACE_HH) * grow,
                 s_hot == WG_BTN_CANCEL ? COL_BTN_HOT : COL_TIME_FILL, COL_BTN_RIM, FALSE);
        cv_text(cv, L"Cancel", s_font_label, wg_s(WG_PLACE_CX), wg_s(WG_CANCEL_CY), COL_BTN_ICON, 1.0f);
    }
}

/* where the widget goes on the primary monitor's work area */
static void wg_target_rect(RECT* rc, int w, int h) {
    POINT zero = { 0, 0 };
    MONITORINFO mi;
    RECT wa;
    int m = (int)wg_s(WG_MARGIN);

    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
    wa = mi.rcWork;
    if (s_pos == WG_POS_FREE) {
        rc->left = wa.left + s_free_x;
        rc->top = wa.top + s_free_y;
    } else {
        int col = s_pos_col[s_pos], row = s_pos_row[s_pos];
        rc->left = col == 0 ? wa.left + m : col == 1 ? (wa.left + wa.right - w) / 2 : wa.right - m - w;
        rc->top = row == 0 ? wa.top + m : row == 1 ? (wa.top + wa.bottom - h) / 2 : wa.bottom - m - h;
    }
    /* stay on screen, e.g. after a resolution change */
    if (rc->left > wa.right - w) rc->left = wa.right - w;
    if (rc->top > wa.bottom - h) rc->top = wa.bottom - h;
    if (rc->left < wa.left) rc->left = wa.left;
    if (rc->top < wa.top) rc->top = wa.top;
    rc->right = rc->left + w;
    rc->bottom = rc->top + h;
}

/* ---- window ---- */

static void wg_place_and_paint(int force) {
    int w = (int)wg_s(wg_frame()->w), h = (int)wg_s(WG_H);
    int hour, min, sec, weekday, month, day, key;
    RECT rc;
    wg_canvas cv;
    BITMAPINFO bi;
    SIZE size;
    POINT src = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    if (s_hwnd == NULL) return;
    size.cx = w;
    size.cy = h;

    wg_target_rect(&rc, w, h);
    /* HWND_BOTTOM: an owned window can't go below its owner, so this parks it
     * directly above the desktop, under every normal window. While it's being
     * placed it floats above everything instead. */
    SetWindowPos(s_hwnd, s_placing ? HWND_TOPMOST : HWND_BOTTOM, rc.left, rc.top, w, h,
                 SWP_NOACTIVATE | (s_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (!s_visible) return;

    /* redraw only when something visible changed */
    pc_idle_get_clock(&hour, &min, &sec, &weekday, &month, &day);
    key = ((((((month * 32 + day) * 24 + hour) * 60 + min) * 2 + (sec & 1)) * 16) + (s_hot + 1)) * 16 + (s_down + 1);
    key = key * 2 + s_placing;
    if (!force && key == s_drawn_key) return;
    s_drawn_key = key;

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    cv.w = w;
    cv.h = h;
    cv.dc = CreateCompatibleDC(NULL);
    cv.bmp = CreateDIBSection(cv.dc, &bi, DIB_RGB_COLORS, (void**)&cv.px, NULL, 0);
    if (cv.bmp == NULL) {
        DeleteDC(cv.dc);
        return;
    }
    SelectObject(cv.dc, cv.bmp);
    wg_render(&cv);
    GdiFlush();
    UpdateLayeredWindow(s_hwnd, NULL, NULL, &size, cv.dc, &src, 0, &blend, ULW_ALPHA);
    DeleteObject(cv.bmp);
    DeleteDC(cv.dc);
}

static int wg_hit(int x, int y) {
    f32 fx = (f32)x, fy = (f32)y;
    wg_layout l;
    int i;

    if (s_placing) {
        /* only Accept/Cancel; anywhere else drags */
        if (fabsf(fx - wg_s(WG_PLACE_CX)) <= wg_s(WG_PLACE_HW)) {
            if (fabsf(fy - wg_s(WG_ACCEPT_CY)) <= wg_s(WG_PLACE_HH)) return WG_BTN_ACCEPT;
            if (fabsf(fy - wg_s(WG_CANCEL_CY)) <= wg_s(WG_PLACE_HH)) return WG_BTN_CANCEL;
        }
        return WG_BTN_NONE;
    }
    wg_get_layout(&l);
    {
        const wg_frame_def* fr = wg_frame();
        f32 cx, cy;

        if (fabsf(fx - wg_s(fr->play_cx)) <= wg_s(WG_PLAY_HW) && fabsf(fy - wg_s(l.time_cy)) <= wg_s(WG_TIME_R)) {
            return WG_BTN_PLAY;
        }
        if (fr->gear_cx >= 0.0f && wg_btn_center(WG_BTN_SETTINGS, &l, &cx, &cy) &&
            (fx - wg_s(cx)) * (fx - wg_s(cx)) + (fy - wg_s(cy)) * (fy - wg_s(cy)) <= wg_s(WG_TIME_R) * wg_s(WG_TIME_R)) {
            return WG_BTN_SETTINGS;
        }
        if (fx >= wg_s(fr->drawer_x0) && fx <= wg_s(fr->drawer_x1)) {
            f32 top = wg_s(l.drawer_top);
            f32 h = wg_s(l.drawer_h > WG_DRAWER_HOVER ? l.drawer_h : WG_DRAWER_HOVER);

            if (fy < top || fy > top + h) return WG_BTN_NONE;
            if (l.open > 0.9f) {
                for (i = 0; i < fr->nbtn; i++) {
                    f32 dx, dy;
                    wg_btn_center(fr->btns[i], &l, &cx, &cy);
                    dx = fx - wg_s(cx);
                    dy = fy - wg_s(cy);
                    if (dx * dx + dy * dy <= wg_s(WG_BTN_R) * wg_s(WG_BTN_R)) return fr->btns[i];
                }
            }
            return WG_BTN_DRAWER;
        }
    }
    return WG_BTN_NONE;
}

/* ---- drawer animation ---- */

static f32 wg_approach(f32 v, f32 goal, f32 step) {
    if (v < goal) return v + step < goal ? v + step : goal;
    return v - step > goal ? v - step : goal;
}

static void wg_anim_step(void) {
    DWORD now = GetTickCount();
    f32 dt = (now - s_anim_last) / 1000.0f;
    /* hovering the drawer or a button in it (not the settings badge beside Play) */
    int in_drawer = s_hot >= 0 && s_hot <= WG_BTN_SETTINGS && !(s_hot == WG_BTN_SETTINGS && wg_frame()->gear_cx >= 0.0f);
    f32 hover_goal = (s_hot == WG_BTN_DRAWER || in_drawer) ? 1.0f : 0.0f;

    if (dt > 0.05f) dt = 0.05f;
    s_anim_last = now;
    s_hover_t = wg_approach(s_hover_t, hover_goal, dt / 0.12f);
    s_open_t = wg_approach(s_open_t, s_open ? 1.0f : 0.0f, dt / 0.25f);
    if (s_hover_t == hover_goal && s_open_t == (s_open ? 1.0f : 0.0f)) KillTimer(s_hwnd, WG_TIMER_ANIM);
    wg_place_and_paint(TRUE);
}

static void wg_anim_start(void) {
    s_anim_last = GetTickCount();
    SetTimer(s_hwnd, WG_TIMER_ANIM, WG_ANIM_MS, NULL);
}

/* ---- tooltips: one tracking tooltip, moved to whatever is hovered ---- */

static const WCHAR* wg_tip_text(int btn) {
    switch (btn) {
        case WG_BTN_ROT_L:    return L"Rotate camera left";
        case WG_BTN_ROT_R:    return L"Rotate camera right";
        case WG_BTN_ZOOM_OUT: return L"Zoom out";
        case WG_BTN_ZOOM_IN:  return L"Zoom in";
        case WG_BTN_RESET:    return L"Reset camera";
        case WG_BTN_SOUND:    return s_muted ? L"Turn music on" : L"Mute music";
        case WG_BTN_SETTINGS: return L"Settings";
        case WG_BTN_PLAY:     return L"Play  (Ctrl+Alt+P)";
        case WG_BTN_DRAWER:
            if (s_open) return L"Hide controls";
            return s_hide_clock ? L"Camera and music controls" : L"Camera, music and settings";
        case WG_BTN_ACCEPT:   return L"Keep the clock here";
        case WG_BTN_CANCEL:   return L"Put it back where it was";
    }
    return NULL;
}

static void wg_tip_info(TOOLINFOW* ti) {
    memset(ti, 0, sizeof(*ti));
    ti->cbSize = sizeof(*ti);
    ti->uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti->hwnd = s_hwnd;
    ti->uId = 1;
}

static void wg_tip_hide(void) {
    TOOLINFOW ti;

    KillTimer(s_hwnd, WG_TIMER_TIP);
    if (s_tip != NULL && s_tip_for != WG_BTN_NONE) {
        wg_tip_info(&ti);
        SendMessageW(s_tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
    }
    s_tip_for = WG_BTN_NONE;
}

static void wg_tip_show(int btn) {
    const WCHAR* text = wg_tip_text(btn);
    TOOLINFOW ti;
    wg_layout l;
    RECT wr;
    f32 cx, top;
    DWORD size;

    if (s_tip == NULL || text == NULL) return;
    wg_tip_info(&ti);
    ti.lpszText = (LPWSTR)text;
    SendMessageW(s_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);

    /* centered above the hovered thing */
    wg_get_layout(&l);
    if (btn == WG_BTN_ACCEPT || btn == WG_BTN_CANCEL) {
        cx = WG_PLACE_CX;
        top = (btn == WG_BTN_ACCEPT ? WG_ACCEPT_CY : WG_CANCEL_CY) - WG_PLACE_HH;
    } else if (btn == WG_BTN_DRAWER) {
        cx = (wg_frame()->drawer_x0 + wg_frame()->drawer_x1) * 0.5f;
        top = l.drawer_top;
    } else {
        f32 cy;
        if (!wg_btn_center(btn, &l, &cx, &cy)) return;
        top = cy - (btn == WG_BTN_PLAY || (btn == WG_BTN_SETTINGS && wg_frame()->gear_cx >= 0.0f) ? WG_TIME_R
                                                                                                  : WG_BTN_R);
    }
    GetWindowRect(s_hwnd, &wr);
    size = (DWORD)SendMessageW(s_tip, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
    {
        MONITORINFO mi;
        int x = wr.left + (int)wg_s(cx) - LOWORD(size) / 2;
        int y = wr.top + (int)wg_s(top) - HIWORD(size) - (int)wg_s(4);

        /* keep it on screen; the widget can hug any edge */
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        if (x > mi.rcWork.right - LOWORD(size) - 4) x = mi.rcWork.right - LOWORD(size) - 4;
        if (x < mi.rcWork.left + 4) x = mi.rcWork.left + 4;
        if (y < mi.rcWork.top + 4) y = mi.rcWork.top + 4;
        SendMessageW(s_tip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    }
    SendMessageW(s_tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    s_tip_for = btn;
}

static void wg_set_hot(int hot) {
    if (hot != s_hot) {
        s_hot = hot;
        wg_tip_hide();
        if (hot != WG_BTN_NONE) SetTimer(s_hwnd, WG_TIMER_TIP, WG_TIP_DELAY_MS, NULL);
        wg_anim_start();
        wg_place_and_paint(FALSE);
    }
}

/* ---- free placement ---- */

static void wg_begin_placing(void) {
    RECT rc, wa;
    MONITORINFO mi;
    POINT zero = { 0, 0 };

    if (s_hwnd == NULL || s_placing) return;
    s_saved_pos = s_pos;
    s_saved_free_x = s_free_x;
    s_saved_free_y = s_free_y;
    /* start from wherever it is now */
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
    wa = mi.rcWork;
    GetWindowRect(s_hwnd, &rc);
    s_free_x = rc.left - wa.left;
    s_free_y = rc.top - wa.top;
    s_pos = WG_POS_FREE;
    s_placing = TRUE;
    /* close the drawer so the clock sits where it will normally rest */
    s_open = FALSE;
    s_open_t = s_hover_t = 0.0f;
    KillTimer(s_hwnd, WG_TIMER_ANIM);
    wg_place_and_paint(TRUE);
}

static void wg_end_placing(int accept) {
    if (!s_placing) return;
    s_placing = FALSE;
    s_dragging = FALSE;
    if (accept) {
        wg_save_settings();
    } else {
        s_pos = s_saved_pos;
        s_free_x = s_saved_free_x;
        s_free_y = s_saved_free_y;
    }
    wg_place_and_paint(TRUE);
}

static void wg_press(int btn) {
    switch (btn) {
        case WG_BTN_ACCEPT:   wg_end_placing(TRUE); break;
        case WG_BTN_CANCEL:   wg_end_placing(FALSE); break;
        case WG_BTN_ROT_L:    pc_idle_user_rotate(WG_ROTATE_STEP); break;
        case WG_BTN_ROT_R:    pc_idle_user_rotate(-WG_ROTATE_STEP); break;
        case WG_BTN_ZOOM_OUT: pc_idle_user_zoom(WG_ZOOM_STEP); break;
        case WG_BTN_ZOOM_IN:  pc_idle_user_zoom(-WG_ZOOM_STEP); break;
        case WG_BTN_RESET:    pc_idle_user_reset(); break;
        case WG_BTN_SOUND:
            s_muted = !s_muted;
            wg_apply_volume();
            wg_save_settings();
            wg_place_and_paint(TRUE);
            break;
        case WG_BTN_SETTINGS: wg_open_settings(); break;
        case WG_BTN_PLAY:     pc_wallpaper_request_play(); break;
        case WG_BTN_DRAWER:
            s_open = !s_open;
            wg_anim_start();
            break;
    }
}

static int wg_repeats(int btn) {
    return btn == WG_BTN_ROT_L || btn == WG_BTN_ROT_R || btn == WG_BTN_ZOOM_OUT || btn == WG_BTN_ZOOM_IN;
}

static LRESULT CALLBACK wg_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_TIMER:
            if (wparam == WG_TIMER_TICK) {
                wg_place_and_paint(FALSE); /* clock, resolution, taskbar */
            } else if (wparam == WG_TIMER_REPEAT) {
                if (s_down != WG_BTN_NONE && s_down == s_hot) wg_press(s_down);
                SetTimer(hwnd, WG_TIMER_REPEAT, WG_REPEAT_MS, NULL);
            } else if (wparam == WG_TIMER_ANIM) {
                wg_anim_step();
            } else if (wparam == WG_TIMER_TIP) {
                KillTimer(hwnd, WG_TIMER_TIP);
                if (s_hot != WG_BTN_NONE && s_down == WG_BTN_NONE) wg_tip_show(s_hot);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (!s_tracking) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                s_tracking = TrackMouseEvent(&tme);
            }
            if (s_dragging) {
                POINT pt;
                GetCursorPos(&pt);
                s_free_x = s_drag_origin.x + pt.x - s_drag_cursor.x;
                s_free_y = s_drag_origin.y + pt.y - s_drag_cursor.y;
                wg_place_and_paint(FALSE);
                return 0;
            }
            wg_set_hot(wg_hit((short)LOWORD(lparam), (short)HIWORD(lparam)));
            return 0;
        case WM_MOUSELEAVE:
            s_tracking = FALSE;
            wg_set_hot(WG_BTN_NONE);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(s_hot != WG_BTN_NONE ? IDC_HAND
                                                  : s_placing          ? IDC_SIZEALL
                                                                       : IDC_ARROW)));
            return TRUE;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            wg_tip_hide();
            s_down = wg_hit((short)LOWORD(lparam), (short)HIWORD(lparam));
            s_hot = s_down;
            if (s_placing && s_down == WG_BTN_NONE) {
                /* the free offset is clamped on screen; drag from the clamped spot */
                RECT rc, wa;
                MONITORINFO mi;
                POINT zero = { 0, 0 };
                mi.cbSize = sizeof(mi);
                GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &mi);
                wa = mi.rcWork;
                GetWindowRect(hwnd, &rc);
                s_drag_origin.x = rc.left - wa.left;
                s_drag_origin.y = rc.top - wa.top;
                GetCursorPos(&s_drag_cursor);
                s_dragging = TRUE;
                SetCapture(hwnd);
                return 0;
            }
            if (s_down != WG_BTN_NONE) {
                SetCapture(hwnd);
                if (wg_repeats(s_down)) {
                    wg_press(s_down);
                    SetTimer(hwnd, WG_TIMER_REPEAT, WG_REPEAT_DELAY_MS, NULL);
                }
            }
            wg_place_and_paint(FALSE);
            return 0;
        case WM_LBUTTONUP: {
            int btn = s_down;
            int over = wg_hit((short)LOWORD(lparam), (short)HIWORD(lparam));

            if (s_dragging) {
                s_dragging = FALSE;
                ReleaseCapture();
                return 0;
            }
            s_down = WG_BTN_NONE;
            KillTimer(hwnd, WG_TIMER_REPEAT);
            ReleaseCapture();
            wg_place_and_paint(FALSE);
            if (btn != WG_BTN_NONE && btn == over && !wg_repeats(btn)) {
                wg_press(btn);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            s_dragging = FALSE;
            if (s_down != WG_BTN_NONE) {
                s_down = WG_BTN_NONE;
                KillTimer(hwnd, WG_TIMER_REPEAT);
                wg_place_and_paint(FALSE);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; /* clicking the widget shouldn't steal focus from the desktop */
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND wg_find_icon_host(void) {
    HWND progman = FindWindowW(L"Progman", NULL);
    HWND defview = progman ? FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL) : NULL;
    HWND top = NULL;

    /* pre-24H2 the icon view lives in a top-level WorkerW instead */
    while (defview == NULL && (top = FindWindowExW(NULL, top, L"WorkerW", NULL)) != NULL) {
        defview = FindWindowExW(top, NULL, L"SHELLDLL_DefView", NULL);
    }
    return defview ? GetParent(defview) : NULL;
}

static HFONT wg_font(const WCHAR* face, int px, int weight) {
    return CreateFontW(-(int)wg_s((f32)px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

/* the clock's numerals are chunky and rounded; use a rounded face if there is one */
static HFONT wg_digit_font(int px) {
    static const WCHAR* faces[] = { L"Arial Rounded MT Bold", L"VAG Rounded", L"Segoe UI Black" };
    HDC dc = GetDC(NULL);
    HFONT font = NULL;
    int i;

    for (i = 0; i < 3 && font == NULL; i++) {
        WCHAR got[LF_FACESIZE];
        HFONT f = wg_font(faces[i], px, i == 2 ? FW_BLACK : FW_NORMAL);
        HGDIOBJ old = SelectObject(dc, f);
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        if (_wcsicmp(got, faces[i]) == 0) {
            font = f;
        } else {
            DeleteObject(f);
        }
    }
    ReleaseDC(NULL, dc);
    return font ? font : wg_font(L"Segoe UI", px, FW_BOLD);
}

static void wg_make_fonts(void) {
    if (s_font_digits) DeleteObject(s_font_digits);
    if (s_font_label) DeleteObject(s_font_label);
    s_font_digits = wg_digit_font(22);
    s_font_label = wg_font(L"Segoe UI", 13, FW_BOLD);
}

static void wg_set_scale(f32 scale) {
    s_scale = scale;
    wg_make_fonts();
    wg_place_and_paint(TRUE);
}

void pc_widget_attach(void) {
    static int registered = FALSE;
    HWND parent = wg_find_icon_host();

    if (parent == NULL) return;

    if (!registered) {
        WNDCLASSW wc;
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
        HDC screen = GetDC(NULL);

        InitCommonControlsEx(&icc);
        s_dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(NULL, screen);
        wg_make_fonts();

        memset(&wc, 0, sizeof(wc));
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = wg_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"ACWallpaperWidget";
        RegisterClassW(&wc);
        registered = TRUE;
    }

    if (s_hwnd != NULL && IsWindow(s_hwnd) && s_parent == parent) {
        wg_place_and_paint(TRUE);
        return;
    }
    if (s_hwnd != NULL && IsWindow(s_hwnd)) DestroyWindow(s_hwnd); /* takes its tooltip with it */
    s_tip = NULL;
    s_tip_for = WG_BTN_NONE;

    s_parent = parent;
    /* A top-level per-pixel-alpha window owned by the desktop host. (As a
     * child of the desktop, 24H2's desktop composition doesn't show layered
     * windows.) Owned windows always stay above their owner, so it sits on
     * the desktop under every normal window; tool window = no taskbar entry. */
    s_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"ACWallpaperWidget", L"",
                             WS_POPUP, 0, 0, 1, 1, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (s_hwnd == NULL) return;

    /* tracking tooltip, positioned by hand: the widget never has focus and
     * its shapes aren't rectangles, so automatic tooltips don't fit */
    s_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s_hwnd, NULL,
                            GetModuleHandleW(NULL), NULL);
    if (s_tip != NULL) {
        TOOLINFOW ti;
        wg_tip_info(&ti);
        ti.lpszText = (LPWSTR)L"";
        SendMessageW(s_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }

    SetTimer(s_hwnd, WG_TIMER_TICK, WG_TICK_MS, NULL);
    wg_place_and_paint(TRUE);
}

void pc_widget_set_visible(int visible) {
    s_visible = visible;
    if (s_hwnd == NULL || !IsWindow(s_hwnd)) return;
    if (visible) {
        wg_place_and_paint(TRUE);
    } else {
        wg_tip_hide();
        wg_end_placing(FALSE);
        ShowWindow(s_hwnd, SW_HIDE);
        s_hot = s_down = WG_BTN_NONE;
        if (s_settings != NULL) DestroyWindow(s_settings);
    }
}

void pc_widget_destroy(void) {
    if (s_settings != NULL && IsWindow(s_settings)) DestroyWindow(s_settings);
    if (s_hwnd != NULL && IsWindow(s_hwnd)) DestroyWindow(s_hwnd);
    s_hwnd = NULL;
    s_tip = NULL;
    s_parent = NULL;
}

/* ---- settings dialog ---- */

enum {
    IDC_PC_CLOCK = 101,
    IDC_CUSTOM_CLOCK,
    IDC_DATE,
    IDC_TIME,
    IDC_CLOCK_SIZE,
    IDC_CAMERA_SPEED,
    IDC_OK,
    IDC_CANCEL,
    IDC_POSITION,
    IDC_MOVE,
    IDC_HIDE_CLOCK
};

static HFONT s_dlg_font = NULL;

static void wg_settings_enable(HWND dlg) {
    int custom = IsDlgButtonChecked(dlg, IDC_CUSTOM_CLOCK) == BST_CHECKED;
    int free = SendMessageW(GetDlgItem(dlg, IDC_POSITION), CB_GETCURSEL, 0, 0) == WG_POS_FREE;
    EnableWindow(GetDlgItem(dlg, IDC_DATE), custom);
    EnableWindow(GetDlgItem(dlg, IDC_TIME), custom);
    EnableWindow(GetDlgItem(dlg, IDC_MOVE), free);
}

/* returns TRUE when the widget should now be dragged into place */
static int wg_settings_apply(HWND dlg) {
    s64 offset = 0;
    int place = FALSE;
    int pos = (int)SendMessageW(GetDlgItem(dlg, IDC_POSITION), CB_GETCURSEL, 0, 0);

    int hide = IsDlgButtonChecked(dlg, IDC_HIDE_CLOCK) == BST_CHECKED;

    if (hide != s_hide_clock) {
        s_hide_clock = hide;
        wg_place_and_paint(TRUE);
    }
    if (pos == WG_POS_FREE) {
        place = s_pos != WG_POS_FREE;
    } else if (pos >= 0 && pos < WG_POS_CHOICES && pos != s_pos) {
        s_pos = pos;
        wg_place_and_paint(TRUE);
    }

    if (IsDlgButtonChecked(dlg, IDC_CUSTOM_CLOCK) == BST_CHECKED) {
        SYSTEMTIME d, t;
        struct tm want;
        time_t when;

        SendMessageW(GetDlgItem(dlg, IDC_DATE), DTM_GETSYSTEMTIME, 0, (LPARAM)&d);
        SendMessageW(GetDlgItem(dlg, IDC_TIME), DTM_GETSYSTEMTIME, 0, (LPARAM)&t);
        memset(&want, 0, sizeof(want));
        want.tm_year = d.wYear - 1900;
        want.tm_mon = d.wMonth - 1;
        want.tm_mday = d.wDay;
        want.tm_hour = t.wHour;
        want.tm_min = t.wMinute;
        want.tm_sec = 0;
        want.tm_isdst = -1;
        when = mktime(&want);
        if (when != (time_t)-1) offset = (s64)difftime(when, time(NULL));
    }

    {
        int sel = (int)SendMessageW(GetDlgItem(dlg, IDC_CLOCK_SIZE), CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < WG_SCALE_CHOICES && s_scale_choices[sel] != s_scale) wg_set_scale(s_scale_choices[sel]);
        sel = (int)SendMessageW(GetDlgItem(dlg, IDC_CAMERA_SPEED), CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < WG_SPEED_CHOICES) pc_idle_set_speed(s_speed_choices[sel]);
    }
    if (offset != pc_os_get_clock_offset()) {
        pc_os_set_clock_offset(offset);
        /* season, trees and lighting are set up when the scene loads */
        pc_idle_request_reload();
    }
    wg_save_settings();
    return place;
}

static LRESULT CALLBACK wg_settings_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDC_PC_CLOCK:
                case IDC_CUSTOM_CLOCK:
                    wg_settings_enable(hwnd);
                    return 0;
                case IDC_POSITION:
                    if (HIWORD(wparam) == CBN_SELCHANGE) wg_settings_enable(hwnd);
                    return 0;
                case IDC_OK:
                case IDC_MOVE: {
                    /* choosing Free place (or Move...) closes the dialog and
                     * hands the widget to the mouse */
                    int place = wg_settings_apply(hwnd) || LOWORD(wparam) == IDC_MOVE;
                    DestroyWindow(hwnd);
                    if (place) wg_begin_placing();
                    return 0;
                }
                case IDC_CANCEL:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            /* labels on the window's own background, not grey boxes */
            SetBkMode((HDC)wparam, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            s_settings = NULL;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND wg_ctl(HWND parent, const WCHAR* cls, const WCHAR* text, DWORD style, int id, int x, int y, int w,
                   int h, int dpi) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96),
                             MulDiv(w, dpi, 96), MulDiv(h, dpi, 96), parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_dlg_font, TRUE);
    return c;
}

static void wg_open_settings(void) {
    static int registered = FALSE;
    HDC screen;
    int dpi, w, h;
    RECT work;
    SYSTEMTIME now;
    s64 offset = pc_os_get_clock_offset();

    if (s_settings != NULL && IsWindow(s_settings)) {
        SetForegroundWindow(s_settings);
        return;
    }

    if (!registered) {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_DATE_CLASSES | ICC_STANDARD_CLASSES };
        NONCLIENTMETRICSW ncm;
        WNDCLASSW wc;

        InitCommonControlsEx(&icc);
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        s_dlg_font = CreateFontIndirectW(&ncm.lfMessageFont);

        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = wg_settings_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"ACWallpaperSettings";
        RegisterClassW(&wc);
        registered = TRUE;
    }

    screen = GetDC(NULL);
    dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(NULL, screen);

    {
        RECT rc = { 0, 0, MulDiv(340, dpi, 96), MulDiv(346, dpi, 96) };
        AdjustWindowRectEx(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    s_settings = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"ACWallpaperSettings",
                                 L"Animal Crossing Wallpaper Settings", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                 work.right - w - MulDiv(24, dpi, 96), work.bottom - h - MulDiv(24, dpi, 96), w, h,
                                 NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (s_settings == NULL) return;

    wg_ctl(s_settings, L"STATIC", L"Town clock", 0, 0, 16, 14, 300, 18, dpi);
    wg_ctl(s_settings, L"BUTTON", L"Use this PC's date and time", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
           IDC_PC_CLOCK, 24, 38, 290, 20, dpi);
    wg_ctl(s_settings, L"BUTTON", L"Use a custom date and time:", BS_AUTORADIOBUTTON, IDC_CUSTOM_CLOCK, 24, 62,
           290, 20, dpi);
    wg_ctl(s_settings, DATETIMEPICK_CLASSW, L"", DTS_SHORTDATECENTURYFORMAT | WS_TABSTOP, IDC_DATE, 44, 88, 130,
           24, dpi);
    wg_ctl(s_settings, DATETIMEPICK_CLASSW, L"", DTS_TIMEFORMAT | DTS_UPDOWN | WS_TABSTOP, IDC_TIME, 184, 88, 120,
           24, dpi);
    wg_ctl(s_settings, L"STATIC", L"A custom clock keeps running from the time you set.", 0, 0, 44, 118, 280, 18,
           dpi);
    wg_ctl(s_settings, L"STATIC", L"Clock size", 0, 0, 16, 154, 120, 18, dpi);
    {
        HWND combo = wg_ctl(s_settings, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CLOCK_SIZE,
                            110, 150, 150, 160, dpi);
        int i, sel = 1;
        for (i = 0; i < WG_SCALE_CHOICES; i++) {
            WCHAR item[48];
            swprintf(item, 48, L"%ls (%d%%)", s_scale_names[i], (int)(s_scale_choices[i] * 100.0f + 0.5f));
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)item);
            if (fabsf(s_scale_choices[i] - s_scale) < 0.01f) sel = i;
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
    }
    wg_ctl(s_settings, L"STATIC", L"Clock position", 0, 0, 16, 188, 120, 18, dpi);
    {
        HWND combo = wg_ctl(s_settings, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_POSITION,
                            110, 184, 150, 260, dpi);
        int i;
        for (i = 0; i < WG_POS_CHOICES; i++) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s_pos_names[i]);
        SendMessageW(combo, CB_SETCURSEL, s_pos, 0);
    }
    wg_ctl(s_settings, L"BUTTON", L"Move...", WS_TABSTOP, IDC_MOVE, 266, 183, 60, 25, dpi);
    wg_ctl(s_settings, L"BUTTON", L"Hide the clock (show only Play and settings)", BS_AUTOCHECKBOX | WS_TABSTOP,
           IDC_HIDE_CLOCK, 16, 218, 310, 20, dpi);
    CheckDlgButton(s_settings, IDC_HIDE_CLOCK, s_hide_clock ? BST_CHECKED : BST_UNCHECKED);
    wg_ctl(s_settings, L"STATIC", L"Camera speed", 0, 0, 16, 256, 120, 18, dpi);
    {
        HWND combo = wg_ctl(s_settings, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                            IDC_CAMERA_SPEED, 110, 252, 150, 160, dpi);
        f32 speed = pc_idle_get_speed();
        int i, sel = 1;
        for (i = 0; i < WG_SPEED_CHOICES; i++) {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s_speed_names[i]);
            if (fabsf(s_speed_choices[i] - speed) < 0.01f) sel = i;
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
    }
    wg_ctl(s_settings, L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_OK, 154, 302, 80, 28, dpi);
    wg_ctl(s_settings, L"BUTTON", L"Cancel", WS_TABSTOP, IDC_CANCEL, 242, 302, 80, 28, dpi);

    /* pickers start at the town's current time */
    {
        time_t t = time(NULL) + (time_t)offset;
        struct tm* lt = localtime(&t);
        memset(&now, 0, sizeof(now));
        if (lt) {
            now.wYear = (WORD)(lt->tm_year + 1900);
            now.wMonth = (WORD)(lt->tm_mon + 1);
            now.wDay = (WORD)lt->tm_mday;
            now.wDayOfWeek = (WORD)lt->tm_wday;
            now.wHour = (WORD)lt->tm_hour;
            now.wMinute = (WORD)lt->tm_min;
        }
        SendMessageW(GetDlgItem(s_settings, IDC_DATE), DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&now);
        SendMessageW(GetDlgItem(s_settings, IDC_TIME), DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&now);
    }
    CheckRadioButton(s_settings, IDC_PC_CLOCK, IDC_CUSTOM_CLOCK, offset == 0 ? IDC_PC_CLOCK : IDC_CUSTOM_CLOCK);
    wg_settings_enable(s_settings);

    ShowWindow(s_settings, SW_SHOW);
    SetForegroundWindow(s_settings);
}

#else /* !_WIN32 */

void pc_widget_attach(void) {}
void pc_widget_set_visible(int visible) { (void)visible; }
void pc_widget_destroy(void) {}
void pc_widget_load_settings(void) {}

#endif
