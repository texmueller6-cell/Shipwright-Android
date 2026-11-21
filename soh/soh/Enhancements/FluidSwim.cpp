// FluidSwim.cpp
//
// Fluid Swim - Ship of Harkinian Android 9.0.2
//
// Makes Link swim faster and smoother:
// - Continuous speed: no more "stroke pulses" where he speeds up and slows down with each arm swing.
// - Much higher top speed (the vanilla cap is about 5.4 units/frame).
// - Smooth turning: the heading eases towards the stick direction instead of snapping to it.
// - Gentle glide when the stick is released.
//
// Only the surface swimming states are changed. Diving, iron boots, Z-targeting facing,
// cutscenes and the Shield Rider mod are left alone.
//
// Place this file next to SurfRider.cpp. Toggle with the CVar "gEnhancements.FluidSwim" (default on).

#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"

extern s32 Player_GetMovementSpeedAndYaw(Player* this, f32* outSpeedTarget, s16* outYawTarget, f32 speedMode,
                                         PlayState* play);
extern void Player_Action_8084D610(Player* this, PlayState* play); // idle in water
extern void Player_Action_8084D84C(Player* this, PlayState* play); // swimming
extern void Player_Action_8084DAB4(Player* this, PlayState* play); // swimming while Z-targeting
extern PlayState* gPlayState;
}

#define CVAR_FLUIDSWIM_NAME CVAR_ENHANCEMENT("FluidSwim")
#define CVAR_FLUIDSWIM_DEFAULT 1
#define CVAR_FLUIDSWIM_VALUE CVarGetInteger(CVAR_FLUIDSWIM_NAME, CVAR_FLUIDSWIM_DEFAULT)

// Tweak these to taste.
static constexpr f32 SWIM_MAX_SPEED = 11.0f;      // top speed with the stick fully pushed (vanilla ~5.4)
static constexpr f32 SWIM_ACCEL = 0.14f;          // 0..1, how fast speed rises towards the target
static constexpr f32 SWIM_MIN_ACCEL_STEP = 0.2f;  // so it never crawls when almost at the target
static constexpr f32 SWIM_GLIDE = 0.07f;          // 0..1, how fast speed falls when the stick is released
static constexpr f32 SWIM_STICK_FULL = 6.7f;      // engine speed target at full stick (60 * 0.8 * 0.14)
static constexpr f32 SWIM_ANIM_SPEED_CAP = 6.0f;  // limits how fast the arm strokes animate

static constexpr s16 SWIM_TURN_SCALE = 3;         // smaller = snappier turns
static constexpr s16 SWIM_TURN_MAX_STEP = 0x0C00; // max turn per frame
static constexpr s16 SWIM_TURN_MIN_STEP = 0x0100; // min turn per frame (so the turn always finishes)

static f32 sSwimSpeed;
static bool sSwimActive;

static bool FluidSwim_IsSwimAction(Player* player) {
    return player->actionFunc == Player_Action_8084D84C || player->actionFunc == Player_Action_8084D610 ||
           player->actionFunc == Player_Action_8084DAB4;
}

static bool FluidSwim_CanSwim(Player* player) {
    const u32 blockedFlags1 = PLAYER_STATE1_INPUT_DISABLED | PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD |
                              PLAYER_STATE1_CARRYING_ACTOR | PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_CUTSCENE;

    return (player->stateFlags1 & PLAYER_STATE1_IN_WATER) && !(player->stateFlags1 & blockedFlags1) &&
           player->currentBoots != PLAYER_BOOTS_IRON && FluidSwim_IsSwimAction(player);
}

static void FluidSwim_Run(Player* player, PlayState* play) {
    bool wasSwimming = sSwimActive;
    bool canSwim = FluidSwim_CanSwim(player);

    if (!canSwim) {
        // Not swimming on the surface: let the engine do everything.
        sSwimActive = false;
        player->actionFunc(player, play);
        return;
    }

    if (!wasSwimming) {
        // Just started swimming: begin from the speed Link already has.
        sSwimSpeed = MAX(player->linearVelocity, 0.0f);
        sSwimActive = true;
    }

    s16 prevYaw = player->yaw;

    // The engine uses linearVelocity to choose the arm-stroke animation speed, so hand it a capped
    // value for this frame; our own smooth speed is applied right after.
    player->linearVelocity = MIN(sSwimSpeed, SWIM_ANIM_SPEED_CAP);

    player->actionFunc(player, play);

    // The engine may have left the swim states this frame (dive, climb out, damage...).
    if (!FluidSwim_CanSwim(player)) {
        sSwimActive = false;
        return;
    }

    f32 speedTarget = 0.0f;
    s16 yawTarget = player->yaw;
    bool hasInput = Player_GetMovementSpeedAndYaw(player, &speedTarget, &yawTarget, 0.0f, play) != 0;

    // Continuous speed.
    f32 target = 0.0f;

    if (hasInput) {
        target = SWIM_MAX_SPEED * CLAMP(speedTarget / SWIM_STICK_FULL, 0.0f, 1.0f);
    }

    f32 diff = target - sSwimSpeed;

    if (diff > 0.0f) {
        sSwimSpeed += MAX(diff * SWIM_ACCEL, MIN(SWIM_MIN_ACCEL_STEP, diff));
    } else {
        sSwimSpeed += diff * SWIM_GLIDE;
    }

    if (sSwimSpeed < 0.05f && !hasInput) {
        sSwimSpeed = 0.0f;
    }

    player->linearVelocity = sSwimSpeed;
    player->actor.speedXZ = sSwimSpeed;

    // Smooth turning (not while Z-targeting, where the engine controls the facing).
    if (hasInput && player->actionFunc != Player_Action_8084DAB4) {
        s16 yaw = prevYaw;

        Math_SmoothStepToS(&yaw, yawTarget, SWIM_TURN_SCALE, SWIM_TURN_MAX_STEP, SWIM_TURN_MIN_STEP);

        player->yaw = yaw;
        player->actor.shape.rot.y = yaw;
        player->actor.world.rot.y = yaw;
    } else if (!hasInput) {
        // Keep gliding in the direction Link was already facing.
        player->yaw = prevYaw;
        player->actor.world.rot.y = prevYaw;
    }
}

static void RegisterFluidSwim() {
    COND_VB_SHOULD(VB_EXECUTE_PLAYER_ACTION_FUNC, CVAR_FLUIDSWIM_VALUE, {
        // If the vanilla condition does not want the actionFunc to run, do nothing.
        if (!*should) {
            return;
        }

        Player* player = va_arg(args, Player*);

        // Only the real player (not Dark Link).
        if (player->actor.id != ACTOR_PLAYER || gPlayState == nullptr) {
            return;
        }

        // Cheap check first: only take over while in the water.
        if (!(player->stateFlags1 & PLAYER_STATE1_IN_WATER) && !sSwimActive) {
            return;
        }

        *should = false;

        FluidSwim_Run(player, gPlayState);
    });
}

static RegisterShipInitFunc initFunc(RegisterFluidSwim, { CVAR_FLUIDSWIM_NAME });
