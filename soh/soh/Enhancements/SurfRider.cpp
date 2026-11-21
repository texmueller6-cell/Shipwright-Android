// ShieldRider.cpp
//
// Shield Rider - Ship of Harkinian Android 9.0.2
//
// Adjustments:
// - Surf only: no smashing of bushes, pots, rocks, crates or enemies.
// - Low obstacles (fences, crates) are jumped automatically.
// - Surfing NEVER stops after a jump: Link keeps sliding in the air, on landing and on water.
// - Landing in water (after a jump or a ledge) keeps Link sliding on the surface (no swimming).
// - Wall contact bounces Link back (reflects), speed is kept.
// - Speed is kept on ramps (uphill assist, glued to floor downhill).
// - Ledges make Link hop and keep the surf pose until landing.
// - Tighter, smoother turns.
// - Visual effects only: speed sparks, water wake and splashes, skid dust in turns, take-off / landing impact.
// - Surfing ends only when R is released.
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

static constexpr s32 RIDER_STICK_DEADZONE = 20;

// Water landing: Link is put on the surface if he would reach it within this much extra depth.
static constexpr f32 RIDER_WATER_SNAP_DEPTH = 45.0f; // must stay below Link's swim depth (56)

static constexpr s16 RIDER_LEAN_BASE = 0x0800;
static constexpr f32 RIDER_LEAN_PER_SPEED = 40.0f;
static constexpr s16 RIDER_LEAN_MAX = 0x1800;
static constexpr f32 RIDER_BANK = 45.0f;
static constexpr s16 RIDER_TILT_STEP = 500;
static constexpr f32 RIDER_STEER = 18.0f;           // tighter turns
static constexpr f32 RIDER_STEER_SMOOTH = 0.5f;     // 0..1, lower = smoother stick response

static constexpr f32 RIDER_SHIELD_SCALE = 0.012f;
static constexpr f32 RIDER_SHIELD_HEIGHT = 1.5f;
static constexpr s16 RIDER_SHIELD_PITCH = -0x4000;

#define RIDER_ANIM gPlayerAnim_link_normal_down_slope_slip

static f32 sRiderSpeed;
static s32 sBounceCooldown;
static bool sWasOnGround;
static bool sLaunched;
static f32 sSteer;
static s32 sObstacleCooldown;
static s32 sNoBounceFrames;
static bool sRiderActive;
static f32 sSavedGravity = -4.0f;
static f32 sSavedMinVelY = -20.0f;
static bool sFxJump;      // set when a jump starts, consumed by the effects
static bool sFakeGround;
static f32 sFakeGroundY;
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

