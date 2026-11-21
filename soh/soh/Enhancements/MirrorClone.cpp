// MirrorClone.cpp (v4) — clone aliado do Link que LUTA ao seu lado
// Ship of Harkinian 9.0.2 (Android) — escrito contra o código-fonte real da 9.0.2.
//
// Onde colocar: soh/soh/Enhancements/MirrorClone.cpp
// NÃO precisa registrar em lugar nenhum: o CMake pega todo soh/*.cpp (GLOB_RECURSE)
// e o RegisterShipInitFunc no fim do arquivo liga o mod sozinho na inicialização.
//
// Como funciona:
//  - O clone é o corpo do Dark Link (ACTOR_EN_TORCH2), que na 9.0.2 é um Player completo.
//    Em vez de reinventar física/animação, o clone roda o MESMO código do Link
//    (play->playerUpdate) controlado por um "controle" sintético, como o Dark Link faz.
//    Resultado: andar, correr, pular degrau, espada e animações de verdade.
//  - Sem inimigo por perto: segue você (fica ao seu lado).
//  - Com inimigo/chefe por perto: corre até ele e ataca com a espada.
//  - É aliado: a espada dele é AT_TYPE_PLAYER (machuca inimigos, não machuca você)
//    e ele não leva dano nem é empurrado.
//
// Ativação: segure D-Pad Esquerda (MIRROR_CLONE_BUTTON), ou ligue "Sempre ativo" no menu.
//
// Limite importante: se já existir um Dark Link de verdade na cena, o clone NÃO aparece
// (os dois dividem variáveis estáticas do actor e o clone quebraria a luta).

#include <libultraship/bridge.h>
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "macros.h"
#include "z64.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

// ----------------- configuração -----------------
#define MIRROR_CLONE_BUTTON BTN_DLEFT
#define MIRROR_CLONE_SIDE_OFFSET 55.0f      // distância lateral quando só segue o Link
#define MIRROR_CLONE_SEARCH_RANGE 450.0f    // raio (a partir do Link) para procurar inimigos
#define MIRROR_CLONE_ATTACK_RANGE 65.0f     // distância para parar e golpear
#define MIRROR_CLONE_SWING_PERIOD 14        // frames entre golpes
#define MIRROR_CLONE_TELEPORT_DIST 800.0f   // longe demais do Link -> volta para perto
#define MIRROR_CLONE_VOID_DROP 600.0f       // caiu abaixo disso -> volta para perto

#define CVAR_MIRROR_CLONE_NAME CVAR_CHEAT("MirrorClone")
#define CVAR_MIRROR_CLONE_DEFAULT 1
#define CVAR_MIRROR_CLONE_VALUE CVarGetInteger(CVAR_MIRROR_CLONE_NAME, CVAR_MIRROR_CLONE_DEFAULT)
#define CVAR_MIRROR_CLONE_ALWAYS_NAME CVAR_CHEAT("MirrorCloneAlwaysOn")
#define CVAR_MIRROR_CLONE_ALWAYS_VALUE CVarGetInteger(CVAR_MIRROR_CLONE_ALWAYS_NAME, 0)

static Actor* sClone = nullptr;
static Input sCloneInput;
static int sSwingTimer = 0;

// ---------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------

static bool MirrorClone_RealDarkLinkExists(PlayState* play) {
    const int cats[] = { ACTORCAT_BOSS, ACTORCAT_ENEMY };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_EN_TORCH2 && a != sClone && a->update != nullptr) {
                return true;
            }
        }
    }
    return false;
}

// Inimigo/chefe mais próximo do Link dentro do raio de busca.
static Actor* MirrorClone_FindTarget(PlayState* play, Player* player) {
    Actor* best = nullptr;
    float bestDist = MIRROR_CLONE_SEARCH_RANGE;
    const int cats[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            if (a == sClone || a->update == nullptr || a->draw == nullptr) {
                continue;
            }
            if (!(a->flags & (ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE))) {
                continue; // não é um alvo "de verdade" (morto, decorativo, etc.)
            }
            if (a->colChkInfo.health <= 0 && cats[c] == ACTORCAT_ENEMY) {
                continue;
            }
            float d = Actor_WorldDistXZToActor(&player->actor, a);
            if (d < bestDist) {
                bestDist = d;
                best = a;
            }
        }
    }
    return best;
}

