// MirrorLink.cpp (v1) — um LINK de verdade, clonado, que luta ao seu lado (túnica de outra cor)
// Ship of Harkinian 9.0.2 (Android) — escrito contra o código-fonte real da 9.0.2.
//
// Onde colocar: soh/soh/Enhancements/MirrorLink.cpp
// NÃO precisa registrar em lugar nenhum: o CMake pega todo soh/*.cpp (GLOB_RECURSE)
// e o RegisterShipInitFunc no fim do arquivo liga o mod sozinho na inicialização.
//
// É um mod SEPARADO do MirrorClone. Mesma ideia (corpo de Player controlado por um "controle" sintético),
// mas aqui o corpo é o do Link normal (adulto ou criança, conforme o seu save), não o do Dark Link:
//  - O corpo nasce como Dark Link (ACTOR_EN_TORCH2, único ator da 9.0.2 que é um Player completo)
//    e logo no nascimento troca o esqueleto pelo do Link (gPlayerSkelHeaders) e o desenho pelo Player_Draw.
//  - Túnica diferente da sua (padrão: azul/Zora; se você já estiver de azul, fica vermelha/Goron).
//  - Sem inimigo perto: segue você (do lado ESQUERDO; o MirrorClone fica do direito).
//  - Com inimigo terrestre na mesma altura: escolhe UM, corre até ele e golpeia até derrotá-lo.
//  - Voadores: só ataca se estiverem na altura de alcance da espada.
//  - É aliado: a espada é AT_TYPE_PLAYER (não machuca você) e ele não leva dano nem é empurrado.
//
// Ativação: aperte R (MIRROR_LINK_BUTTON) uma vez para ligar e outra para desligar.
//
// COMPATIBILIDADE COM O MirrorClone (leia): os dois mods usam ACTOR_EN_TORCH2 como corpo.
//  - Este arquivo já ignora o clone do MirrorClone (só considera "Dark Link de verdade" quem ainda roda
//    o EnTorch2_Update original).
//  - O MirrorClone v6, porém, considera QUALQUER EN_TORCH2 como Dark Link de verdade. Para os dois aparecerem
//    juntos, troque nele (MirrorClone.cpp) a linha do teste em MirrorClone_RealDarkLinkExists:
//        if (a->id == ACTOR_EN_TORCH2 && a != sClone && a->update != nullptr) {
//    por
//        if (a->id == ACTOR_EN_TORCH2 && a != sClone && a->update == EnTorch2_Update) {
//    e acrescente dentro do extern "C" do MirrorClone:   void EnTorch2_Update(Actor* thisx, PlayState* play);
//    Sem essa troca, cada mod funciona sozinho, mas se um já estiver ligado o outro não aparece.

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
// Desenho do Link normal (usa a túnica/botas do próprio Player, ao contrário do EnTorch2_Draw).
void Player_Draw(Actor* thisx, PlayState* play);
// Update original do Dark Link — serve para distinguir um Dark Link de verdade de um clone nosso.
void EnTorch2_Update(Actor* thisx, PlayState* play);
}

