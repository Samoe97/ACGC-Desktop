/* pc_idle.c - wallpaper idle mode: autopilot walker + slow zoomed-out camera
 *
 * The title demo is a normal play scene in the player's town, loaded from the
 * save and thrown away afterwards (the title reloads the save before Start).
 * That makes it a safe sandbox for an attract mode: nothing done here is ever
 * written to the memory card.
 *
 * The demo player is kept (hidden) because the field only draws the 3x3 blocks
 * around it and structures/villagers spawn near it. Walking it with stick
 * input instead of teleporting keeps collision, acre transitions and water
 * handling in the game's hands. */
#include "pc_platform.h"
#include "pc_idle.h"
#include "pc_wallpaper.h"
#include "pc_timelapse.h"

#include "m_player_lib.h"
#include "m_event.h"
#include "m_field_info.h"
#include "m_field_make.h"
#include "m_random_field_h.h"
#include "m_actor.h"
#include "sys_math.h"
#include "m_lib.h"
#include "m_common_data.h"

/* camera framing: a high three-quarter view, much wider than the gameplay
 * camera. Zoom level 1 is this view and 0 is the gameplay camera itself
 * (Init_Camera2: eye 1240 back at 45 degrees, 20-degree lens); the widget's
 * zoom slides between them, so zooming in also tilts up. */
#define IDLE_CAM_DIST       1500.0f
#define IDLE_CAM_DIST_SWING 150.0f   /* slow zoom breathing */
#define IDLE_CAM_PITCH_DEG  57.0f    /* elevation above the ground plane */
#define IDLE_CAM_YAW_SWING  14.0f    /* degrees of slow side-to-side sway */
#define IDLE_CAM_FOV        28.0f
#define GAME_CAM_DIST       1240.0f
#define GAME_CAM_PITCH_DEG  45.0f
#define GAME_CAM_FOV        20.0f
#define IDLE_CAM_NEAR       200.0f
#define IDLE_CAM_FAR        4000.0f
#define IDLE_CAM_FOLLOW_SEC 2.5f     /* center lag behind the walker */

/* autopilot */
#define IDLE_WALK_STICK     34.0f    /* stick magnitude at speed 1.0: an unhurried walk */
#define IDLE_WALK_STICK_MIN 14.0f    /* just above the stick dead zone (STICK_MIN 9.9) */
#define IDLE_WADE_STICK     61.0f    /* full tilt (STICK_MAX), needed to trigger an acre crossing */
#define IDLE_BORDER_APPROACH 60.0f   /* square up this close to a block border */
#define IDLE_ARRIVE_DIST    60.0f
#define IDLE_NPC_STANDOFF   90.0f    /* stop this far from a villager */
#define IDLE_STUCK_SEC      3.0f
#define IDLE_STUCK_PROGRESS 40.0f
#define IDLE_NPC_CHANCE     0.45f
#define IDLE_NPC_MIN_DIST   250.0f   /* skip villagers we're already beside */

/* The map ends at the outer edge of the border cliffs; past it is void the
 * gameplay camera never sees. The view's ground footprint is kept inside. */
#define IDLE_BEND_REACH     1.7f     /* top-of-screen reach vs. a flat world (tuned by eye) */
#define IDLE_MAP_MIN_X      0.0f
#define IDLE_MAP_MAX_X      (mFI_BK_WORLDSIZE_X_F * BLOCK_X_NUM)
#define IDLE_MAP_MIN_Z      0.0f
#define IDLE_MAP_MARGIN     24.0f

/* user camera controls (the desktop widget) */
#define IDLE_USER_YAW_MAX   35.0f    /* degrees; acre crossings need a roughly north-up camera */
#define IDLE_USER_ZOOM_MIN  0.0f     /* the gameplay camera */
#define IDLE_USER_ZOOM_MAX  1.25f    /* further out would see past the 5x5 ground */
#define IDLE_USER_EASE_SEC  0.35f

