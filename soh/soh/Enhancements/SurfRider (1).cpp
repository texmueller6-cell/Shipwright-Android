// ShieldRider.cpp
//
// Shield Rider - Ship of Harkinian Android 9.0.2
//
// Adjustments:
// - Enemies touched while surfing are knocked away, tumbling, then burst (bosses excepted).
// - Bushes, pots, rocks (small, large, bombable, hammer) and crates are smashed while surfing.
// - Fire trail behind Link kills enemies that chase him.
// - Bullet time: hold L and enemies move at half speed.
// - Low obstacles (fences, crates) are jumped automatically.
// - Wall contact bounces Link back (reflects), speed is kept.
// - Speed is kept on ramps (uphill assist, glued to floor downhill).
// - Ledges make Link hop and keep the surf pose until landing.
// - Link surfs over water.
// - Tighter, smoother turns.
// - Surfing continues while R is held.
//
// Based on the original ShieldRider.cpp.

#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "assets/objects/gameplay_keep/gameplay_keep.h"
#include "assets/objects/object_gi_shield_1/object_gi_shield_1.h"
#include "assets/objects/object_gi_shield_2/object_gi_shield_2.h"
#include "assets/objects/object_gi_shield_3/object_gi_shield_3.h"
#include "soh/frame_interpolation.h"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"

extern s32 Player_SetupAction(PlayState* play, Player* player, PlayerActionFunc actionFunc, s32 flags);
extern void Player_AnimChangeLoopMorph(PlayState* play, Player* player, LinkAnimationHeader* anim);
extern void func_80832564(PlayState* play, Player* player);
extern void func_8083A098(Player* player, LinkAnimationHeader* anim, PlayState* play);
extern void func_80837B9C(Player* player, PlayState* play);
extern s32 func_8084269C(PlayState* play, Player* player);
extern void Player_Draw(Actor* thisx, PlayState* play);
extern PlayState* gPlayState;
}

#define CVAR_SHIELDRIDER_NAME CVAR_ENHANCEMENT("ShieldRider")
#define CVAR_SHIELDRIDER_DEFAULT 1
#define CVAR_SHIELDRIDER_VALUE CVarGetInteger(CVAR_SHIELDRIDER_NAME, CVAR_SHIELDRIDER_DEFAULT)

// Movement settings (speeds doubled)
static constexpr f32 RIDER_MIN_START_SPEED = 5.0f;
static constexpr f32 RIDER_START_BOOST = 4.0f;
static constexpr f32 RIDER_ACCEL = 0.5f;
static constexpr f32 RIDER_MAX_SPEED = 44.0f;

// Wall bounce
static constexpr f32 RIDER_BOUNCE_KEEP = 0.85f;      // speed kept after a bounce
static constexpr f32 RIDER_BOUNCE_MIN_SPEED = 10.0f; // never bounce slower than this
static constexpr s32 RIDER_BOUNCE_COOLDOWN = 8;      // frames before the next bounce
static constexpr f32 RIDER_WALL_MAX_NORMAL_Y = 0.3f; // above this it is a steep slope, not a wall

// Edge jump / slopes
static constexpr f32 RIDER_EDGE_JUMP_VEL = 12.0f;    // upward hop when leaving a ledge
static constexpr f32 RIDER_GROUND_SNAP = 40.0f;      // base drop that still counts as a slope (+ current speed)
static constexpr f32 RIDER_RISE_BASE = 8.0f;         // uphill assist: base rise per frame
static constexpr f32 RIDER_RISE_PER_SPEED = 0.6f;    // uphill assist: extra rise per unit of speed

// Obstacle jump (fences, crates, low walls)
static constexpr f32 RIDER_OBSTACLE_JUMP_VEL = 26.0f;     // upward speed of the jump (~84 units high)
static constexpr f32 RIDER_OBSTACLE_LOW_Y = 22.0f;        // low probe height: something is in the way
static constexpr f32 RIDER_OBSTACLE_TOP_Y = 80.0f;        // top probe height: taller than this = real wall
static constexpr f32 RIDER_OBSTACLE_LOOKAHEAD_FRAMES = 4.0f;
static constexpr f32 RIDER_OBSTACLE_MIN_LOOKAHEAD = 90.0f;
static constexpr s32 RIDER_OBSTACLE_COOLDOWN = 6;
static constexpr s32 RIDER_OBSTACLE_NO_BOUNCE_FRAMES = 14; // ignore wall bounce while clearing the obstacle

// Ramming enemies
static constexpr f32 RIDER_RAM_REACH = 40.0f;      // base reach (+ enemy radius + current speed)
static constexpr f32 RIDER_RAM_HEIGHT = 100.0f;    // vertical tolerance