// Visual-only effects: speed sparks, water wake, skid dust in turns, take-off / landing impact.
static void ShieldRider_SpawnFx(Player* player, PlayState* play, bool onGround, bool onWater, f32 groundY,
                                bool justLanded) {
    static Color_RGBA8 dustPrim = { 210, 190, 150, 255 };
    static Color_RGBA8 dustEnv = { 120, 100, 70, 0 };
    static Color_RGBA8 sprayPrim = { 235, 245, 255, 255 };
    static Color_RGBA8 sprayEnv = { 90, 150, 220, 0 };
    static Color_RGBA8 sparkPrimSlow = { 120, 220, 255, 255 };
    static Color_RGBA8 sparkPrimFast = { 255, 220, 90, 255 };
    static Color_RGBA8 sparkEnv = { 255, 120, 20, 0 };

    f32 s = Math_SinS(player->actor.shape.rot.y);
    f32 c = Math_CosS(player->actor.shape.rot.y);
    Vec3f feet = player->actor.world.pos;
    s32 frame = play->state.frames;

    // Take-off or landing: ring of puffs / splash.
    if (sFxJump || justLanded) {
        Color_RGBA8* prim = onWater ? &sprayPrim : &dustPrim;
        Color_RGBA8* env = onWater ? &sprayEnv : &dustEnv;
        f32 power = justLanded ? 4.5f : 3.0f;
        Vec3f base = feet;

        base.y = onGround ? (onWater ? groundY : feet.y) : feet.y;

        for (s32 i = 0; i < 8; i++) {
            s16 angle = (s16)(i * 0x2000);
            Vec3f pos = base;
            Vec3f vel = { Math_SinS(angle) * power, power * 0.4f, Math_CosS(angle) * power };
            Vec3f accel = { 0.0f, -0.1f, 0.0f };

            EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, prim, env, 140, 18, 16, 0);
        }

        if (onWater && justLanded) {
            Vec3f splash = { base.x, groundY, base.z };

            EffectSsGSplash_Spawn(play, &splash, nullptr, nullptr, 0, 500);
            EffectSsGRipple_Spawn(play, &splash, 150, 900, 0);
        }

        sFxJump = false;
    }

    // Speed sparks from the shield / boots, more of them (and golden) the faster Link goes.
    if (sRiderSpeed > 14.0f && (frame & 1) == 0) {
        f32 t = CLAMP((sRiderSpeed - 14.0f) / (RIDER_MAX_SPEED - 14.0f), 0.0f, 1.0f);
        Color_RGBA8 prim = {
            (u8)(sparkPrimSlow.r + (sparkPrimFast.r - sparkPrimSlow.r) * t),
            (u8)(sparkPrimSlow.g + (sparkPrimFast.g - sparkPrimSlow.g) * t),
            (u8)(sparkPrimSlow.b + (sparkPrimFast.b - sparkPrimSlow.b) * t),
            255,
        };
        Vec3f pos = { feet.x - s * 14.0f, feet.y + 6.0f, feet.z - c * 14.0f };
        Vec3f vel = { -s * 2.0f + c * ((frame & 2) ? 2.0f : -2.0f), 1.5f, -c * 2.0f - s * ((frame & 2) ? 2.0f : -2.0f) };
        Vec3f accel = { 0.0f, -0.15f, 0.0f };

        EffectSsKiraKira_SpawnSmall(play, &pos, &vel, &accel, &prim, &sparkEnv);
    }

    // Water wake: two sprays flaring out behind Link and a trailing ripple.
    if (onGround && onWater) {
        if ((frame & 1) == 0) {
            for (s32 side = -1; side <= 1; side += 2) {
                Vec3f pos = { feet.x - s * 12.0f + c * side * 10.0f, groundY + 2.0f, feet.z - c * 12.0f - s * side * 10.0f };
                Vec3f vel = { -s * 2.0f + c * side * 2.5f, 2.2f, -c * 2.0f - s * side * 2.5f };
                Vec3f accel = { 0.0f, -0.25f, 0.0f };

                EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, &sprayPrim, &sprayEnv, 90, 10, 14, 0);
            }
        }

        if ((frame % 5) == 0) {
            Vec3f splash = { feet.x - s * 18.0f, groundY, feet.z - c * 18.0f };

            EffectSsGSplash_Spawn(play, &splash, nullptr, nullptr, 0, 120);
        }
    }

    // Skid dust when carving a hard turn on the ground.
    if (onGround && !onWater && ABS(sSteer) > 14.0f && (frame & 1) == 0) {
        f32 side = sSteer > 0.0f ? -1.0f : 1.0f; // outside of the turn
        Vec3f pos = { feet.x + c * side * 12.0f, feet.y + 2.0f, feet.z - s * side * 12.0f };
        Vec3f vel = { c * side * 2.0f - s * 1.0f, 1.0f, -s * side * 2.0f - c * 1.0f };
        Vec3f accel = { 0.0f, 0.1f, 0.0f };

        EffectSsDust_Spawn(play, 0, &pos, &vel, &accel, &dustPrim, &dustEnv, 120, 16, 16, 0);
    }
}