/* timelapse: the walker tours from acre to acre at the slowest pace, with
 * short stops, and the camera trails it more lazily than usual */
#define IDLE_TL_SPEED       0.35f
#define IDLE_TL_FOLLOW_SEC  5.0f

typedef struct {
    int    have_target;
    f32    target_x, target_z;
    ACTOR* target_npc;         /* re-validated against the actor list each frame */
    f32    wait_sec;
    f32    stuck_sec;
    f32    best_dist;

    int    shadow_hidden;      /* we turned the walker's shadow off */
    int    cam_init;
    xyz_t  cam_center;
    f32    time_sec;

    int    reload_requested;
    int    last_day;                     /* for reloading at midnight */

    f32    user_yaw_target, user_yaw;    /* degrees */
    f32    user_zoom_target, user_zoom;  /* 0 = gameplay camera, 1 = default idle view */
} pc_idle_state;

static pc_idle_state s_idle = { .user_zoom_target = 1.0f, .user_zoom = 1.0f };

/* The "Camera speed" setting. The camera follows the walker, so this scales
 * the walk, the pauses, the camera's follow lag and its slow drift together. */
static f32 s_speed = 0.6f;

void pc_idle_set_speed(f32 speed) {
    if (speed < 0.2f) speed = 0.2f;
    if (speed > 2.0f) speed = 2.0f;
    s_speed = speed;
}

f32 pc_idle_get_speed(void) {
    return s_speed;
}

/* the pace actually used: the setting, or the timelapse's own */
static f32 idle_speed(void) {
    return g_pc_timelapse ? IDLE_TL_SPEED : s_speed;
}

int pc_idle_active(void) {
    return g_pc_wallpaper_active && mEv_IsTitleDemo();
}

void pc_idle_request_reload(void) {
    s_idle.reload_requested = TRUE;
}

int pc_idle_take_reload_request(void) {
    /* a new day means new weather, events and maybe a new season */
    int day = Common_Get(time.rtc_time.day);
    if (s_idle.last_day != 0 && day != s_idle.last_day && !g_pc_timelapse) s_idle.reload_requested = TRUE;
    s_idle.last_day = day;

    if (!s_idle.reload_requested) return FALSE;
    s_idle.reload_requested = FALSE;
    return TRUE;
}

void pc_idle_scene_start(void) {
    s_idle.cam_init = FALSE;
    s_idle.have_target = FALSE;
    s_idle.target_npc = NULL;
    s_idle.wait_sec = 0.0f;
    s_idle.last_day = Common_Get(time.rtc_time.day);
}

/* ---- autopilot ---- */

static int idle_npc_alive(GAME_PLAY* play, ACTOR* npc) {
    ACTOR* a;

    if (npc == NULL) return FALSE;
    for (a = play->actor_info.list[ACTOR_PART_NPC].actor; a != NULL; a = a->next_actor) {
        if (a == npc) return TRUE;
    }
    return FALSE;
}

/* A random villager that's worth walking to (not one we're standing next to). */
static ACTOR* idle_pick_npc(GAME_PLAY* play, const xyz_t* from) {
    ACTOR* far_npcs[16];
    int count = 0;
    ACTOR* a;

    for (a = play->actor_info.list[ACTOR_PART_NPC].actor; a != NULL && count < 16; a = a->next_actor) {
        f32 dx = a->world.position.x - from->x;
        f32 dz = a->world.position.z - from->z;
        if (dx * dx + dz * dz >= IDLE_NPC_MIN_DIST * IDLE_NPC_MIN_DIST) far_npcs[count++] = a;
    }
    return count > 0 ? far_npcs[(int)RANDOM_F(count)] : NULL;
}

/* A random spot in a town block, usually one next to where we are so the
 * camera drifts across the map instead of shuttling corner to corner. */
