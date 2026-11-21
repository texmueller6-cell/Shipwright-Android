// MirrorClone.cpp (v6) — clone aliado do Link que LUTA ao seu lado
// Ship of Harkinian 9.0.2 (Android) — escrito contra o código-fonte real da 9.0.2.
//
// Onde colocar: soh/soh/Enhancements/MirrorClone.cpp
// NÃO precisa registrar em lugar nenhum: o CMake pega todo soh/*.cpp (GLOB_RECURSE)
// e o RegisterShipInitFunc no fim do arquivo liga o mod sozinho na inicialização.
//
// Como funciona:
//  - O clone é o corpo do Dark Link (ACTOR_EN_TORCH2), que na 9.0.2 é um Player completo.
//    Ele roda o MESMO código do Link (play->playerUpdate) controlado por um "controle" sintético.
//  - Sem inimigo terrestre por perto: segue você (fica ao seu lado).
//  - Com inimigo terrestre: escolhe UM, corre até ele e golpeia com a espada até derrotá-lo; só então
//    passa para o próximo. A área de busca é enorme (cobre a área toda).
//  - Inimigos terrestres: só ataca se o chão deles estiver na mesma altura do chão dele (ignora outros andares).
//  - Inimigos que voam/flutuam (Keese, Peahat, Bubble, Poe, corvos, Skulltulas, vaso voador, placa giratória...):
//    só ataca se estiverem na altura de alcance da espada dele. Fora disso, ignora.
//  - Ataca 3x mais rápido (ver MIRROR_CLONE_ATTACK_SPEED).
//  - Não existe mais mecânica de arco.
//  - É aliado: a espada dele é AT_TYPE_PLAYER (machuca inimigos, não machuca você)
//    e ele não leva dano nem é empurrado.
//
// Ativação: aperte L (MIRROR_CLONE_BUTTON) uma vez para ligar e outra para desligar.
//
// Limite importante: se já existir um Dark Link de verdade na cena, o clone NÃO aparece.

#include <libultraship/bridge.h>
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "macros.h"
#include "z64.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
// Action de "golpe de espada" do Player (z_player.c é compilado junto).
void Player_Action_808502D0(Player* self, PlayState* play);
}

// ----------------- configuração -----------------
#define MIRROR_CLONE_BUTTON BTN_L
#define MIRROR_CLONE_SIDE_OFFSET 55.0f      // distância lateral quando só segue o Link
#define MIRROR_CLONE_SEARCH_RANGE 2000.0f   // raio (a partir do Link) para procurar inimigos — área enorme
#define MIRROR_CLONE_KEEP_RANGE 3000.0f     // alvo travado só é solto se passar disso
#define MIRROR_CLONE_ATTACK_RANGE 65.0f     // distância para parar e golpear (+ parte do raio do inimigo)
#define MIRROR_CLONE_SWING_PERIOD 3         // intervalo mínimo (frames) entre apertos do botão de ataque
#define MIRROR_CLONE_ATTACK_SPEED 1.75f     // multiplicador da velocidade da animação do golpe (2.0 era rápido demais; 3.0 pulava o frame do acerto)
#define MIRROR_CLONE_FLOOR_TOLERANCE 100.0f // diferença máxima entre o CHÃO do inimigo e o chão do Link
#define MIRROR_CLONE_FLOAT_MAX 70.0f        // inimigo mais alto que isso acima do próprio chão = flutuando/voando
#define MIRROR_CLONE_MELEE_MAX_DY 80.0f     // altura máxima (pés do inimigo vs. clone) para a espada alcançar
#define MIRROR_CLONE_TELEPORT_DIST 3500.0f  // longe demais do Link -> volta para perto
#define MIRROR_CLONE_VOID_DROP 600.0f       // caiu abaixo disso -> volta para perto
#define MIRROR_CLONE_STUCK_FRAMES 90        // sem se aproximar do alvo por tanto tempo -> desiste dele
#define MIRROR_CLONE_IGNORE_FRAMES 300      // tempo que ignora um alvo inalcançável