// ----------------- configuração -----------------
#define MIRROR_LINK_BUTTON BTN_R
#define MIRROR_LINK_TUNIC PLAYER_TUNIC_ZORA        // túnica preferida do clone (KOKIRI = verde, GORON = vermelha, ZORA = azul)
#define MIRROR_LINK_TUNIC_ALT PLAYER_TUNIC_GORON   // usada se o Link já estiver com a túnica preferida
#define MIRROR_LINK_SIDE_OFFSET (-55.0f)           // negativo = lado esquerdo do Link (o MirrorClone fica no direito)
#define MIRROR_LINK_SEARCH_RANGE 2000.0f           // raio (a partir do Link) para procurar inimigos
#define MIRROR_LINK_KEEP_RANGE 3000.0f             // alvo travado só é solto se passar disso
#define MIRROR_LINK_ATTACK_RANGE 65.0f             // distância para parar e golpear (+ parte do raio do inimigo)
#define MIRROR_LINK_SWING_PERIOD 3                 // intervalo mínimo (frames) entre apertos do botão de ataque
#define MIRROR_LINK_ATTACK_SPEED 1.75f             // multiplicador da velocidade da animação do golpe (igual ao MirrorClone)
#define MIRROR_LINK_FLOOR_TOLERANCE 100.0f         // diferença máxima entre o CHÃO do inimigo e o chão do Link
#define MIRROR_LINK_FLOAT_MAX 70.0f                // inimigo mais alto que isso acima do próprio chão = voando
#define MIRROR_LINK_MELEE_MAX_DY 80.0f             // altura máxima (pés do inimigo vs. clone) para a espada alcançar
#define MIRROR_LINK_TELEPORT_DIST 3500.0f          // longe demais do Link -> volta para perto
#define MIRROR_LINK_VOID_DROP 600.0f               // caiu abaixo disso -> volta para perto
#define MIRROR_LINK_STUCK_FRAMES 90                // sem se aproximar do alvo por tanto tempo -> desiste dele
#define MIRROR_LINK_IGNORE_FRAMES 300              // tempo que ignora um alvo inalcançável

#define CVAR_MIRROR_LINK_NAME CVAR_CHEAT("MirrorLink")
#define CVAR_MIRROR_LINK_DEFAULT 1
#define CVAR_MIRROR_LINK_VALUE CVarGetInteger(CVAR_MIRROR_LINK_NAME, CVAR_MIRROR_LINK_DEFAULT)
#define CVAR_MIRROR_LINK_ALWAYS_NAME CVAR_CHEAT("MirrorLinkAlwaysOn")
#define CVAR_MIRROR_LINK_ALWAYS_VALUE CVarGetInteger(CVAR_MIRROR_LINK_ALWAYS_NAME, 0)

static Actor* sLink = nullptr;
static Actor* sTarget = nullptr; // inimigo travado: luta com UM até derrotar
static Input sLinkInput;
static int sSwingTimer = 0;      // contagem regressiva até o próximo golpe
static bool sLinkToggled = false; // liga/desliga com um toque no botão

static Actor* sIgnoreTarget = nullptr; // alvo de que o clone desistiu (inalcançável)
static int sIgnoreTimer = 0;
static int sStuckTimer = 0;
static f32 sBestDist = 99999.0f;

static void MirrorLink_ResetTargeting() {
    sTarget = nullptr;
    sIgnoreTarget = nullptr;
    sIgnoreTimer = 0;
    sStuckTimer = 0;
    sBestDist = 99999.0f;
    sSwingTimer = 0;
}

// ---------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------

// Dark Link "de verdade" = EN_TORCH2 que ainda roda o update original. Clones (nosso ou do MirrorClone)
// trocam o update, então não contam.
static bool MirrorLink_RealDarkLinkExists(PlayState* play) {
    const int cats[] = { ACTORCAT_BOSS, ACTORCAT_ENEMY };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_EN_TORCH2 && a != sLink && a->update == EnTorch2_Update) {
                return true;
            }
        }
    }
    return false;
}