// Knock-away animation
static constexpr s32 RIDER_LAUNCH_MAX = 12;
static constexpr s32 RIDER_LAUNCH_FRAMES = 14;
static constexpr f32 RIDER_LAUNCH_SPEED = 14.0f;     // base horizontal speed (+ half of Link's speed)
static constexpr f32 RIDER_LAUNCH_UP = 14.0f;
static constexpr f32 RIDER_LAUNCH_GRAVITY = 2.0f;

// Prop smashing (bushes, pots, rocks, crates)
static constexpr f32 RIDER_BUSH_REACH = 45.0f;       // + 0.6 * Link's speed
static constexpr f32 RIDER_BUSH_HEIGHT = 80.0f;

// Fire trail
static constexpr s32 RIDER_TRAIL_POINTS = 16;
static constexpr s32 RIDER_TRAIL_LIFE = 40;          // frames a flame stays on the ground
static constexpr f32 RIDER_TRAIL_REACH = 50.0f;      // how close an enemy must be to a flame to die
static constexpr f32 RIDER_TRAIL_HEIGHT = 60.0f;

// Bullet time (hold this button: enemies update every other frame)
#define RIDER_BULLET_BUTTON BTN_L

static constexpr s32 RIDER_STICK_DEADZONE = 20;

static constexpr s16 RIDER_LEAN_BASE = 0x0800;
static constexpr f32 RIDER_LEAN_PER_SPEED = 40.0f;
static constexpr s16 RIDER_LEAN_MAX = 0x1800;
static constexpr f32 RIDER_BANK = 45.0f;
static constexpr s16 RIDER_TILT_STEP = 500;
static constexpr f32 RIDER_STEER = 18.0f;           // tighter turns
static constexpr f32 RIDER_STEER_SMOOTH = 0.5f;     // 0..1, lower = smoother stick response

static constexpr s32 RIDER_AIR_GRACE_FRAMES = 240;  // only a safety limit; R release ends surf

static constexpr f32 RIDER_SHIELD_SCALE = 0.012f;
static constexpr f32 RIDER_SHIELD_HEIGHT = 1.5f;
static constexpr s16 RIDER_SHIELD_PITCH = -0x4000;

#define RIDER_ANIM gPlayerAnim_link_normal_down_slope_slip

static f32 sRiderSpeed;
static s32 sAirFrames;
static s32 sBounceCooldown;
static bool sWasOnGround;
static bool sLaunched;
static f32 sSteer;
static s32 sObstacleCooldown;
static s32 sNoBounceFrames;
static bool sRiderActive;
static f32 sSavedGravity = -4.0f;
static f32 sSavedMinVelY = -20.0f;
static bool sFakeGround;
static f32 sFakeGroundY;
struct RiderLaunch {
    Actor* actor;
    Vec3f vel;
    s32 frames;
};
static RiderLaunch sLaunches[RIDER_LAUNCH_MAX];
static Vec3f sTrailPos[RIDER_TRAIL_POINTS];
static s32 sTrailLife[RIDER_TRAIL_POINTS];
static s32 sTrailHead;

static void ShieldRider_Action(Player* player, PlayState* play);
static void ShieldRider_RestorePhysics(Player* player);

// Ignores small stick values (drift / miscalibrated analog) and rescales the rest.
static f32 ShieldRider_StickX(Input* input) {
    s32 x = input->rel.stick_x;
    s32 ax = x < 0 ? -x : x;

    if (ax <= RIDER_STICK_DEADZONE) {
        return 0.0f;
    }

    f32 v = (f32)(ax - RIDER_STICK_DEADZONE);
    return x < 0 ? -v : v;
}