static void idle_pick_spot(const xyz_t* from, f32* x, f32* z) {
    int bx, bz, tries;

    mFI_Wpos2BlockNum(&bx, &bz, *from);

    for (tries = 0; tries < 12; tries++) {
        int tx, tz;

        if (g_pc_timelapse) {
            /* a tour: always on to a neighbouring acre */
            tx = bx + (int)RANDOM_F(3) - 1;
            tz = bz + (int)RANDOM_F(3) - 1;
            if (tx == bx && tz == bz) continue;
        } else if (fqrand() < 0.7f) {
            tx = bx + (int)RANDOM_F(3) - 1;
            tz = bz + (int)RANDOM_F(3) - 1;
        } else {
            tx = 1 + (int)RANDOM_F(FG_BLOCK_X_NUM);
            tz = 1 + (int)RANDOM_F(FG_BLOCK_Z_NUM);
        }
        if (tx < 1 || tx > FG_BLOCK_X_NUM || tz < 1 || tz > FG_BLOCK_Z_NUM) continue;
        if (mFI_BkNum2BlockKind(tx, tz) & (mRF_BLOCKKIND_MARINE | mRF_BLOCKKIND_BORDER)) continue;

        mFI_BkNum2WposXZ(x, z, tx, tz);
        *x += mFI_BK_WORLDSIZE_HALF_X_F + RANDOM_CENTER_F(mFI_BK_WORLDSIZE_X_F * 0.6f);
        *z += mFI_BK_WORLDSIZE_HALF_Z_F + RANDOM_CENTER_F(mFI_BK_WORLDSIZE_Z_F * 0.6f);
        return;
    }

    /* nowhere better: amble around the current spot */
    *x = from->x + RANDOM_CENTER_F(300.0f);
    *z = from->z + RANDOM_CENTER_F(300.0f);
}

static void idle_new_target(GAME_PLAY* play, const xyz_t* pos) {
    s_idle.target_npc = (!g_pc_timelapse && fqrand() < IDLE_NPC_CHANCE) ? idle_pick_npc(play, pos) : NULL;
    if (s_idle.target_npc == NULL) {
        idle_pick_spot(pos, &s_idle.target_x, &s_idle.target_z);
    }
    s_idle.have_target = TRUE;
    s_idle.stuck_sec = 0.0f;
    s_idle.best_dist = 1e9f;
}