// Tipos que voam/flutuam/pairam. O clone só os ataca quando estão na altura de alcance da espada.
static bool MirrorLink_IsFlyingType(Actor* a) {
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
static f32 MirrorLink_MaxDy(Actor* a) {
    return MIRROR_LINK_MELEE_MAX_DY + CLAMP(a->colChkInfo.cylHeight * 0.3f, 0.0f, 120.0f);
}

// ---------------------------------------------------------------
// Escolha de alvo — só terrestres na mesma altura do chão (ou voadores ao alcance); um por vez.
// ---------------------------------------------------------------

// 'a' é um alvo válido? Se sim, devolve a nota (menor = melhor): perto do Link e do clone.
static bool MirrorLink_EvalTarget(Actor* clone, Player* player, Actor* a, int cat, f32 maxDistFromLink,
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
    bool flier = MirrorLink_IsFlyingType(a) || (a->world.pos.y - a->floorHeight) > MIRROR_LINK_FLOAT_MAX;
    if (flier) {
        if (ABS(a->world.pos.y - clone->world.pos.y) > MirrorLink_MaxDy(a)) {
            return false;
        }
    } else if (ABS(a->floorHeight - player->actor.floorHeight) > MIRROR_LINK_FLOOR_TOLERANCE) {
        return false; // terrestre em outro andar
    }
    f32 dLink = Actor_WorldDistXZToActor(&player->actor, a);
    if (dLink > maxDistFromLink) {
        return false;
    }
    *score = dLink + 0.35f * Actor_WorldDistXZToActor(clone, a);
    return true;
}

static Actor* MirrorLink_FindTarget(PlayState* play, Actor* clone, Player* player) {
    Actor* best = nullptr;
    f32 bestScore = 99999.0f;
    const int cats[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            f32 score;
            if (MirrorLink_EvalTarget(clone, player, a, cats[c], MIRROR_LINK_SEARCH_RANGE, &score) &&
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
static bool MirrorLink_IsValidTarget(PlayState* play, Actor* clone, Player* player, Actor* t) {
    if (t == nullptr) {
        return false;
    }
    const int cats[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            if (a == t) {
                f32 score;
                return MirrorLink_EvalTarget(clone, player, a, cats[c], MIRROR_LINK_KEEP_RANGE, &score);
            }
        }
    }
    return false;
}

static void MirrorLink_IgnoreTarget() {
    sIgnoreTarget = sTarget;
    sIgnoreTimer = MIRROR_LINK_IGNORE_FRAMES;
    sTarget = nullptr;
    sStuckTimer = 0;
    sBestDist = 99999.0f;
}

static void MirrorLink_TeleportToPlayer(Actor* clone, Player* player) {
    s16 yaw = player->actor.shape.rot.y;
    clone->world.pos.x = player->actor.world.pos.x + Math_CosS(yaw) * MIRROR_LINK_SIDE_OFFSET;
    clone->world.pos.y = player->actor.world.pos.y + 5.0f;
    clone->world.pos.z = player->actor.world.pos.z - Math_SinS(yaw) * MIRROR_LINK_SIDE_OFFSET;
    clone->prevPos = clone->world.pos;
    clone->velocity.x = clone->velocity.y = clone->velocity.z = 0.0f;
    clone->speedXZ = 0.0f;
    clone->shape.rot.y = clone->world.rot.y = yaw;
}

// Converte "ângulo + inclinação do analógico" (relativo ao mundo) em stick do controle, relativo à câmera.
static void MirrorLink_SetStick(PlayState* play, s16 angle, f32 tilt) {
    Camera* camera = Play_GetCamera(play, 0);
    s16 rel = camera->camDir.y - angle;
    sLinkInput.cur.stick_x = (s8)(tilt * Math_SinS(rel));
    sLinkInput.cur.stick_y = (s8)(tilt * Math_CosS(rel));
}

// Atualiza press/rel/prev do "controle" do clone (mesma lógica do Dark Link).
static void MirrorLink_UpdateInput() {
    u16 changed = sLinkInput.prev.button ^ sLinkInput.cur.button;
    sLinkInput.press.button = sLinkInput.cur.button & changed;
    sLinkInput.rel.button = sLinkInput.prev.button & changed;
    sLinkInput.prev.button = sLinkInput.cur.button & (u16) ~(BTN_A | BTN_B);
    PadUtils_UpdateRelXY(&sLinkInput);

    if (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)) {
        sLinkInput.rel.stick_x *= -1;
    }

    sLinkInput.press.stick_x += (s8)(sLinkInput.cur.stick_x - sLinkInput.prev.stick_x);
    sLinkInput.press.stick_y += (s8)(sLinkInput.cur.stick_y - sLinkInput.prev.stick_y);
}

// Roupa do clone: túnica diferente da do Link, botas normais, escudo Hylian.
static void MirrorLink_ApplyOutfit(Player* clone, Player* player) {
    clone->currentTunic = (player->currentTunic == MIRROR_LINK_TUNIC) ? MIRROR_LINK_TUNIC_ALT : MIRROR_LINK_TUNIC;
    clone->currentBoots = PLAYER_BOOTS_KOKIRI;
    clone->currentShield = PLAYER_SHIELD_HYLIAN;
}

// ---------------------------------------------------------------
// Update do clone — substitui o EnTorch2_Update (a IA hostil do Dark Link)
// ---------------------------------------------------------------
static void MirrorLink_Update(Actor* thisx, PlayState* play) {
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
    clone->cylinder.base.atFlags = AT_NONE;

    // O clone nunca usa a mira em primeira pessoa do Player.
    clone->unk_6AD = 0;
    clone->stateFlags1 &= ~PLAYER_STATE1_FIRST_PERSON;

    MirrorLink_ApplyOutfit(clone, player);

    // Segurança: longe demais ou caiu no vazio -> volta para perto do Link.
    if (Actor_WorldDistXZToActor(thisx, &player->actor) > MIRROR_LINK_TELEPORT_DIST ||
        thisx->world.pos.y < player->actor.world.pos.y - MIRROR_LINK_VOID_DROP) {
        MirrorLink_TeleportToPlayer(thisx, player);
    }

    sLinkInput.cur.button = 0;
    sLinkInput.cur.stick_x = sLinkInput.cur.stick_y = 0;

    if (sIgnoreTimer > 0 && --sIgnoreTimer == 0) {
        sIgnoreTarget = nullptr;
    }
    if (sSwingTimer > 0) {
        sSwingTimer--;
    }

    // Um inimigo por vez: só escolhe outro quando o atual morre, some, vira inválido ou é inalcançável.
    if (!MirrorLink_IsValidTarget(play, thisx, player, sTarget)) {
        sTarget = MirrorLink_FindTarget(play, thisx, player);
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
        f32 reach = MIRROR_LINK_ATTACK_RANGE + CLAMP(target->colChkInfo.cylRadius * 0.6f, 0.0f, 150.0f);
        f32 maxDy = MirrorLink_MaxDy(target);

        if (dist > reach) {
            MirrorLink_SetStick(play, toTarget, 127.0f);
            // Anti-travamento: se não está chegando mais perto (parede, buraco...), desiste desse alvo.
            if (dist < sBestDist - 4.0f) {
                sBestDist = dist;
                sStuckTimer = 0;
            } else if (++sStuckTimer > MIRROR_LINK_STUCK_FRAMES) {
                MirrorLink_IgnoreTarget();
            }
        } else if (dy > maxDy) {
            // Perto no plano, mas em altura que a espada não alcança: não fica batendo no ar.
            MirrorLink_IgnoreTarget();
        } else {
            sStuckTimer = 0;
            thisx->shape.rot.y = thisx->world.rot.y = clone->yaw = toTarget;
            // Só começa outro golpe quando o anterior terminou (apertar B no meio reiniciaria a animação
            // antes do frame de acerto da espada).
            if (sSwingTimer == 0 && clone->actionFunc != Player_Action_808502D0) {
                sSwingTimer = MIRROR_LINK_SWING_PERIOD;
                sLinkInput.cur.button = BTN_B;
            }
        }
    } else {
        // MODO SEGUIR: vai para o lado do Link e espera lá.
        sSwingTimer = 0;
        s16 yaw = player->actor.shape.rot.y;
        Vec3f goal;
        goal.x = player->actor.world.pos.x + Math_CosS(yaw) * MIRROR_LINK_SIDE_OFFSET;
        goal.y = player->actor.world.pos.y;
        goal.z = player->actor.world.pos.z - Math_SinS(yaw) * MIRROR_LINK_SIDE_OFFSET;
        f32 dist = Math_Vec3f_DistXZ(&thisx->world.pos, &goal);
        if (dist > 120.0f) {
            MirrorLink_SetStick(play, Math_Vec3f_Yaw(&thisx->world.pos, &goal), 127.0f);
        } else if (dist > 30.0f) {
            MirrorLink_SetStick(play, Math_Vec3f_Yaw(&thisx->world.pos, &goal), 60.0f);
        }
    }

    MirrorLink_UpdateInput();

    // Golpe de espada acelerado.
    if (clone->actionFunc == Player_Action_808502D0) {
        clone->skelAnime.playSpeed = MIRROR_LINK_ATTACK_SPEED * 1.33f;
    }

    // Roda o corpo de Player (física, colisão com parede/chão, animações, espada).
    play->playerUpdate(clone, play, &sLinkInput);

    clone->unk_6AD = 0;
    clone->stateFlags1 &= ~PLAYER_STATE1_FIRST_PERSON;
    MirrorLink_ApplyOutfit(clone, player); // o update pode ter mexido; reafirma antes do desenho

    thisx->focus.pos = thisx->world.pos;
    thisx->focus.pos.y += 20.0f;
}

// ---------------------------------------------------------------
// Ciclo de vida
// ---------------------------------------------------------------
static void MirrorLink_Spawn() {
    if (sLink != nullptr || gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr || MirrorLink_RealDarkLinkExists(gPlayState)) {
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

    // Troca o corpo: de Dark Link para Link de verdade (adulto ou criança, igual ao seu save).
    // O EnTorch2_Init já chamou playerInit uma vez (com o esqueleto do Dark Link); chamamos de novo com o
    // esqueleto do Link. Antes, apagamos o rastro de espada criado na 1ª vez para não deixar um órfão.
    Player* clone = reinterpret_cast<Player*>(a);
    Effect_Delete(gPlayState, clone->meleeWeaponEffectIndex);
    gPlayState->playerInit(clone, gPlayState, gPlayerSkelHeaders[gSaveContext.linkAge]);
    gPlayState->func_11D54(clone, gPlayState); // volta para a ação "parado", como o Init do Dark Link faz
    a->draw = Player_Draw;                      // desenho do Link normal (respeita currentTunic)
    MirrorLink_ApplyOutfit(clone, player);

    a->update = MirrorLink_Update;

    sLinkInput = {};
    MirrorLink_ResetTargeting();
    sLink = a;
    MirrorLink_TeleportToPlayer(a, player);
}

static void MirrorLink_Despawn() {
    if (sLink != nullptr) {
        Actor_Kill(sLink); // só marca para destruir; o jogo libera no próximo ciclo
    }
    MirrorLink_ResetTargeting();
    sLink = nullptr;
}

static void OnPlayerUpdateMirrorLink() {
    if (gPlayState == nullptr) {
        return;
    }

    // Um toque no botão liga; outro toque desliga.
    if (CHECK_BTN_ALL(gPlayState->state.input[0].press.button, MIRROR_LINK_BUTTON)) {
        sLinkToggled = !sLinkToggled;
    }
    bool wanted = CVAR_MIRROR_LINK_ALWAYS_VALUE || sLinkToggled;
    // Só com o jogo em estado normal (sem menu de pausa, cutscene ou texto).
    wanted = wanted && gPlayState->pauseCtx.state == 0 && gPlayState->csCtx.state == CS_STATE_IDLE &&
             Message_GetState(&gPlayState->msgCtx) == TEXT_STATE_NONE;

    if (wanted) {
        MirrorLink_Spawn();
    } else {
        MirrorLink_Despawn();
    }
}

static void OnActorKillMirrorLink(void* actor) {
    if (actor == sLink) {
        sLink = nullptr;
    }
}

static void OnSceneInitMirrorLink(int16_t sceneNum) {
    sLink = nullptr; // os atores da cena anterior já foram destruídos
    MirrorLink_ResetTargeting();
}

void RegisterMirrorLink() {
    if (!CVAR_MIRROR_LINK_VALUE) {
        MirrorLink_Despawn();
    }
    COND_HOOK(OnPlayerUpdate, CVAR_MIRROR_LINK_VALUE, OnPlayerUpdateMirrorLink);
    COND_HOOK(OnActorKill, CVAR_MIRROR_LINK_VALUE, OnActorKillMirrorLink);
    COND_HOOK(OnSceneInit, CVAR_MIRROR_LINK_VALUE, OnSceneInitMirrorLink);
}

static RegisterShipInitFunc initFunc(RegisterMirrorLink, { CVAR_MIRROR_LINK_NAME });