#define CVAR_MIRROR_CLONE_NAME CVAR_CHEAT("MirrorClone")
#define CVAR_MIRROR_CLONE_DEFAULT 1
#define CVAR_MIRROR_CLONE_VALUE CVarGetInteger(CVAR_MIRROR_CLONE_NAME, CVAR_MIRROR_CLONE_DEFAULT)
#define CVAR_MIRROR_CLONE_ALWAYS_NAME CVAR_CHEAT("MirrorCloneAlwaysOn")
#define CVAR_MIRROR_CLONE_ALWAYS_VALUE CVarGetInteger(CVAR_MIRROR_CLONE_ALWAYS_NAME, 0)

static Actor* sClone = nullptr;
static Actor* sTarget = nullptr; // inimigo travado: luta com UM até derrotar
static Input sCloneInput;
static int sSwingTimer = 0;        // contagem regressiva até o próximo golpe
static bool sCloneToggled = false; // liga/desliga com um toque no botão

static Actor* sIgnoreTarget = nullptr; // alvo de que o clone desistiu (inalcançável)
static int sIgnoreTimer = 0;
static int sStuckTimer = 0;
static f32 sBestDist = 99999.0f;

static void MirrorClone_ResetTargeting() {
    sTarget = nullptr;
    sIgnoreTarget = nullptr;
    sIgnoreTimer = 0;
    sStuckTimer = 0;
    sBestDist = 99999.0f;
    sSwingTimer = 0;
}

static void MirrorClone_ResetBow() { // nome mantido pelos chamadores; agora só zera o controle de alvo
    MirrorClone_ResetTargeting();
}

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

// Tipos que voam/flutuam/pairam. O clone só os ataca quando estão na altura de alcance da espada.
static bool MirrorClone_IsFlyingType(Actor* a) {
    switch (a->id) {
        case ACTOR_EN_FIREFLY:    // Keese
        case ACTOR_EN_PEEHAT:     // Peahat
        case ACTOR_EN_BILI:       // Biri
        case ACTOR_EN_VALI:       // Bari
        case ACTOR_EN_BB:         // Bubble (caveira flutuante)
        case ACTOR_EN_BUBBLE:     // Shabom
        case ACTOR_EN_TP:         // Tailpasaran
        case ACTOR_EN_POH:        // Poe
        case ACTOR_EN_PO_FIELD:   // Poe do campo
        case ACTOR_EN_PO_SISTERS: // Poe Sisters
        case ACTOR_EN_CROW:       // Guay (corvo)
        case ACTOR_EN_ST:         // Skulltula
        case ACTOR_EN_SW:         // Gold Skulltula
        case ACTOR_EN_TUBO_TRAP:  // vaso voador
        case ACTOR_EN_YUKABYUN:   // placa/azulejo giratório voador
        case ACTOR_EN_WALLMAS:    // Wallmaster
        case ACTOR_BOSS_VA:       // Barinade
        case ACTOR_BOSS_FD:       // Volvagia
        case ACTOR_BOSS_FD2:      // Volvagia (voando)
        case ACTOR_BOSS_TW:       // Twinrova
            return true;
        default:
            return false;
    }
}

// Altura máxima (pés do inimigo vs. clone) para a espada alcançar. Cresce um pouco com inimigos grandes.
static f32 MirrorClone_MaxDy(Actor* a) {
    return MIRROR_CLONE_MELEE_MAX_DY + CLAMP(a->colChkInfo.cylHeight * 0.3f, 0.0f, 120.0f);
}

// ---------------------------------------------------------------
// Escolha de alvo — só terrestres, na mesma altura do chão; um por vez até derrotar.
// ---------------------------------------------------------------