void pc_idle_title_demo_move(GAME_PLAY* play) {
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR(play);
    f32 dt = play->game.graph->dt;
    xyz_t pos;
    f32 dx, dz, dist, arrive, stick;
    s16 yaw, stick_angle;

    if (player == NULL) {
        mPlib_SetData1_controller_data_for_title_demo(0, 0, 0.0f, 0.0f);
        return;
    }
    if (dt > 0.1f) dt = 0.1f;
    pos = player->actor_class.world.position;

    /* the body isn't drawn (Player_actor_draw), but the actor system still
     * draws its shadow */
    player->actor_class.shape_info.draw_shadow = FALSE;
    s_idle.shadow_hidden = TRUE;

    if (s_idle.wait_sec > 0.0f) {
        s_idle.wait_sec -= dt;
        mPlib_SetData1_controller_data_for_title_demo(0, 0, 0.0f, 0.0f);
        return;
    }

    if (s_idle.target_npc != NULL && !idle_npc_alive(play, s_idle.target_npc)) {
        s_idle.have_target = FALSE;
    }
    if (!s_idle.have_target) {
        idle_new_target(play, &pos);
    }

    arrive = IDLE_ARRIVE_DIST;
    if (s_idle.target_npc != NULL) {
        s_idle.target_x = s_idle.target_npc->world.position.x;
        s_idle.target_z = s_idle.target_npc->world.position.z;
        arrive = IDLE_NPC_STANDOFF;
    }

    dx = s_idle.target_x - pos.x;
    dz = s_idle.target_z - pos.z;
    dist = sqrtf(dx * dx + dz * dz);

    if (dist < arrive) {
        /* linger: longer when watching a villager */
        s_idle.have_target = FALSE;
        s_idle.wait_sec = ((s_idle.target_npc != NULL ? 5.0f : 1.5f) + RANDOM_F(s_idle.target_npc != NULL ? 6.0f : 3.0f)) / idle_speed();
        if (g_pc_timelapse) s_idle.wait_sec = 1.0f + RANDOM_F(2.0f); /* keep touring */
        mPlib_SetData1_controller_data_for_title_demo(0, 0, 0.0f, 0.0f);
        return;
    }

    /* give up on targets behind rivers, cliffs and buildings */
    if (dist < s_idle.best_dist - IDLE_STUCK_PROGRESS) {
        s_idle.best_dist = dist;
        s_idle.stuck_sec = 0.0f;
    } else {
        s_idle.stuck_sec += dt;
        if (s_idle.stuck_sec > IDLE_STUCK_SEC / idle_speed()) {
            s_idle.have_target = FALSE;
            s_idle.wait_sec = 1.0f + RANDOM_F(2.0f);
        }
    }

    yaw = atans_table(dz, dx);
    stick = IDLE_WALK_STICK * idle_speed();
    if (stick < IDLE_WALK_STICK_MIN) stick = IDLE_WALK_STICK_MIN;
    if (stick > IDLE_WADE_STICK) stick = IDLE_WADE_STICK;

    /* Crossing into the next acre is a scripted "wade" that only starts when
     * the stick is pushed hard and nearly straight at the border
     * (Player_actor_CheckAbleMoveWadeBlock), so square up when we get close. */
    {
        xyz_t local;
        mFI_WpostoLposInBK(&local, pos);
        if (dx > 0.0f && local.x > mFI_BK_WORLDSIZE_X_F - IDLE_BORDER_APPROACH && dx > fabsf(dz) * 0.5f) {
            yaw = DEG2SHORT_ANGLE2(90.0f);
            stick = IDLE_WADE_STICK;
        } else if (dx < 0.0f && local.x < IDLE_BORDER_APPROACH && -dx > fabsf(dz) * 0.5f) {
            yaw = DEG2SHORT_ANGLE2(-90.0f);
            stick = IDLE_WADE_STICK;
        } else if (dz > 0.0f && local.z > mFI_BK_WORLDSIZE_Z_F - IDLE_BORDER_APPROACH && dz > fabsf(dx) * 0.5f) {
            yaw = DEG2SHORT_ANGLE2(0.0f);
            stick = IDLE_WADE_STICK;
        } else if (dz < 0.0f && local.z < IDLE_BORDER_APPROACH && -dz > fabsf(dx) * 0.5f) {
            yaw = DEG2SHORT_ANGLE2(180.0f);
            stick = IDLE_WADE_STICK;
        }
    }

    /* world yaw = 270deg + stick angle + camera yaw (Player_actor_Get_ControllerAngle) */
    stick_angle = (s16)(yaw - getCamera2AngleY(play) - DEG2SHORT_ANGLE2(270.0f));
    mPlib_SetData1_controller_data_for_title_demo(0, 0, cos_s(stick_angle) * stick, sin_s(stick_angle) * stick);
}

/* ---- camera ---- */

void pc_idle_user_rotate(f32 degrees) {
    s_idle.user_yaw_target += degrees;
    if (s_idle.user_yaw_target > IDLE_USER_YAW_MAX) s_idle.user_yaw_target = IDLE_USER_YAW_MAX;
    if (s_idle.user_yaw_target < -IDLE_USER_YAW_MAX) s_idle.user_yaw_target = -IDLE_USER_YAW_MAX;
}

void pc_idle_user_zoom(f32 delta) {
    s_idle.user_zoom_target += delta;
    if (s_idle.user_zoom_target > IDLE_USER_ZOOM_MAX) s_idle.user_zoom_target = IDLE_USER_ZOOM_MAX;
    if (s_idle.user_zoom_target < IDLE_USER_ZOOM_MIN) s_idle.user_zoom_target = IDLE_USER_ZOOM_MIN;
}