// Draw Link and shield
static void ShieldRider_Draw(Actor* thisx, PlayState* play) {
    Player* player = (Player*)thisx;

    Player_Draw(thisx, play);

    if (player->actionFunc != ShieldRider_Action) {
        thisx->draw = Player_Draw;
        thisx->shape.rot.x = 0;
        thisx->shape.rot.z = 0;
        return;
    }

    Gfx* shieldDl = nullptr;
    Gfx* mirrorSymbolDl = nullptr;

    switch (player->currentShield) {
        case PLAYER_SHIELD_DEKU:
            shieldDl = (Gfx*)gGiDekuShieldDL;
            break;

        case PLAYER_SHIELD_HYLIAN:
            shieldDl = (Gfx*)gGiHylianShieldDL;
            break;

        case PLAYER_SHIELD_MIRROR:
            shieldDl = (Gfx*)gGiMirrorShieldDL;
            mirrorSymbolDl = (Gfx*)gGiMirrorShieldSymbolDL;
            break;

        default:
            return;
    }

    Vec3s rot = { RIDER_SHIELD_PITCH, thisx->shape.rot.y, 0 };

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    FrameInterpolation_RecordActorPosRotMatrix();

    Matrix_SetTranslateRotateYXZ(
        thisx->world.pos.x,
        thisx->world.pos.y + RIDER_SHIELD_HEIGHT,
        thisx->world.pos.z,
        &rot
    );

    Matrix_Scale(
        RIDER_SHIELD_SCALE,
        RIDER_SHIELD_SCALE,
        RIDER_SHIELD_SCALE,
        MTXMODE_APPLY
    );

    if (mirrorSymbolDl != nullptr) {
        gSPSegment(
            POLY_OPA_DISP++,
            0x08,
            (uintptr_t)Gfx_TwoTexScroll(
                play->state.gfxCtx,
                0, 0, (play->state.frames * 2) % 256, 64, 64,
                1, 0, (play->state.frames * 1) % 128, 32, 32
            )
        );
    }

    gSPMatrix(
        POLY_OPA_DISP++,
        MATRIX_NEWMTX(play->state.gfxCtx),
        G_MTX_MODELVIEW | G_MTX_LOAD
    );

    gSPDisplayList(POLY_OPA_DISP++, shieldDl);

    if (mirrorSymbolDl != nullptr) {
        Gfx_SetupDL_25Xlu(play->state.gfxCtx);

        gSPMatrix(
            POLY_XLU_DISP++,
            MATRIX_NEWMTX(play->state.gfxCtx),
            G_MTX_MODELVIEW | G_MTX_LOAD
        );

        gSPDisplayList(POLY_XLU_DISP++, mirrorSymbolDl);
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

static void ShieldRider_Stop(Player* player, PlayState* play, bool grounded) {
    f32 exitSpeed = sRiderSpeed;
    sRiderSpeed = 0.0f;
    sAirFrames = 0;

    ShieldRider_RestorePhysics(player);

    if (player->actor.draw == ShieldRider_Draw) {
        player->actor.draw = Player_Draw;
    }

    player->actor.shape.rot.x = 0;
    player->actor.shape.rot.z = 0;

    if (grounded) {
        player->linearVelocity = 0.0f;

        func_8083A098(
            player,
            (LinkAnimationHeader*)gPlayerAnim_link_normal_down_slope_slip_end,
            play
        );
    } else {
        player->linearVelocity = exitSpeed;
        func_80837B9C(player, play);
    }
}

// Fire / smoke coming out of both boots.
static void ShieldRider_SpawnBootFire(Player* player, PlayState* play) {
    static Color_RGBA8 firePrim = { 255, 170, 40, 255 };
    static Color_RGBA8 fireEnv = { 255, 60, 0, 0 };
    static Color_RGBA8 smokePrim = { 110, 110, 110, 200 };
    static Color_RGBA8 smokeEnv = { 40, 40, 40, 0 };

    f32 s = Math_SinS(player->actor.shape.rot.y);
    f32 c = Math_CosS(player->actor.shape.rot.y);

    for (s32 side = -1; side <= 1; side += 2) {
        Vec3f pos = {
            player->actor.world.pos.x - s * 10.0f + c * side * 6.0f,
            player->actor.world.pos.y + 4.0f,
            player->actor.world.pos.z - c * 10.0f - s * side * 6.0f,
        };
        Vec3f vel = { -s * 3.0f, 1.0f, -c * 3.0f };
        Vec3f accel = { 0.0f, 0.4f, 0.0f };

        EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, &firePrim, &fireEnv, 90, 12, 9, 0);

        if ((play->state.frames & 1) == 0) {
            pos.y += 3.0f;
            EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, &smokePrim, &smokeEnv, 120, 20, 14, 0);
        }
    }
}

// Enemies that must not be removed abruptly (they carry puzzle / progress logic).
static bool ShieldRider_IsProtectedEnemy(Actor* actor) {
    switch (actor->id) {
        case ACTOR_EN_SW:          // Gold Skulltula (drops its token on a normal death)
        case ACTOR_EN_PO_SISTERS:  // Poe Sisters (door / puzzle logic)
            return true;
        default:
            return false;
    }
}

static void ShieldRider_KillEnemy(Actor* actor, PlayState* play) {
    static Color_RGBA8 puffPrim = { 255, 220, 120, 255 };
    static Color_RGBA8 puffEnv = { 255, 80, 0, 0 };

    Vec3f pos = actor->world.pos;
    Vec3f vel = { 0.0f, 2.0f, 0.0f };
    Vec3f accel = { 0.0f, 0.3f, 0.0f };

    pos.y += 15.0f;
    EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, &puffPrim, &puffEnv, 200, 30, 12, 0);

    Item_DropCollectibleRandom(play, nullptr, &actor->world.pos, 0x30);
    Actor_Kill(actor);
}

static bool ShieldRider_IsLaunched(Actor* actor) {
    for (s32 i = 0; i < RIDER_LAUNCH_MAX; i++) {
        if (sLaunches[i].actor == actor) {
            return true;
        }
    }
    return false;
}