static void ShieldRider_Stop(Player* player, PlayState* play, bool grounded) {
    f32 exitSpeed = sRiderSpeed;
    sRiderSpeed = 0.0f;

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
            surfaceY <= player->actor.world.pos.y + (onGround ? 30.0f : RIDER_WATER_SNAP_DEPTH)
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

        // Where Link will be after this frame's fall. If that is on / under the water surface,
        // put him on the surface NOW so the engine never sees him sink (which would start swimming).
        f32 nextY = player->actor.world.pos.y + player->actor.velocity.y + sSavedGravity;

        if (onWater && falling && nextY <= groundY) {
            // Came down on the water (after a jump, a ledge or a fall): keep sliding on it.
            player->actor.world.pos.y = groundY;
            player->actor.velocity.y = 0.0f;
            onGround = true;
        } else if (sLaunched) {
            // In the air after a jump: keep the surf pose and speed until landing.
        } else if (drop >= -60.0f && drop <= snapDist) {
            // Slope, small drop or water just below: stay on it and keep sliding.
            player->actor.world.pos.y = groundY;
            player->actor.velocity.y = 0.0f;
            onGround = true;
        } else if (sWasOnGround) {
            // Real ledge: hop off it at full speed (surf pose is kept).
            player->actor.velocity.y = RIDER_EDGE_JUMP_VEL;
            sLaunched = true;
            sFxJump = true;
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

    bool justLanded = onGround && !sWasOnGround;

    sWasOnGround = onGround;

    // Only releasing R ends surfing (no time limit in the air).
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
            sFxJump = true;
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

    // Boot thrusters, dust / water ripples and sliding sound
    ShieldRider_SpawnBootFire(player, play);
    ShieldRider_SpawnFx(player, play, onGround, standingOnWater, groundY, justLanded);

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

// Surfing must never stop on its own. If the engine takes control while R is still held (for example
// it puts Link into swimming after a jump into water, or into a landing / falling action), put him
// back into the surf action, keeping his speed. Real interruptions (damage, death, cutscenes,
// talking, scene changes, ledge / ladder climbing) are left alone.
static bool ShieldRider_TryResume(Player* player, PlayState* play) {
    const u32 blockedFlags1 =
        PLAYER_STATE1_INPUT_DISABLED |
        PLAYER_STATE1_TALKING |
        PLAYER_STATE1_DEAD |
        PLAYER_STATE1_CARRYING_ACTOR |
        PLAYER_STATE1_ON_HORSE |
        PLAYER_STATE1_DAMAGED |
        PLAYER_STATE1_IN_CUTSCENE |
        PLAYER_STATE1_HANGING_OFF_LEDGE |
        PLAYER_STATE1_CLIMBING_LEDGE |
        PLAYER_STATE1_CLIMBING_LADDER;

    if (
        !sRiderActive ||
        sRiderSpeed <= 0.0f ||
        !CHECK_BTN_ALL(play->state.input[0].cur.button, BTN_R) ||
        (player->stateFlags1 & blockedFlags1) ||
        player->csAction != 0 ||
        play->transitionTrigger != TRANS_TRIGGER_OFF ||
        gSaveContext.health <= 0
    ) {
        return false;
    }

    // Undo the swimming state the engine may have set.
    player->stateFlags1 &= ~PLAYER_STATE1_IN_WATER;
    player->stateFlags2 &= ~PLAYER_STATE2_UNDERWATER;
    player->underwaterTimer = 0;
    Audio_SetBaseFilter(0);

    // If Link is at / just under a water surface, stand him back on it; otherwise he is in the air.
    bool onGround = (player->actor.bgCheckFlags & 1) != 0;
    f32 surfaceY;
    WaterBox* waterBox;

    if (
        WaterBox_GetSurface1(
            play, &play->colCtx,
            player->actor.world.pos.x, player->actor.world.pos.z,
            &surfaceY, &waterBox
        ) &&
        surfaceY >= player->actor.floorHeight - 0.5f &&
        player->actor.world.pos.y <= surfaceY + 5.0f &&
        player->actor.world.pos.y >= surfaceY - 150.0f
    ) {
        player->actor.world.pos.y = surfaceY + 1.0f;
        player->actor.velocity.y = 0.0f;
        player->actor.bgCheckFlags |= 1;
        onGround = true;
    }

    Player_SetupAction(play, player, ShieldRider_Action, 0);
    func_80832564(play, player);
    Player_AnimChangeLoopMorph(play, player, (LinkAnimationHeader*)RIDER_ANIM);

    player->actor.shape.rot.x = 0;
    player->actor.shape.rot.z = 0;
    player->actor.shape.rot.y = player->yaw;
    player->actor.world.rot.y = player->yaw;
    player->linearVelocity = sRiderSpeed;
    player->actor.speedXZ = sRiderSpeed;
    player->actor.gravity = sSavedGravity;

    sFakeGround = false;
    sWasOnGround = onGround;
    sLaunched = !onGround;
    sNoBounceFrames = 0;
    sSteer = 0.0f;

    return true;
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

    if (player->actionFunc == ShieldRider_Action) {
        // After the engine's own update: keep the camera / engine believing we stand on the water.
        if (sFakeGround) {
            player->actor.bgCheckFlags |= 1;
            player->actor.floorHeight = sFakeGroundY;
            player->stateFlags1 &= ~PLAYER_STATE1_IN_WATER;
        }
        return;
    }

    // The engine took over while we were surfing: take Link back unless it is a real interruption.
    if (ShieldRider_TryResume(player, play)) {
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

    sBounceCooldown = 0;
    sFxJump = false;
    sWasOnGround = true;
    sLaunched = false;
    sSteer = 0.0f;
    sObstacleCooldown = 0;
    sNoBounceFrames = 0;

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