void pc_idle_user_reset(void) {
    s_idle.user_yaw_target = 0.0f;
    s_idle.user_zoom_target = 1.0f;
}

/* Roughly where the four corners of the view land on the ground (a plane
 * at the camera target's height, stretched at the top for the world bend). */
static void idle_view_footprint(const xyz_t* eye, const xyz_t* center, f32 fov_y_deg, f32 aspect, f32 far,
                                f32* min_x, f32* max_x, f32* min_z) {
    f32 fx = center->x - eye->x, fy = center->y - eye->y, fz = center->z - eye->z;
    f32 len = sqrtf(fx * fx + fy * fy + fz * fz);
    f32 rx, rz, rlen, ux, uy, uz, tv, th;
    int sx, sy;

    fx /= len; fy /= len; fz /= len;
    /* right = forward x world-up, up = right x forward */
    rx = -fz; rz = fx;
    rlen = sqrtf(rx * rx + rz * rz);
    rx /= rlen; rz /= rlen;
    ux = -rz * fy; uy = rz * fx - rx * fz; uz = rx * fy;

    tv = tanf(fov_y_deg * 0.5f * (F_PI / 180.0f));
    th = tv * aspect;

    *min_x = 1e9f; *max_x = -1e9f; *min_z = 1e9f;
    for (sx = -1; sx <= 1; sx += 2) {
        for (sy = -1; sy <= 1; sy += 2) {
            f32 dx = fx + rx * sx * th + ux * sy * tv;
            f32 dy = fy + uy * sy * tv;
            f32 dz = fz + rz * sx * th + uz * sy * tv;
            f32 t = far;
            f32 px, pz;

            if (dy < -1e-4f) {
                t = (center->y - eye->y) / dy;
                /* the world bends away with distance, so the top of the
                 * screen actually reaches further than the flat hit */
                if (sy > 0) t *= IDLE_BEND_REACH;
                if (t > far) t = far;
            }
            px = eye->x + dx * t;
            pz = eye->z + dz * t;
            if (px < *min_x) *min_x = px;
            if (px > *max_x) *max_x = px;
            if (pz < *min_z) *min_z = pz;
        }
    }
}