// Burst of puffs flying outward from a point.
static void ShieldRider_Burst(PlayState* play, Vec3f* center, Color_RGBA8* prim, Color_RGBA8* env, f32 power) {
    for (s32 i = 0; i < 6; i++) {
        s16 angle = (s16)(i * 0x2AAA);
        Vec3f pos = *center;
        Vec3f vel = { Math_SinS(angle) * power, power * 0.8f, Math_CosS(angle) * power };
        Vec3f accel = { 0.0f, -0.2f, 0.0f };

        EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, prim, env, 160, 20, 14, 0);
    }
}

// Knocks an enemy away (tumbling) instead of removing it instantly.
static void ShieldRider_LaunchEnemy(Actor* actor, Player* player, PlayState* play) {
    static Color_RGBA8 hitPrim = { 255, 255, 200, 255 };
    static Color_RGBA8 hitEnv = { 255, 120, 0, 0 };

    if (ShieldRider_IsLaunched(actor)) {
        return;
    }

    s32 slot = -1;

    for (s32 i = 0; i < RIDER_LAUNCH_MAX; i++) {
        if (sLaunches[i].actor == nullptr) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        ShieldRider_KillEnemy(actor, play);
        return;
    }

    // Away from Link, biased towards where Link is heading.
    s16 away = (s16)(actor->yawTowardsPlayer + 0x8000);
    s16 dir = (s16)(player->yaw + (s16)(away - player->yaw) / 2);
    f32 speed = RIDER_LAUNCH_SPEED + sRiderSpeed * 0.5f;

    sLaunches[slot].actor = actor;
    sLaunches[slot].vel.x = Math_SinS(dir) * speed;
    sLaunches[slot].vel.y = RIDER_LAUNCH_UP;
    sLaunches[slot].vel.z = Math_CosS(dir) * speed;
    sLaunches[slot].frames = RIDER_LAUNCH_FRAMES;

    Vec3f center = actor->world.pos;
    center.y += 20.0f;
    ShieldRider_Burst(play, &center, &hitPrim, &hitEnv, 5.0f);
}

// Moves / tumbles launched enemies; when the flight ends they burst and die.
static void ShieldRider_UpdateLaunches(PlayState* play) {
    for (s32 i = 0; i < RIDER_LAUNCH_MAX; i++) {
        Actor* actor = sLaunches[i].actor;

        if (actor == nullptr) {
            continue;
        }

        // Make sure the enemy still exists (scene change, killed by something else...).
        bool alive = false;
        for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next) {
            if (a == actor) {
                alive = actor->update != nullptr;
                break;
            }
        }

        if (!alive) {
            sLaunches[i].actor = nullptr;
            continue;
        }

        if (sLaunches[i].frames <= 0) {
            actor->scale.x = actor->scale.y = actor->scale.z = 0.0f;
            ShieldRider_KillEnemy(actor, play);
            sLaunches[i].actor = nullptr;
            continue;
        }

        // Keep the enemy's own logic frozen while we fly it.
        actor->freezeTimer = 1;

        actor->world.pos.x += sLaunches[i].vel.x;
        actor->world.pos.y += sLaunches[i].vel.y;
        actor->world.pos.z += sLaunches[i].vel.z;
        sLaunches[i].vel.y -= RIDER_LAUNCH_GRAVITY;

        actor->shape.rot.x += 0x1C00;
        actor->shape.rot.z += 0x1000;

        if (sLaunches[i].frames <= 5) {
            actor->scale.x *= 0.8f;
            actor->scale.y *= 0.8f;
            actor->scale.z *= 0.8f;
        }

        sLaunches[i].frames--;
    }
}