static void MirrorClone_TeleportToPlayer(Actor* clone, Player* player) {
    s16 yaw = player->actor.shape.rot.y;
    clone->world.pos.x = player->actor.world.pos.x + Math_CosS(yaw) * MIRROR_CLONE_SIDE_OFFSET;
    clone->world.pos.y = player->actor.world.pos.y + 5.0f;
    clone->world.pos.z = player->actor.world.pos.z - Math_SinS(yaw) * MIRROR_CLONE_SIDE_OFFSET;
    clone->prevPos = clone->world.pos;
    clone->velocity.x = clone->velocity.y = clone->velocity.z = 0.0f;
    clone->speedXZ = 0.0f;
    clone->shape.rot.y = clone->world.rot.y = yaw;
}

// Converte "ângulo + inclinação do analógico" (relativo ao mundo) em stick do controle,
// relativo à câmera — exatamente como o Dark Link faz em z_en_torch2.c.
static void MirrorClone_SetStick(PlayState* play, s16 angle, f32 tilt) {
    Camera* camera = Play_GetCamera(play, 0);
    s16 rel = camera->camDir.y - angle;
    sCloneInput.cur.stick_x = (s8)(tilt * Math_SinS(rel));
    sCloneInput.cur.stick_y = (s8)(tilt * Math_CosS(rel));
}

// Atualiza press/rel/prev do "controle" do clone (mesma lógica do Dark Link).
static void MirrorClone_UpdateInput() {
    u16 changed = sCloneInput.prev.button ^ sCloneInput.cur.button;
    sCloneInput.press.button = sCloneInput.cur.button & changed;
    sCloneInput.rel.button = sCloneInput.prev.button & changed;
    sCloneInput.prev.button = sCloneInput.cur.button & (u16) ~(BTN_A | BTN_B);
    PadUtils_UpdateRelXY(&sCloneInput);

    if (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)) {
        sCloneInput.rel.stick_x *= -1;
    }

    sCloneInput.press.stick_x += (s8)(sCloneInput.cur.stick_x - sCloneInput.prev.stick_x);
    sCloneInput.press.stick_y += (s8)(sCloneInput.cur.stick_y - sCloneInput.prev.stick_y);
}

// ---------------------------------------------------------------
// Update do clone — substitui o EnTorch2_Update (a IA hostil do Dark Link)
// ---------------------------------------------------------------
static void MirrorClone_Update(Actor* thisx, PlayState* play) {
    Player* player = GET_PLAYER(play);
    Player* clone = reinterpret_cast<Player*>(thisx);
    if (player == nullptr) {
        return;
    }

    // Garante que continua aliado e intocável (re-afirmado todo frame por segurança).
    thisx->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
    thisx->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
    clone->cylinder.base.acFlags = AC_NONE;
    clone->cylinder.base.ocFlags1 = OC1_NONE;
    clone->meleeWeaponQuads[0].base.acFlags = AC_NONE;
    clone->meleeWeaponQuads[1].base.acFlags = AC_NONE;
    clone->meleeWeaponQuads[0].base.atFlags = AT_ON | AT_TYPE_PLAYER;
    clone->meleeWeaponQuads[1].base.atFlags = AT_ON | AT_TYPE_PLAYER;
    clone->shieldQuad.base.atFlags = AT_NONE;
    clone->shieldQuad.base.acFlags = AC_NONE;
    clone->invincibilityTimer = 0;

    // Segurança: longe demais ou caiu no vazio -> volta para perto do Link.
    if (Actor_WorldDistXZToActor(thisx, &player->actor) > MIRROR_CLONE_TELEPORT_DIST ||
        thisx->world.pos.y < player->actor.world.pos.y - MIRROR_CLONE_VOID_DROP) {
        MirrorClone_TeleportToPlayer(thisx, player);
    }

    sCloneInput.cur.button = 0;
    sCloneInput.cur.stick_x = sCloneInput.cur.stick_y = 0;

    Actor* target = MirrorClone_FindTarget(play, player);

    if (target != nullptr) {
        // MODO LUTA: corre até o alvo; perto dele, para, encara e golpeia.
        f32 dist = Actor_WorldDistXZToActor(thisx, target);
        s16 toTarget = Actor_WorldYawTowardActor(thisx, target);
        if (dist > MIRROR_CLONE_ATTACK_RANGE) {
            MirrorClone_SetStick(play, toTarget, 127.0f);
            sSwingTimer = 0;
        } else {
            thisx->shape.rot.y = thisx->world.rot.y = toTarget;
            if ((sSwingTimer++ % MIRROR_CLONE_SWING_PERIOD) == 0) {
                sCloneInput.cur.button = BTN_B;
            }
        }
    } else {
        // MODO SEGUIR: vai para o lado do Link e espera lá.
        sSwingTimer = 0;
        s16 yaw = player->actor.shape.rot.y;
        Vec3f goal;
        goal.x = player->actor.world.pos.x + Math_CosS(yaw) * MIRROR_CLONE_SIDE_OFFSET;
        goal.y = player->actor.world.pos.y;
        goal.z = player->actor.world.pos.z - Math_SinS(yaw) * MIRROR_CLONE_SIDE_OFFSET;
        f32 dist = Math_Vec3f_DistXZ(&thisx->world.pos, &goal);
        if (dist > 120.0f) {
            MirrorClone_SetStick(play, Math_Vec3f_Yaw(&thisx->world.pos, &goal), 127.0f);
        } else if (dist > 30.0f) {
            MirrorClone_SetStick(play, Math_Vec3f_Yaw(&thisx->world.pos, &goal), 60.0f);
        }
    }

    MirrorClone_UpdateInput();

    // Roda o corpo de Player (física, colisão com parede/chão, animações, espada).
    play->playerUpdate(clone, play, &sCloneInput);

    thisx->focus.pos = thisx->world.pos;
    thisx->focus.pos.y += 20.0f;
}