// 'a' é um alvo válido? Se sim, devolve a nota (menor = melhor): perto do Link e do clone.
static bool MirrorClone_EvalTarget(Actor* clone, Player* player, Actor* a, int cat, f32 maxDistFromLink,
                                   f32* score) {
    if (a == clone || a->update == nullptr || a->draw == nullptr) {
        return false;
    }
    if (!(a->flags & (ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE))) {
        return false; // não é um alvo "de verdade" (morto, decorativo, etc.)
    }
    if (cat == ACTORCAT_ENEMY && a->colChkInfo.health <= 0) {
        return false;
    }
    if (a == sIgnoreTarget && sIgnoreTimer > 0) {
        return false;
    }
    // Voando/flutuando = tipo de voador, ou corpo bem acima do próprio chão. (Independe do tamanho: a origem
    // do ator fica nos pés, então um inimigo gigante no chão não conta como voador.)
    bool flier = MirrorClone_IsFlyingType(a) || (a->world.pos.y - a->floorHeight) > MIRROR_CLONE_FLOAT_MAX;
    if (flier) {
        // REGRA: voador só é alvo se estiver na altura de alcance da espada do clone. Senão, ignora.
        if (ABS(a->world.pos.y - clone->world.pos.y) > MirrorClone_MaxDy(a)) {
            return false;
        }
    } else if (ABS(a->floorHeight - player->actor.floorHeight) > MIRROR_CLONE_FLOOR_TOLERANCE) {
        // Terrestre: o chão dele tem que estar no nível do chão do Link (não vale outro andar).
        return false;
    }
    f32 dLink = Actor_WorldDistXZToActor(&player->actor, a);
    if (dLink > maxDistFromLink) {
        return false;
    }
    *score = dLink + 0.35f * Actor_WorldDistXZToActor(clone, a);
    return true;
}

static Actor* MirrorClone_FindTarget(PlayState* play, Actor* clone, Player* player) {
    Actor* best = nullptr;
    f32 bestScore = 99999.0f;
    const int cats[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            f32 score;
            if (MirrorClone_EvalTarget(clone, player, a, cats[c], MIRROR_CLONE_SEARCH_RANGE, &score) &&
                score < bestScore) {
                bestScore = score;
                best = a;
            }
        }
    }
    return best;
}

// O alvo travado ainda existe e continua válido? (procura o ponteiro nas listas, porque o ator pode
// ter sido destruído e o ponteiro ficado solto)
static bool MirrorClone_IsValidTarget(PlayState* play, Actor* clone, Player* player, Actor* t) {
    if (t == nullptr) {
        return false;
    }
    const int cats[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            if (a == t) {
                f32 score;
                return MirrorClone_EvalTarget(clone, player, a, cats[c], MIRROR_CLONE_KEEP_RANGE, &score);
            }
        }
    }
    return false;
}

