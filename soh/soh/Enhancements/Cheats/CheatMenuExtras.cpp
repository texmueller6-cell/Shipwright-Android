#include <libultraship/bridge.h>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/Enhancements/enemyrandomizer.h"
#include "soh/ShipInit.hpp"

extern "C" {
extern PlayState* gPlayState;
#include "macros.h"
#include "variables.h"
#include "functions.h"
}

// ---------------------------------------------------------------------------
// CVars
// ---------------------------------------------------------------------------
#define CVAR_SPAWN_AMOUNT CVAR_CHEAT("SpawnAmount")         // int 1..10
#define CVAR_AUTO_SPAWN CVAR_CHEAT("AutoSpawnEnemies")      // bool
#define CVAR_AUTO_SPAWN_SECS CVAR_CHEAT("AutoSpawnSeconds") // int 1..60
#define CVAR_ACTOR_SCALE CVAR_CHEAT("ActorScale")           // float 0.1..10
#define CVAR_GIANT_LINK CVAR_CHEAT("GiantLink")             // bool
#define CVAR_SUPER_JUMP CVAR_CHEAT("SuperJump")             // bool
#define CVAR_FLY CVAR_CHEAT("FlyWithR")                     // bool
#define CVAR_AUTO_HEAL CVAR_CHEAT("AutoHeal")               // bool
#define CVAR_INFINITE_MAGIC CVAR_CHEAT("InfiniteMagic")     // bool
#define CVAR_AUTO_RUPEES CVAR_CHEAT("AutoRupees")           // bool
#define CVAR_INVINCIBILITY CVAR_CHEAT("Invincibility")      // bool
#define CVAR_SLOW_FALL CVAR_CHEAT("SlowFall")               // bool

#define SPAWN_AMOUNT_VAL CVarGetInteger(CVAR_SPAWN_AMOUNT, 1)
#define AUTO_SPAWN_VAL CVarGetInteger(CVAR_AUTO_SPAWN, 0)
#define AUTO_SPAWN_SECS_VAL CVarGetInteger(CVAR_AUTO_SPAWN_SECS, 5)
#define ACTOR_SCALE_VAL CVarGetFloat(CVAR_ACTOR_SCALE, 1.0f)

// ---------------------------------------------------------------------------
// Scale state
// The factor is always applied on top of each actor's ORIGINAL scale, which
// is remembered the first time we touch that actor. Otherwise actors with
// different natural sizes would all be forced to the same size.
// ---------------------------------------------------------------------------
static std::unordered_map<Actor*, Vec3f> sBaseScale;

static Vec3f GetBaseScale(Actor* actor) {
    auto it = sBaseScale.find(actor);
    if (it == sBaseScale.end()) {
        sBaseScale[actor] = actor->scale;
        return actor->scale;
    }
    return it->second;
}

static void SetScaleFactor(Actor* actor, f32 factor) {
    Vec3f base = GetBaseScale(actor);
    actor->scale.x = base.x * factor;
    actor->scale.y = base.y * factor;
    actor->scale.z = base.z * factor;
}

static void RestoreBaseScale(Actor* actor) {
    auto it = sBaseScale.find(actor);
    if (it != sBaseScale.end()) {
        actor->scale = it->second;
        sBaseScale.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool GetLinkPos(Vec3f* out) {
    if (gPlayState == nullptr) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return false;
    }
    *out = player->actor.world.pos;
    return true;
}

static void SpawnRandomEnemyNearLink(int count) {
    Vec3f base;
    if (!GetLinkPos(&base)) {
        return;
    }
    for (int i = 0; i < count; i++) {
        Vec3f pos = { base.x + ((std::rand() % 401) - 200), base.y + 50, base.z + ((std::rand() % 401) - 200) };

        CollisionPoly poly;
        f32 floorY = BgCheck_AnyRaycastFloor1(&gPlayState->colCtx, &poly, &pos);
        if (floorY > BGCHECK_Y_MIN) {
            pos.y = floorY;
        }

        EnemyEntry entry = GetRandomizedEnemyEntry((uint32_t)std::rand());
        Actor_Spawn(&gPlayState->actorCtx, gPlayState, entry.id, pos.x, pos.y, pos.z, 0, 0, 0, entry.params, 0);
    }
}

// Collects enemies first, so killing or moving them never breaks the list being walked.
static std::vector<Actor*> CollectEnemies() {
    std::vector<Actor*> enemies;
    if (gPlayState == nullptr) {
        return enemies;
    }
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; actor != nullptr;
         actor = actor->next) {
        enemies.push_back(actor);
    }
    return enemies;
}

// ---------------------------------------------------------------------------
// Button actions (called from the cheat menu)
// ---------------------------------------------------------------------------
void CheatExtras_SpawnRandomEnemy() {
    SpawnRandomEnemyNearLink(SPAWN_AMOUNT_VAL);
}

void CheatExtras_SpawnRandomBurst() {
    SpawnRandomEnemyNearLink(10);
}

void CheatExtras_EnemyShower() {
    Vec3f base;
    if (!GetLinkPos(&base)) {
        return;
    }
    for (int i = 0; i < 10; i++) {
        // Spawned high above Link, so they fall onto him.
        f32 x = base.x + ((std::rand() % 401) - 200);
        f32 z = base.z + ((std::rand() % 401) - 200);
        EnemyEntry entry = GetRandomizedEnemyEntry((uint32_t)std::rand());
        Actor_Spawn(&gPlayState->actorCtx, gPlayState, entry.id, x, base.y + 400, z, 0, 0, 0, entry.params, 0);
    }
}