// ---------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------
static void MirrorClone_Spawn() {
    if (sClone != nullptr || gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr || MirrorClone_RealDarkLinkExists(gPlayState)) {
        return;
    }

    // canRandomize = false: com Enemy Randomizer ligado o jogo trocaria o clone por outro inimigo.
    Actor* a = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_TORCH2, player->actor.world.pos.x,
                           player->actor.world.pos.y, player->actor.world.pos.z, 0, player->actor.shape.rot.y, 0, 0,
                           false);
    if (a == nullptr) {
        return;
    }

    // O Dark Link nasce como BOSS e hostil; aqui vira um aliado.
    Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, a, ACTORCAT_NPC);
    a->flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
    a->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
    a->update = MirrorClone_Update;

    sCloneInput = {};
    sSwingTimer = 0;
    sClone = a;
    MirrorClone_TeleportToPlayer(a, player);
}

static void MirrorClone_Despawn() {
    if (sClone != nullptr) {
        Actor_Kill(sClone); // só marca para destruir; o jogo libera no próximo ciclo
    }
    sClone = nullptr;
}

static void OnPlayerUpdateMirrorClone() {
    if (gPlayState == nullptr) {
        return;
    }

    bool wanted = CVAR_MIRROR_CLONE_ALWAYS_VALUE ||
                  CHECK_BTN_ALL(gPlayState->state.input[0].cur.button, MIRROR_CLONE_BUTTON);
    // Só com o jogo em estado normal (sem menu de pausa, cutscene ou texto).
    wanted = wanted && gPlayState->pauseCtx.state == 0 && gPlayState->csCtx.state == CS_STATE_IDLE &&
             Message_GetState(&gPlayState->msgCtx) == TEXT_STATE_NONE;

    if (wanted) {
        MirrorClone_Spawn();
    } else {
        MirrorClone_Despawn();
    }
}

static void OnActorKillMirrorClone(void* actor) {
    if (actor == sClone) {
        sClone = nullptr;
    }
}

static void OnSceneInitMirrorClone(int16_t sceneNum) {
    sClone = nullptr; // os atores da cena anterior já foram destruídos
}

void RegisterMirrorClone() {
    if (!CVAR_MIRROR_CLONE_VALUE) {
        MirrorClone_Despawn();
    }
    COND_HOOK(OnPlayerUpdate, CVAR_MIRROR_CLONE_VALUE, OnPlayerUpdateMirrorClone);
    COND_HOOK(OnActorKill, CVAR_MIRROR_CLONE_VALUE, OnActorKillMirrorClone);
    COND_HOOK(OnSceneInit, CVAR_MIRROR_CLONE_VALUE, OnSceneInitMirrorClone);
}

static RegisterShipInitFunc initFunc(RegisterMirrorClone, { CVAR_MIRROR_CLONE_NAME });