// Smashes bushes, pots, rocks (small, large, bombable, hammer) and crates Link passes through.
static void ShieldRider_SmashProps(Player* player, PlayState* play) {
    static Color_RGBA8 leafPrim = { 110, 210, 70, 255 };
    static Color_RGBA8 leafEnv = { 30, 110, 20, 0 };
    static Color_RGBA8 clayPrim = { 205, 140, 90, 255 };
    static Color_RGBA8 clayEnv = { 120, 70, 30, 0 };
    static Color_RGBA8 rockPrim = { 180, 180, 175, 255 };
    static Color_RGBA8 rockEnv = { 80, 80, 80, 0 };
    static Color_RGBA8 woodPrim = { 195, 145, 85, 255 };
    static Color_RGBA8 woodEnv = { 100, 60, 25, 0 };

    static const s32 categories[] = { ACTORCAT_PROP, ACTORCAT_BG };
    f32 baseReach = RIDER_BUSH_REACH + sRiderSpeed * 0.6f;

    for (s32 c = 0; c < 2; c++) {
        Actor* actor = play->actorCtx.actorLists[categories[c]].head;

        while (actor != nullptr) {
            Actor* next = actor->next;

            Color_RGBA8* prim = nullptr;
            Color_RGBA8* env = nullptr;
            f32 power = 4.0f;
            f32 extraReach = 0.0f;
            f32 height = RIDER_BUSH_HEIGHT;
            bool big = false;

            switch (actor->id) {
                case ACTOR_EN_KUSA:      // bushes / grass
                    prim = &leafPrim;
                    env = &leafEnv;
                    break;
                case ACTOR_OBJ_TSUBO:    // pots
                    prim = &clayPrim;
                    env = &clayEnv;
                    power = 5.0f;
                    break;
                case ACTOR_EN_ISHI:      // small rocks, and large (silver gauntlets) rocks
                    prim = &rockPrim;
                    env = &rockEnv;
                    power = 5.0f;
                    if ((actor->params & 1) != 0) {
                        big = true;
                    }
                    break;
                case ACTOR_OBJ_BOMBIWA:  // bombable boulders
                case ACTOR_OBJ_HAMISHI:  // hammer boulders
                    prim = &rockPrim;
                    env = &rockEnv;
                    power = 5.0f;
                    big = true;
                    break;
                case ACTOR_OBJ_KIBAKO:   // small crates
                case ACTOR_OBJ_KIBAKO2:  // large crates
                    prim = &woodPrim;
                    env = &woodEnv;
                    power = 5.5f;
                    break;
                default:
                    break;
            }

            if (big) {
                extraReach = 35.0f;
                height = 140.0f;
                power = 7.0f;
            }

            if (
                prim != nullptr &&
                actor->update != nullptr &&
                actor->xzDistToPlayer < baseReach + extraReach &&
                ABS(actor->yDistToPlayer) < height
            ) {
                Vec3f center = actor->world.pos;
                center.y += big ? 30.0f : 10.0f;

                ShieldRider_Burst(play, &center, prim, env, power);

                if (big) {
                    ShieldRider_Burst(play, &center, prim, env, power * 0.5f);
                }

                Item_DropCollectibleRandom(play, nullptr, &actor->world.pos, 0x30);
                Actor_Kill(actor);
            }

            actor = next;
        }
    }
}

// Kills every (non-boss) enemy Link touches while surfing, like a trampling horse.
static void ShieldRider_RamEnemies(Player* player, PlayState* play) {
    // Bosses live in ACTORCAT_BOSS, so only regular enemies are checked here.
    Actor* actor = play->actorCtx.actorLists[ACTORCAT_ENEMY].head;

    while (actor != nullptr) {
        Actor* next = actor->next;

        if (
            actor->update != nullptr &&
            actor->colChkInfo.health > 0 &&
            !ShieldRider_IsProtectedEnemy(actor)
        ) {
            f32 reach = RIDER_RAM_REACH + (f32)actor->colChkInfo.cylRadius + sRiderSpeed;

            if (actor->xzDistToPlayer < reach && ABS(actor->yDistToPlayer) < RIDER_RAM_HEIGHT) {
                ShieldRider_LaunchEnemy(actor, player, play);
            }
        }

        actor = next;
    }
}

// Fire trail: flames stay on the ground behind Link and kill enemies that touch them.
static void ShieldRider_UpdateTrail(Player* player, PlayState* play, bool onGround) {
    static Color_RGBA8 flamePrim = { 255, 140, 20, 255 };
    static Color_RGBA8 flameEnv = { 255, 40, 0, 0 };

    if (onGround) {
        sTrailPos[sTrailHead] = player->actor.world.pos;
        sTrailLife[sTrailHead] = RIDER_TRAIL_LIFE;
        sTrailHead = (sTrailHead + 1) % RIDER_TRAIL_POINTS;
    }

    for (s32 i = 0; i < RIDER_TRAIL_POINTS; i++) {
        if (sTrailLife[i] <= 0) {
            continue;
        }

        sTrailLife[i]--;

        // Flame visual (not every frame, to save effect slots)
        if (((play->state.frames + i) & 3) == 0) {
            Vec3f vel = { 0.0f, 1.5f, 0.0f };
            Vec3f accel = { 0.0f, 0.2f, 0.0f };
            Vec3f pos = sTrailPos[i];

            pos.y += 4.0f;
            EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, &flamePrim, &flameEnv, 140, 8, 14, 0);
        }

        // Enemies touching a flame die.
        Actor* actor = play->actorCtx.actorLists[ACTORCAT_ENEMY].head;

        while (actor != nullptr) {
            Actor* next = actor->next;

            if (
                actor->update != nullptr &&
                actor->colChkInfo.health > 0 &&
                !ShieldRider_IsProtectedEnemy(actor)
            ) {
                f32 dx = actor->world.pos.x - sTrailPos[i].x;
                f32 dz = actor->world.pos.z - sTrailPos[i].z;
                f32 dy = actor->world.pos.y - sTrailPos[i].y;
                f32 reach = RIDER_TRAIL_REACH + (f32)actor->colChkInfo.cylRadius;

                if (dx * dx + dz * dz < reach * reach && ABS(dy) < RIDER_TRAIL_HEIGHT) {
                    ShieldRider_LaunchEnemy(actor, player, play);
                }
            }

            actor = next;
        }
    }
}