void CheatExtras_SpinEnemies() {
    for (Actor* enemy : CollectEnemies()) {
        enemy->world.rot.y = (s16)std::rand();
    }
}

void CheatExtras_ApplyScale() {
    if (gPlayState == nullptr) {
        return;
    }
    f32 factor = ACTOR_SCALE_VAL;
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        // Link is handled by Giant Link, so the global button skips him.
        if (cat == ACTORCAT_PLAYER) {
            continue;
        }
        for (Actor* actor = gPlayState->actorCtx.actorLists[cat].head; actor != nullptr; actor = actor->next) {
            SetScaleFactor(actor, factor);
        }
    }
}

void CheatExtras_ChaosScale() {
    if (gPlayState == nullptr) {
        return;
    }
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        if (cat == ACTORCAT_PLAYER) {
            continue;
        }
        for (Actor* actor = gPlayState->actorCtx.actorLists[cat].head; actor != nullptr; actor = actor->next) {
            // Random factor between 0.5x and 2.0x for each actor.
            f32 factor = 0.5f + (std::rand() % 1501) / 1000.0f;
            SetScaleFactor(actor, factor);
        }
    }
}

void CheatExtras_ResetScale() {
    if (gPlayState == nullptr) {
        return;
    }
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[cat].head; actor != nullptr; actor = actor->next) {
            RestoreBaseScale(actor);
        }
    }
}

void CheatExtras_KillAllEnemies() {
    for (Actor* enemy : CollectEnemies()) {
        Actor_Kill(enemy);
    }
}

void CheatExtras_TeleportEnemiesToLink() {
    Vec3f linkPos;
    if (!GetLinkPos(&linkPos)) {
        return;
    }
    for (Actor* enemy : CollectEnemies()) {
        enemy->world.pos.x = linkPos.x + 100.0f;
        enemy->world.pos.y = linkPos.y;
        enemy->world.pos.z = linkPos.z;
    }
}

void CheatExtras_HealLink() {
    gSaveContext.health = gSaveContext.healthCapacity;
}

void CheatExtras_RefillMagic() {
    if (gSaveContext.magicLevel > 0) {
        gSaveContext.magic = (gSaveContext.magicLevel == 2) ? 0x60 : 0x30;
    }
}

void CheatExtras_MaxRupees() {
    gSaveContext.rupees = 999;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static int sAutoSpawnFrames = 0;

void OnGameFrameUpdateCheatExtras() {
    if (gPlayState == nullptr) {
        return;
    }

    if (AUTO_SPAWN_VAL) {
        sAutoSpawnFrames++;
        // Game logic runs at 20 frames per second.
        if (sAutoSpawnFrames >= AUTO_SPAWN_SECS_VAL * 20) {
            sAutoSpawnFrames = 0;
            SpawnRandomEnemyNearLink(1);
        }
    } else {
        sAutoSpawnFrames = 0;
    }

    if (CVarGetInteger(CVAR_AUTO_HEAL, 0) && gSaveContext.health < gSaveContext.healthCapacity) {
        gSaveContext.health = gSaveContext.healthCapacity;
    }

    if (CVarGetInteger(CVAR_INFINITE_MAGIC, 0)) {
        CheatExtras_RefillMagic();
    }

    if (CVarGetInteger(CVAR_AUTO_RUPEES, 0)) {
        gSaveContext.rupees = 999;
    }
}

void OnPlayerUpdateCheatExtras() {
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }

    u16 buttons = gPlayState->state.input[0].cur.button;

    // Super Jump: holding A while rising adds extra height.
    if (CVarGetInteger(CVAR_SUPER_JUMP, 0) && CHECK_BTN_ANY(buttons, BTN_A) && player->actor.velocity.y > 0.0f) {
        player->actor.velocity.y += 1.5f;
    }

    // Fly: holding R keeps Link rising.
    if (CVarGetInteger(CVAR_FLY, 0) && CHECK_BTN_ANY(buttons, BTN_R)) {
        player->actor.velocity.y = 4.0f;
    }

    // Slow Fall: halves downward speed while falling.
    if (CVarGetInteger(CVAR_SLOW_FALL, 0) && player->actor.velocity.y < 0.0f) {
        player->actor.velocity.y *= 0.5f;
    }

    // Invincibility: negative values mean invulnerability in the game's timer.
    if (CVarGetInteger(CVAR_INVINCIBILITY, 0)) {
        player->invincibilityTimer = -1;
    }

    // Giant Link: relative to Link's original scale. Turning it off restores him.
    if (CVarGetInteger(CVAR_GIANT_LINK, 0)) {
        SetScaleFactor(&player->actor, ACTOR_SCALE_VAL);
    } else {
        RestoreBaseScale(&player->actor);
    }
}

void OnSceneInitCheatExtras(int16_t sceneNum) {
    // Actor pointers from the previous scene are gone, so forget their saved scales.
    sBaseScale.clear();
    sAutoSpawnFrames = 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void RegisterCheatMenuExtras() {
    COND_HOOK(OnGameFrameUpdate, true, OnGameFrameUpdateCheatExtras);
    COND_HOOK(OnPlayerUpdate, true, OnPlayerUpdateCheatExtras);
    COND_HOOK(OnSceneInit, true, OnSceneInitCheatExtras);
}

static RegisterShipInitFunc initCheatMenuExtras(RegisterCheatMenuExtras,
                                                { CVAR_AUTO_SPAWN, CVAR_SUPER_JUMP, CVAR_FLY, CVAR_GIANT_LINK,
                                                  CVAR_AUTO_HEAL, CVAR_INFINITE_MAGIC, CVAR_AUTO_RUPEES,
                                                  CVAR_INVINCIBILITY, CVAR_SLOW_FALL });