int pc_idle_camera(GAME_PLAY* play, xyz_t* center, xyz_t* eye, f32* fov_y, f32* near, f32* far) {
    PLAYER_ACTOR* player;
    f32 dt, follow, ease, dist, pitch, yaw, horiz, fov, z, swing;
    f32 aspect, min_x, max_x, min_z, shift_x = 0.0f, shift_z = 0.0f;

    if (!pc_idle_active()) {
        s_idle.cam_init = FALSE;
        if (s_idle.shadow_hidden && GET_PLAYER_ACTOR(play) != NULL) {
            GET_PLAYER_ACTOR(play)->actor_class.shape_info.draw_shadow = TRUE;
            s_idle.shadow_hidden = FALSE;
        }
        return FALSE;
    }
    player = GET_PLAYER_ACTOR(play);
    if (player == NULL) return FALSE;

    dt = play->game.graph->dt;
    if (dt > 0.1f) dt = 0.1f;
    s_idle.time_sec += dt * idle_speed(); /* drives the slow sway/zoom drift */

    if (!s_idle.cam_init) {
        /* new scene or just went idle: start on the walker */
        s_idle.cam_center = player->actor_class.world.position;
        s_idle.user_yaw = s_idle.user_yaw_target;
        s_idle.user_zoom = s_idle.user_zoom_target;
        s_idle.cam_init = TRUE;
    } else {
        follow = 1.0f - expf(-dt * idle_speed() / (g_pc_timelapse ? IDLE_TL_FOLLOW_SEC : IDLE_CAM_FOLLOW_SEC));
        s_idle.cam_center.x += (player->actor_class.world.position.x - s_idle.cam_center.x) * follow;
        s_idle.cam_center.y += (player->actor_class.world.position.y - s_idle.cam_center.y) * follow;
        s_idle.cam_center.z += (player->actor_class.world.position.z - s_idle.cam_center.z) * follow;

        ease = 1.0f - expf(-dt / IDLE_USER_EASE_SEC);
        s_idle.user_yaw += (s_idle.user_yaw_target - s_idle.user_yaw) * ease;
        s_idle.user_zoom += (s_idle.user_zoom_target - s_idle.user_zoom) * ease;
    }

    /* zoom: 0..1 blends gameplay camera -> idle view, past 1 just backs off */
    z = s_idle.user_zoom;
    if (z <= 1.0f) {
        dist = GAME_CAM_DIST + (IDLE_CAM_DIST - GAME_CAM_DIST) * z;
        pitch = GAME_CAM_PITCH_DEG + (IDLE_CAM_PITCH_DEG - GAME_CAM_PITCH_DEG) * z;
        fov = GAME_CAM_FOV + (IDLE_CAM_FOV - GAME_CAM_FOV) * z;
        swing = z;
    } else {
        dist = IDLE_CAM_DIST * z;
        pitch = IDLE_CAM_PITCH_DEG;
        fov = IDLE_CAM_FOV;
        swing = 1.0f;
    }

    /* slow, out-of-phase drifts so the framing never visibly loops */
    dist += swing * IDLE_CAM_DIST_SWING * sinf(s_idle.time_sec * (2.0f * F_PI / 170.0f));
    pitch = (pitch + swing * 3.0f * sinf(s_idle.time_sec * (2.0f * F_PI / 230.0f))) * (F_PI / 180.0f);
    yaw = (s_idle.user_yaw + IDLE_CAM_YAW_SWING * sinf(s_idle.time_sec * (2.0f * F_PI / 300.0f))) * (F_PI / 180.0f);
    *center = s_idle.cam_center;
    horiz = dist * cosf(pitch);

    /* the gameplay camera looks north (-z) from the south; keep that, swayed */
    eye->x = center->x + horiz * sinf(yaw);
    eye->y = center->y + dist * sinf(pitch);
    eye->z = center->z + horiz * cosf(yaw);

    /* slide the whole view back inside the map if it sees past an edge */
    aspect = (g_pc_window_h > 0) ? (f32)g_pc_window_w / (f32)g_pc_window_h : (16.0f / 9.0f);
    idle_view_footprint(eye, center, fov, aspect, IDLE_CAM_FAR, &min_x, &max_x, &min_z);
    if (max_x - min_x > IDLE_MAP_MAX_X - IDLE_MAP_MIN_X - 2.0f * IDLE_MAP_MARGIN) {
        shift_x = (IDLE_MAP_MIN_X + IDLE_MAP_MAX_X) * 0.5f - (min_x + max_x) * 0.5f;
    } else if (min_x < IDLE_MAP_MIN_X + IDLE_MAP_MARGIN) {
        shift_x = IDLE_MAP_MIN_X + IDLE_MAP_MARGIN - min_x;
    } else if (max_x > IDLE_MAP_MAX_X - IDLE_MAP_MARGIN) {
        shift_x = IDLE_MAP_MAX_X - IDLE_MAP_MARGIN - max_x;
    }
    if (min_z < IDLE_MAP_MIN_Z + IDLE_MAP_MARGIN) {
        shift_z = IDLE_MAP_MIN_Z + IDLE_MAP_MARGIN - min_z;
    }
    center->x += shift_x; eye->x += shift_x;
    center->z += shift_z; eye->z += shift_z;

    *fov_y = fov;
    *near = IDLE_CAM_NEAR;
    *far = IDLE_CAM_FAR;
    return TRUE;
}

void pc_idle_get_clock(int* hour, int* min, int* sec, int* weekday, int* month, int* day) {
    lbRTC_time_c* t = Common_GetPointer(time.rtc_time);

    *hour = t->hour;
    *min = t->min;
    *sec = t->sec;
    *weekday = t->weekday;
    *month = t->month;
    *day = t->day;
}