// Bullet time: while the button is held, enemies only update every other frame.
static void ShieldRider_BulletTime(PlayState* play) {
    if (!CHECK_BTN_ALL(play->state.input[0].cur.button, RIDER_BULLET_BUTTON)) {
        return;
    }

    if ((play->state.frames & 1) != 0) {
        return;
    }

    Actor* actor = play->actorCtx.actorLists[ACTORCAT_ENEMY].head;

    while (actor != nullptr) {
        actor->freezeTimer = 1;
        actor = actor->next;
    }
}

// Highest ground (floor or water surface) at a point, searching down from fromY.
static f32 ShieldRider_GroundAheadY(PlayState* play, f32 x, f32 z, f32 fromY, bool* isWater) {
    Vec3f p = { x, fromY, z };
    CollisionPoly poly;
    f32 floorY = BgCheck_AnyRaycastFloor1(&play->colCtx, &poly, &p);
    f32 surfaceY;
    WaterBox* waterBox;

    *isWater = false;

    if (
        WaterBox_GetSurface1(play, &play->colCtx, x, z, &surfaceY, &waterBox) &&
        surfaceY <= fromY &&
        surfaceY >= floorY
    ) {
        *isWater = true;
        return surfaceY;
    }

    return floorY;
}

// Gives back the physics values we changed while surfing.
static void ShieldRider_RestorePhysics(Player* player) {
    if (sRiderActive) {
        player->actor.gravity = sSavedGravity;
        player->actor.minVelocityY = sSavedMinVelY;
        sRiderActive = false;
    }

    sFakeGround = false;
}

// True if a wall blocks the line ahead of Link at the given height.
static bool ShieldRider_WallAhead(PlayState* play, Player* player, f32 height, f32 dist) {
    Vec3f from = {
        player->actor.world.pos.x,
        player->actor.world.pos.y + height,
        player->actor.world.pos.z,
    };
    Vec3f to = {
        from.x + Math_SinS(player->yaw) * dist,
        from.y,
        from.z + Math_CosS(player->yaw) * dist,
    };
    Vec3f hit;
    CollisionPoly* poly = nullptr;
    s32 bgId;

    return BgCheck_EntityLineTest1(
        &play->colCtx, &from, &to, &hit, &poly, true, false, false, true, &bgId
    ) != 0;
}