static void MirrorClone_IgnoreTarget() {
    sIgnoreTarget = sTarget;
    sIgnoreTimer = MIRROR_CLONE_IGNORE_FRAMES;
    sTarget = nullptr;
    sStuckTimer = 0;
    sBestDist = 99999.0f;
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
    // Os golpes do clone são "do jogador": só acertam inimigos (AC_TYPE_PLAYER), nunca o Link (AC_TYPE_ENEMY).
    clone->cylinder.base.atFlags = AT_NONE;

    // O clone nunca usa a mira em primeira pessoa do Player.
    clone->unk_6AD = 0;
    clone->stateFlags1 &= ~PLAYER_STATE1_FIRST_PERSON;

    // Segurança: longe demais ou caiu no vazio -> volta para perto do Link.
    if (Actor_WorldDistXZToActor(thisx, &player->actor) > MIRROR_CLONE_TELEPORT_DIST ||
        thisx->world.pos.y < player->actor.world.pos.y - MIRROR_CLONE_VOID_DROP) {
        MirrorClone_TeleportToPlayer(thisx, player);
    }

    sCloneInput.cur.button = 0;
    sCloneInput.cur.stick_x = sCloneInput.cur.stick_y = 0;

    if (sIgnoreTimer > 0 && --sIgnoreTimer == 0) {
        sIgnoreTarget = nullptr;
    }
    if (sSwingTimer > 0) {
        sSwingTimer--;
    }

    // Um inimigo por vez: só escolhe outro quando o atual morre, some, vira inválido ou é inalcançável.
    if (!MirrorClone_IsValidTarget(play, thisx, player, sTarget)) {
        sTarget = MirrorClone_FindTarget(play, thisx, player);
        sSwingTimer = 0;
        sStuckTimer = 0;
        sBestDist = 99999.0f;
    }
    Actor* target = sTarget;

    if (target != nullptr) {
        // MODO LUTA: corre até o alvo, para perto dele, encara e golpeia sem parar.
        f32 dist = Actor_WorldDistXZToActor(thisx, target);
        s16 toTarget = Actor_WorldYawTowardActor(thisx, target);
        f32 dy = ABS(target->world.pos.y - thisx->world.pos.y);
        f32 reach = MIRROR_CLONE_ATTACK_RANGE + CLAMP(target->colChkInfo.cylRadius * 0.6f, 0.0f, 150.0f);
        f32 maxDy = MirrorClone_MaxDy(target);

        if (dist > reach) {
            MirrorClone_SetStick(play, toTarget, 127.0f);
            // Anti-travamento: se não está chegando mais perto (parede, buraco...), desiste desse alvo.
            if (dist < sBestDist - 4.0f) {
                sBestDist = dist;
                sStuckTimer = 0;
            } else if (++sStuckTimer > MIRROR_CLONE_STUCK_FRAMES) {
                MirrorClone_IgnoreTarget();
            }
        } else if (dy > maxDy) {
            // Perto no plano, mas em altura que a espada não alcança: não fica batendo no ar.
            MirrorClone_IgnoreTarget();
        } else {
            sStuckTimer = 0;
            thisx->shape.rot.y = thisx->world.rot.y = clone->yaw = toTarget;
            // Só começa outro golpe quando o anterior terminou: apertar B no meio da animação a reiniciava
            // antes do frame de acerto da espada (o clone "atacava" mas não acertava ninguém).
            if (sSwingTimer == 0 && clone->actionFunc != Player_Action_808502D0) {
                sSwingTimer = MIRROR_CLONE_SWING_PERIOD;
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

    // Golpe de espada acelerado: enquanto está na animação de ataque, ela toca MIRROR_CLONE_ATTACK_SPEED vezes mais rápido.
    if (clone->actionFunc == Player_Action_808502D0) {
        clone->skelAnime.playSpeed = MIRROR_CLONE_ATTACK_SPEED * 1.33f;
    }

    // Roda o corpo de Player (física, colisão com parede/chão, animações, espada).
    play->playerUpdate(clone, play, &sCloneInput);

    clone->unk_6AD = 0;
    clone->stateFlags1 &= ~PLAYER_STATE1_FIRST_PERSON;

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
    sTarget = nullptr;
    MirrorClone_ResetBow();
    sClone = a;
    MirrorClone_TeleportToPlayer(a, player);
}

static void MirrorClone_Despawn() {
    if (sClone != nullptr) {
        Actor_Kill(sClone); // só marca para destruir; o jogo libera no próximo ciclo
    }
    MirrorClone_ResetBow();
    sClone = nullptr;
    sTarget = nullptr;
}

static void OnPlayerUpdateMirrorClone() {
    if (gPlayState == nullptr) {
        return;
    }

    // Um toque no botão liga; outro toque desliga.
    if (CHECK_BTN_ALL(gPlayState->state.input[0].press.button, MIRROR_CLONE_BUTTON)) {
        sCloneToggled = !sCloneToggled;
    }
    bool wanted = CVAR_MIRROR_CLONE_ALWAYS_VALUE || sCloneToggled;
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
    sTarget = nullptr;
    MirrorClone_ResetBow();
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


