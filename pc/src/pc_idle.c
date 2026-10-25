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

#include "m_player_lib.h"
#include "m_event.h"
#include "m_field_info.h"
#include "m_field_make.h"
#include "m_random_field_h.h"
#include "m_actor.h"
#include "sys_math.h"
#include "m_lib.h"

/* camera framing: a high three-quarter view, much wider than the 20-degree,
 * ~1240-unit gameplay camera */
#define IDLE_CAM_DIST       1500.0f
#define IDLE_CAM_DIST_SWING 150.0f   /* slow zoom breathing */
#define IDLE_CAM_PITCH_DEG  57.0f    /* elevation above the ground plane */
#define IDLE_CAM_YAW_SWING  14.0f    /* degrees of slow side-to-side sway */
#define IDLE_CAM_FOV        28.0f
#define IDLE_CAM_NEAR       200.0f
#define IDLE_CAM_FAR        4000.0f
#define IDLE_CAM_FOLLOW_SEC 2.5f     /* center lag behind the walker */

/* autopilot */
#define IDLE_WALK_STICK     34.0f    /* stick magnitude: an unhurried walk */
#define IDLE_WADE_STICK     61.0f    /* full tilt (STICK_MAX), needed to trigger an acre crossing */
#define IDLE_BORDER_APPROACH 60.0f   /* square up this close to a block border */
#define IDLE_ARRIVE_DIST    60.0f
#define IDLE_NPC_STANDOFF   90.0f    /* stop this far from a villager */
#define IDLE_STUCK_SEC      3.0f
#define IDLE_STUCK_PROGRESS 40.0f
#define IDLE_NPC_CHANCE     0.45f
#define IDLE_NPC_MIN_DIST   250.0f   /* skip villagers we're already beside */

/* keep the camera center far enough inside the map that the view never shows
 * the void past the border cliffs (the gameplay camera can't see that far) */
#define IDLE_CAM_MIN_X      (mFI_BK_WORLDSIZE_X_F * 1.55f)
#define IDLE_CAM_MAX_X      (mFI_BK_WORLDSIZE_X_F * (FG_BLOCK_X_NUM + 0.3f))
#define IDLE_CAM_MIN_Z      (mFI_BK_WORLDSIZE_Z_F * 1.55f)

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
} pc_idle_state;

static pc_idle_state s_idle;

int pc_idle_active(void) {
    return g_pc_wallpaper_active && mEv_IsTitleDemo();
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

        if (fqrand() < 0.7f) {
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
    s_idle.target_npc = (fqrand() < IDLE_NPC_CHANCE) ? idle_pick_npc(play, pos) : NULL;
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
        s_idle.wait_sec = (s_idle.target_npc != NULL ? 5.0f : 1.5f) + RANDOM_F(s_idle.target_npc != NULL ? 6.0f : 3.0f);
        mPlib_SetData1_controller_data_for_title_demo(0, 0, 0.0f, 0.0f);
        return;
    }

    /* give up on targets behind rivers, cliffs and buildings */
    if (dist < s_idle.best_dist - IDLE_STUCK_PROGRESS) {
        s_idle.best_dist = dist;
        s_idle.stuck_sec = 0.0f;
    } else {
        s_idle.stuck_sec += dt;
        if (s_idle.stuck_sec > IDLE_STUCK_SEC) {
            s_idle.have_target = FALSE;
            s_idle.wait_sec = 1.0f + RANDOM_F(2.0f);
        }
    }

    yaw = atans_table(dz, dx);
    stick = IDLE_WALK_STICK;

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

int pc_idle_camera(GAME_PLAY* play, xyz_t* center, xyz_t* eye, f32* fov_y, f32* near, f32* far) {
    PLAYER_ACTOR* player;
    f32 dt, follow, dist, pitch, yaw, horiz;

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
    s_idle.time_sec += dt;

    if (!s_idle.cam_init) {
        /* new scene or just went idle: start on the walker */
        s_idle.cam_center = player->actor_class.world.position;
        s_idle.cam_init = TRUE;
    } else {
        follow = 1.0f - expf(-dt / IDLE_CAM_FOLLOW_SEC);
        s_idle.cam_center.x += (player->actor_class.world.position.x - s_idle.cam_center.x) * follow;
        s_idle.cam_center.y += (player->actor_class.world.position.y - s_idle.cam_center.y) * follow;
        s_idle.cam_center.z += (player->actor_class.world.position.z - s_idle.cam_center.z) * follow;
    }

    /* slow, out-of-phase drifts so the framing never visibly loops */
    dist = IDLE_CAM_DIST + IDLE_CAM_DIST_SWING * sinf(s_idle.time_sec * (2.0f * F_PI / 170.0f));
    pitch = (IDLE_CAM_PITCH_DEG + 3.0f * sinf(s_idle.time_sec * (2.0f * F_PI / 230.0f))) * (F_PI / 180.0f);
    yaw = IDLE_CAM_YAW_SWING * sinf(s_idle.time_sec * (2.0f * F_PI / 300.0f)) * (F_PI / 180.0f);
    horiz = dist * cosf(pitch);

    *center = s_idle.cam_center;
    if (center->x < IDLE_CAM_MIN_X) center->x = IDLE_CAM_MIN_X;
    if (center->x > IDLE_CAM_MAX_X) center->x = IDLE_CAM_MAX_X;
    if (center->z < IDLE_CAM_MIN_Z) center->z = IDLE_CAM_MIN_Z;
    /* the gameplay camera looks north (-z) from the south; keep that, swayed */
    eye->x = center->x + horiz * sinf(yaw);
    eye->y = center->y + dist * sinf(pitch);
    eye->z = center->z + horiz * cosf(yaw);
    *fov_y = IDLE_CAM_FOV;
    *near = IDLE_CAM_NEAR;
    *far = IDLE_CAM_FAR;
    return TRUE;
}