static void ShieldRider_Action(Player* player, PlayState* play) {
    Input* input = &play->state.input[0];

    bool onGround = (player->actor.bgCheckFlags & 1) != 0;
    bool hitWall = (player->actor.bgCheckFlags & 8) != 0;

    player->stateFlags2 |=
        PLAYER_STATE2_DISABLE_ROTATION_Z_TARGET |
        PLAYER_STATE2_DISABLE_ROTATION_ALWAYS;

    LinkAnimation_Update(play, &player->skelAnime);

    if (player->actor.draw == Player_Draw) {
        player->actor.draw = ShieldRider_Draw;
    }

    if (sBounceCooldown > 0) {
        sBounceCooldown--;
    }

    if (sObstacleCooldown > 0) {
        sObstacleCooldown--;
    }

    if (sNoBounceFrames > 0) {
        sNoBounceFrames--;
    }

    // Ground = floor or water surface, whichever is higher.
    f32 groundY = player->actor.floorHeight;
    bool onWater = false;
    {
        f32 surfaceY;
        WaterBox* waterBox;

        if (
            WaterBox_GetSurface1(
                play, &play->colCtx,
                player->actor.world.pos.x, player->actor.world.pos.z,
                &surfaceY, &waterBox
            ) &&
            surfaceY >= groundY - 0.5f &&
            surfaceY <= player->actor.world.pos.y + 30.0f
        ) {
            groundY = surfaceY;
            onWater = true;
        }
    }

    // We glide down steep slopes, so lift the engine's fall-speed cap.
    player->actor.minVelocityY = -200.0f;

    // Slopes, ledges and water.
    if (!onGround) {
        f32 drop = player->actor.world.pos.y - groundY;
        bool falling = player->actor.velocity.y <= 0.0f;
        f32 snapDist = RIDER_GROUND_SNAP + sRiderSpeed;

        if (sLaunched) {
            if (onWater && falling && drop <= 0.0f) {
                // Came down on the water: stand on it.
                player->actor.world.pos.y = groundY;
                player->actor.velocity.y = 0.0f;
                onGround = true;
            }
        } else if (drop >= -60.0f && drop <= snapDist) {
            // Slope, small drop or water just below: stay on it and keep sliding.
            player->actor.world.pos.y = groundY;
            player->actor.velocity.y = 0.0f;
            onGround = true;
        } else if (sWasOnGround) {
            // Real ledge: hop off it at full speed (surf pose is kept).
            player->actor.velocity.y = RIDER_EDGE_JUMP_VEL;
            sLaunched = true;
        }
    }

    if (onGround) {
        sLaunched = false;
    }

    // Standing on water: no gravity (so we do not sink) and a tiny lift above the surface.
    bool standingOnWater = onGround && onWater;

    if (standingOnWater) {
        player->actor.gravity = 0.0f;
        player->actor.velocity.y = 0.0f;
        player->actor.world.pos.y = groundY + 1.0f;
        player->stateFlags1 &= ~PLAYER_STATE1_IN_WATER;
    } else {
        player->actor.gravity = sSavedGravity;
    }

    sFakeGround = standingOnWater;
    sFakeGroundY = groundY;

    sWasOnGround = onGround;
    sAirFrames = onGround ? 0 : sAirFrames + 1;

    // Safety limit only; normally surfing continues in the air until landing.
    if (sAirFrames > RIDER_AIR_GRACE_FRAMES) {
        ShieldRider_Stop(player, play, false);
        return;
    }

    // Only releasing R ends surfing.
    if (!CHECK_BTN_ALL(input->cur.button, BTN_R)) {
        ShieldRider_Stop(player, play, onGround);
        return;
    }

    // Always accelerate (nothing slows us down, walls bounce us).
    sRiderSpeed = MIN(sRiderSpeed + RIDER_ACCEL, RIDER_MAX_SPEED);

    // Smoothed stick input (less camera jerk), tighter steering.
    f32 stickX = ShieldRider_StickX(input);
    sSteer += (stickX - sSteer) * RIDER_STEER_SMOOTH;

    player->yaw -= (s16)(sSteer * RIDER_STEER);

    // Obstacle jump: something low is ahead (fence, crate...) and nothing tall -> jump over it.
    if (onGround && !sLaunched && sObstacleCooldown == 0) {
        f32 lookahead = MAX(RIDER_OBSTACLE_MIN_LOOKAHEAD, sRiderSpeed * RIDER_OBSTACLE_LOOKAHEAD_FRAMES);

        if (
            ShieldRider_WallAhead(play, player, RIDER_OBSTACLE_LOW_Y, lookahead) &&
            !ShieldRider_WallAhead(play, player, RIDER_OBSTACLE_TOP_Y, lookahead)
        ) {
            player->actor.velocity.y = RIDER_OBSTACLE_JUMP_VEL;
            sLaunched = true;
            sObstacleCooldown = RIDER_OBSTACLE_COOLDOWN;
            sNoBounceFrames = RIDER_OBSTACLE_NO_BOUNCE_FRAMES;
        }
    }

    // Wall bounce: reflect off the wall and keep sliding forward (facing away).
    if (hitWall && sBounceCooldown == 0 && sNoBounceFrames == 0 && player->actor.wallPoly != nullptr) {
        f32 normalY = ABS(player->actor.wallPoly->normal.y / 32767.0f);
        s16 wallYaw = player->actor.wallYaw;
        s16 towardWall = (s16)(player->yaw - (s16)(wallYaw + 0x8000));

        // Real wall (not a steep ramp) and we are moving into it.
        if (normalY < RIDER_WALL_MAX_NORMAL_Y && ABS(towardWall) < 0x4000) {
            player->yaw = (s16)(2 * (s32)wallYaw - (s32)player->yaw + 0x8000);

            player->actor.world.pos.x += Math_SinS(wallYaw) * 4.0f;
            player->actor.world.pos.z += Math_CosS(wallYaw) * 4.0f;

            sRiderSpeed = MAX(sRiderSpeed * RIDER_BOUNCE_KEEP, RIDER_BOUNCE_MIN_SPEED);
            sBounceCooldown = RIDER_BOUNCE_COOLDOWN;
            sSteer = 0.0f;
        }
    }

    // Heading follows the steering directly (no lag, so camera and movement agree).
    player->actor.shape.rot.y = player->yaw;
    player->actor.world.rot.y = player->yaw;

    // Ground following: look one frame ahead and follow ramps (up and down) and the water
    // surface without losing speed.
    if (onGround && !sLaunched) {
        f32 maxRise = RIDER_RISE_BASE + sRiderSpeed * RIDER_RISE_PER_SPEED;
        f32 maxDrop = RIDER_GROUND_SNAP + sRiderSpeed;
        bool aheadWater = false;
        f32 aheadY = ShieldRider_GroundAheadY(
            play,
            player->actor.world.pos.x + Math_SinS(player->yaw) * sRiderSpeed,
            player->actor.world.pos.z + Math_CosS(player->yaw) * sRiderSpeed,
            player->actor.world.pos.y + maxRise + 10.0f,
            &aheadWater
        );

        if (aheadY > BGCHECK_Y_MIN + 1.0f) {
            f32 delta = (aheadY + (aheadWater ? 1.0f : 0.0f)) - player->actor.world.pos.y;

            if (delta <= maxRise && delta >= -maxDrop) {
                player->actor.velocity.y = delta - player->actor.gravity;
            }
        }
    }

    player->linearVelocity = sRiderSpeed;
    player->actor.speedXZ = sRiderSpeed;

    // Body inclination
    s16 leanTarget = (s16)MIN(
        (f32)RIDER_LEAN_BASE + sRiderSpeed * RIDER_LEAN_PER_SPEED,
        (f32)RIDER_LEAN_MAX
    );

    s16 bankTarget = (s16)(sSteer * RIDER_BANK);

    Math_ScaledStepToS(
        &player->actor.shape.rot.x,
        leanTarget,
        RIDER_TILT_STEP
    );

    Math_ScaledStepToS(
        &player->actor.shape.rot.z,
        bankTarget,
        RIDER_TILT_STEP
    );

    // Ram enemies, smash props and fire trail
    ShieldRider_RamEnemies(player, play);
    ShieldRider_SmashProps(player, play);
    ShieldRider_UpdateTrail(player, play, onGround);

    // Boot thrusters, dust / water ripples and sliding sound
    ShieldRider_SpawnBootFire(player, play);

    if (standingOnWater) {
        if ((play->state.frames % 3) == 0) {
            Vec3f ripplePos = { player->actor.world.pos.x, groundY, player->actor.world.pos.z };
            EffectSsGRipple_Spawn(play, &ripplePos, 100, 500, 0);
        }
    } else if (onGround) {
        func_8084269C(play, player);
    }

    func_800F4138(
        &player->actor.projectedPos,
        NA_SE_PL_SLIP_LEVEL - SFX_FLAG,
        player->actor.speedXZ
    );
}

static bool ShieldRider_CanStart(Player* player, PlayState* play) {
    const u32 blockedFlags1 =
        PLAYER_STATE1_INPUT_DISABLED |
        PLAYER_STATE1_TALKING |
        PLAYER_STATE1_DEAD |
        PLAYER_STATE1_CARRYING_ACTOR |
        PLAYER_STATE1_ON_HORSE |
        PLAYER_STATE1_DAMAGED |
        PLAYER_STATE1_IN_WATER |
        PLAYER_STATE1_IN_CUTSCENE;

    return
        player->currentShield != PLAYER_SHIELD_NONE &&
        !(player->stateFlags1 & blockedFlags1) &&
        (player->actor.bgCheckFlags & 1) &&
        player->linearVelocity >= RIDER_MIN_START_SPEED;
}

static void ShieldRider_OnPlayerUpdate() {
    PlayState* play = gPlayState;

    if (play == nullptr) {
        return;
    }

    Player* player = GET_PLAYER(play);

    if (player == nullptr) {
        return;
    }

    ShieldRider_BulletTime(play);
    ShieldRider_UpdateLaunches(play);

    if (player->actionFunc == ShieldRider_Action) {
        // After the engine's own update: keep the camera / engine believing we stand on the water.
        if (sFakeGround) {
            player->actor.bgCheckFlags |= 1;
            player->actor.floorHeight = sFakeGroundY;
            player->stateFlags1 &= ~PLAYER_STATE1_IN_WATER;
        }
        return;
    }

    // Something else took over (damage, cutscene...): give the physics back.
    ShieldRider_RestorePhysics(player);

    if (
        !CHECK_BTN_ALL(play->state.input[0].press.button, BTN_R) ||
        !ShieldRider_CanStart(player, play)
    ) {
        return;
    }

    sRiderSpeed = CLAMP(
        player->linearVelocity + RIDER_START_BOOST,
        0.0f,
        RIDER_MAX_SPEED
    );

    sSavedGravity = (player->actor.gravity != 0.0f) ? player->actor.gravity : -4.0f;
    sSavedMinVelY = player->actor.minVelocityY;
    sRiderActive = true;
    sFakeGround = false;

    sAirFrames = 0;
    sBounceCooldown = 0;
    sWasOnGround = true;
    sLaunched = false;
    sSteer = 0.0f;
    sObstacleCooldown = 0;
    sNoBounceFrames = 0;
    sTrailHead = 0;
    for (s32 i = 0; i < RIDER_TRAIL_POINTS; i++) {
        sTrailLife[i] = 0;
    }

    Player_SetupAction(play, player, ShieldRider_Action, 0);

    func_80832564(play, player);

    Player_AnimChangeLoopMorph(
        play,
        player,
        (LinkAnimationHeader*)RIDER_ANIM
    );

    player->yaw = player->actor.shape.rot.y;
    player->linearVelocity = sRiderSpeed;
}

static void RegisterShieldRider() {
    COND_HOOK(
        OnPlayerUpdate,
        CVAR_SHIELDRIDER_VALUE,
        ShieldRider_OnPlayerUpdate
    );
}

static RegisterShipInitFunc initFunc(
    RegisterShieldRider,
    { CVAR_SHIELDRIDER_NAME }
);
