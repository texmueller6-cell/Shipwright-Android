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
//  - Com inimigo/chefe terrestre por perto: corre até ele e ataca com a espada.
//  - Com inimigo/chefe que voa ou plana: para a distância, saca o arco, puxa, mira e atira flechas nele.
//  - É aliado: a espada dele é AT_TYPE_PLAYER (machuca inimigos, não machuca você)
//    e ele não leva dano nem é empurrado.
//
// Ativação: aperte L (MIRROR_CLONE_BUTTON) uma vez para ligar e outra para desligar.
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
#include "overlays/actors/ovl_En_Arrow/z_en_arrow.h"
extern PlayState* gPlayState;
#include "objects/gameplay_keep/gameplay_keep.h"
// Não estão nos headers; as funções existem no executável (z_player.c / z_en_arrow.c são compilados junto).
void EnArrow_Fly(EnArrow* self, PlayState* play);
void Player_InitItemActionWithAnim(PlayState* play, Player* self, s8 itemAction);
void Player_SetUpperActionFunc(Player* self, UpperActionFunc upperActionFunc);
s32 func_8083501C(Player* self, PlayState* play); // upper action "arco na mão, parado"
}

// ----------------- configuração -----------------
#define MIRROR_CLONE_BUTTON BTN_L
#define MIRROR_CLONE_SIDE_OFFSET 55.0f      // distância lateral quando só segue o Link
#define MIRROR_CLONE_SEARCH_RANGE 450.0f    // raio (a partir do Link) para procurar inimigos
#define MIRROR_CLONE_ATTACK_RANGE 65.0f     // distância para parar e golpear
#define MIRROR_CLONE_SWING_PERIOD 14        // frames entre golpes
#define MIRROR_CLONE_ARROW_RANGE 380.0f     // distância máxima para atirar flechas em inimigos voadores
#define MIRROR_CLONE_ARROW_PERIOD 30        // frames entre flechas
#define MIRROR_CLONE_FLY_HEIGHT 80.0f       // alvo acima disso (e fora do chão) conta como voador
#define MIRROR_CLONE_BOW_ANIMATION 1        // 1 = clone saca o arco e faz a animação; 0 = só solta a flecha
#define MIRROR_CLONE_AIM_HOLD_FRAMES 6      // frames mirando antes de soltar
#define MIRROR_CLONE_TELEPORT_DIST 800.0f   // longe demais do Link -> volta para perto
#define MIRROR_CLONE_VOID_DROP 600.0f       // caiu abaixo disso -> volta para perto

#define CVAR_MIRROR_CLONE_NAME CVAR_CHEAT("MirrorClone")
#define CVAR_MIRROR_CLONE_DEFAULT 1
#define CVAR_MIRROR_CLONE_VALUE CVarGetInteger(CVAR_MIRROR_CLONE_NAME, CVAR_MIRROR_CLONE_DEFAULT)
#define CVAR_MIRROR_CLONE_ALWAYS_NAME CVAR_CHEAT("MirrorCloneAlwaysOn")
#define CVAR_MIRROR_CLONE_ALWAYS_VALUE CVarGetInteger(CVAR_MIRROR_CLONE_ALWAYS_NAME, 0)

static Actor* sClone = nullptr;
static Actor* sTarget = nullptr; // inimigo travado: o clone luta com UM por vez
static Input sCloneInput;
static int sSwingTimer = 0;
static bool sCloneToggled = false; // liga/desliga com um toque no botão

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

// O alvo travado ainda existe, está vivo e perto o bastante? (procura o ponteiro nas listas de atores,
// porque o ator pode ter sido destruído e o ponteiro ficado solto)
static bool MirrorClone_IsValidTarget(PlayState* play, Player* player, Actor* t) {
    if (t == nullptr) {
        return false;
    }
    const int cats[] = { ACTORCAT_ENEMY, ACTORCAT_BOSS };
    for (int c = 0; c < 2; c++) {
        for (Actor* a = play->actorCtx.actorLists[cats[c]].head; a != nullptr; a = a->next) {
            if (a != t) {
                continue;
            }
            if (a->update == nullptr || a->draw == nullptr) {
                return false;
            }
            if (!(a->flags & (ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE))) {
                return false;
            }
            if (a->colChkInfo.health <= 0 && cats[c] == ACTORCAT_ENEMY) {
                return false;
            }
            return Actor_WorldDistXZToActor(&player->actor, a) < MIRROR_CLONE_SEARCH_RANGE * 1.5f;
        }
    }
    return false;
}

// O Link está no caminho entre o clone e o alvo? (aí o golpe/flecha pareceria ir "para cima do Link")
static bool MirrorClone_PlayerInWay(Actor* clone, Player* player, Actor* target, s16 toTarget, f32 reach,
                                    s16 cone) {
    f32 dLink = Actor_WorldDistXZToActor(clone, &player->actor);
    f32 dTarget = Actor_WorldDistXZToActor(clone, target);
    if (dLink > reach || dLink > dTarget + 25.0f) {
        return false;
    }
    s16 toLink = Actor_WorldYawTowardActor(clone, &player->actor);
    s16 diff = (s16)(toLink - toTarget);
    return ABS(diff) < cone;
}

// Ângulo para contornar o Link: anda de lado, para o lado em que o Link NÃO está.
static s16 MirrorClone_SidestepAngle(Actor* clone, Player* player, s16 toTarget) {
    s16 toLink = Actor_WorldYawTowardActor(clone, &player->actor);
    s16 diff = (s16)(toLink - toTarget);
    return (diff >= 0) ? (s16)(toTarget - 0x4000) : (s16)(toTarget + 0x4000);
}

// Inimigo que voa/plana? (lista de conhecidos + heurística de altura)
static bool MirrorClone_IsFlyingTarget(Actor* clone, Actor* target) {
    switch (target->id) {
        case ACTOR_EN_FIREFLY:    // Keese
        case ACTOR_EN_PEEHAT:     // Peahat
        case ACTOR_EN_BILI:       // Biri
        case ACTOR_EN_VALI:       // Bari
        case ACTOR_EN_BB:         // Bubble (caveira flamejante)
        case ACTOR_EN_BUBBLE:     // Shabom
        case ACTOR_EN_TP:         // Tailpasaran
        case ACTOR_EN_POH:        // Poe
        case ACTOR_EN_PO_FIELD:   // Poe do campo
        case ACTOR_EN_PO_SISTERS: // Poe Sisters
        case ACTOR_BOSS_VA:       // Barinade
        case ACTOR_BOSS_FD2:      // Volvagia (voando)
        case ACTOR_BOSS_TW:       // Twinrova
            return true;
        default:
            break;
    }
    bool onGround = (target->bgCheckFlags & 1) != 0;
    return !onGround && (target->world.pos.y - clone->world.pos.y) > MIRROR_CLONE_FLY_HEIGHT;
}

// Dispara uma flecha normal do clone em direção ao alvo (não gasta flechas do Link).
static void MirrorClone_ShootArrow(PlayState* play, Actor* clone, Actor* target) {
    Vec3f from = clone->world.pos;
    from.y += 40.0f;

    Vec3f aim = target->focus.pos;
    if (aim.x == 0.0f && aim.y == 0.0f && aim.z == 0.0f) {
        aim = target->world.pos;
    }

    s16 yaw = Math_Vec3f_Yaw(&from, &aim);
    s16 pitch = Math_Vec3f_Pitch(&from, &aim); // positivo = para baixo (convenção do rot.x)

    Actor* a = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_ARROW, from.x + Math_SinS(yaw) * 20.0f, from.y,
                           from.z + Math_CosS(yaw) * 20.0f, pitch, yaw, 0, ARROW_NORMAL, false);
    if (a == nullptr) {
        return;
    }

    // O EnArrow_Shoot original mata a flecha se não houver o Link mirando (parent == NULL e
    // player->unk_A73 == 0). Então aplicamos aqui o mesmo "lançamento" que ele faria.
    EnArrow* arrow = reinterpret_cast<EnArrow*>(a);
    arrow->actionFunc = EnArrow_Fly;
    Math_Vec3f_Copy(&arrow->unk_210, &a->world.pos);
    Actor_SetProjectileSpeed(a, 150.0f);
    arrow->timer = 12;
    Audio_PlayActorSound2(clone, NA_SE_IT_ARROW_SHOT);
}

#if MIRROR_CLONE_BOW_ANIMATION
// ---------------------------------------------------------------
// Animação do arco (própria, só do corpo — NÃO usa o fluxo de mira do Link, que mexeria na câmera)
// ---------------------------------------------------------------
static int sBowPhase = 0; // 0 = sem tiro em andamento, 1 = puxando, 2 = mirando, 3 = soltando, 4 = recuperando
static int sBowTimer = 0;
static bool sBowFireNow = false; // a animação pede: "solte a flecha agora"
static s8 sSwordAction = -1;     // espada que o clone segurava (para voltar depois)
static u8 sSwordItemId = 0;

static LinkAnimationHeader* MirrorClone_Anim(const char* name) {
    return reinterpret_cast<LinkAnimationHeader*>(const_cast<char*>(name));
}

// Upper action do clone enquanto atira: devolve true = "parte de cima ocupada" (usa upperSkelAnime).
static s32 MirrorClone_UpperActionBow(Player* self, PlayState* play) {
    switch (sBowPhase) {
        case 1: // puxa o arco
            if (LinkAnimation_Update(play, &self->upperSkelAnime)) {
                sBowPhase = 2;
                sBowTimer = MIRROR_CLONE_AIM_HOLD_FRAMES;
            }
            return true;
        case 2: // segura a mira
            if (--sBowTimer <= 0) {
                LinkAnimation_PlayOnce(play, &self->upperSkelAnime,
                                       MirrorClone_Anim(gPlayerAnim_link_bow_bow_shoot_next));
                sBowFireNow = true; // o update do clone solta a flecha
                sBowPhase = 3;
            }
            return true;
        case 3: // solta
            if (LinkAnimation_Update(play, &self->upperSkelAnime)) {
                LinkAnimation_PlayOnce(play, &self->upperSkelAnime,
                                       MirrorClone_Anim(gPlayerAnim_link_bow_bow_shoot_end));
                sBowPhase = 4;
            }
            return true;
        case 4: // recolhe o arco
            if (LinkAnimation_Update(play, &self->upperSkelAnime)) {
                sBowPhase = 0;
                Player_SetUpperActionFunc(self, func_8083501C);
                return false;
            }
            return true;
        default:
            sBowPhase = 0;
            Player_SetUpperActionFunc(self, func_8083501C);
            return false;
    }
}

// 0 = não dá para trocar (usa o fallback sem animação), 1 = arco na mão, 2 = espere (espadada em andamento)
static int MirrorClone_EquipBow(PlayState* play, Player* clone) {
    if (clone->heldItemAction == PLAYER_IA_BOW) {
        return 1;
    }
    if (clone->heldItemAction < PLAYER_IA_SWORD_MASTER || clone->heldItemAction > PLAYER_IA_SWORD_BIGGORON) {
        return 0;
    }
    if (clone->meleeWeaponState != 0) {
        return 2;
    }
    sSwordAction = clone->heldItemAction;
    sSwordItemId = clone->heldItemId;
    clone->nextModelGroup = PLAYER_MODELGROUP_BOW_SLINGSHOT;
    Player_InitItemActionWithAnim(play, clone, PLAYER_IA_BOW);
    clone->heldItemId = ITEM_BOW;
    return 1;
}

static void MirrorClone_EquipSword(PlayState* play, Player* clone) {
    if (clone->heldItemAction != PLAYER_IA_BOW || sBowPhase != 0 || sSwordAction < 0) {
        return;
    }
    clone->nextModelGroup = PLAYER_MODELGROUP_SWORD_AND_SHIELD;
    Player_InitItemActionWithAnim(play, clone, sSwordAction);
    clone->heldItemId = sSwordItemId;
}

static void MirrorClone_ResetBow() {
    sBowPhase = 0;
    sBowTimer = 0;
    sBowFireNow = false;
    sSwordAction = -1;
}
#else
static void MirrorClone_EquipSword(PlayState*, Player*) {}
static void MirrorClone_ResetBow() {}
#endif

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

    // Segurança: longe demais ou caiu no vazio -> volta para perto do Link.
    if (Actor_WorldDistXZToActor(thisx, &player->actor) > MIRROR_CLONE_TELEPORT_DIST ||
        thisx->world.pos.y < player->actor.world.pos.y - MIRROR_CLONE_VOID_DROP) {
        MirrorClone_TeleportToPlayer(thisx, player);
    }

    sCloneInput.cur.button = 0;
    sCloneInput.cur.stick_x = sCloneInput.cur.stick_y = 0;

    // Um inimigo por vez: só troca de alvo quando o atual morre, some ou fica longe demais.
    if (!MirrorClone_IsValidTarget(play, player, sTarget)) {
        sTarget = MirrorClone_FindTarget(play, player);
        sSwingTimer = 0;
    }
    Actor* target = sTarget;

#if MIRROR_CLONE_BOW_ANIMATION
    if (sBowFireNow) {
        sBowFireNow = false;
        if (target != nullptr) {
            MirrorClone_ShootArrow(play, thisx, target);
        }
    }
#endif

    if (target != nullptr) {
        // MODO LUTA: corre até o alvo; perto dele, para, encara e golpeia.
        f32 dist = Actor_WorldDistXZToActor(thisx, target);
        s16 toTarget = Actor_WorldYawTowardActor(thisx, target);
        if (MirrorClone_IsFlyingTarget(thisx, target)) {
            // VOADOR/PLANADOR: chega até o alcance do arco, para, mira e atira flechas.
            if (dist > MIRROR_CLONE_ARROW_RANGE) {
                MirrorClone_SetStick(play, toTarget, 127.0f);
                sSwingTimer = 0;
            } else if (sBowPhase == 0 &&
                       MirrorClone_PlayerInWay(thisx, player, target, toTarget, MIRROR_CLONE_ARROW_RANGE, 0x1800)) {
                // Link na linha de tiro: contorna antes de mirar.
                MirrorClone_SetStick(play, MirrorClone_SidestepAngle(thisx, player, toTarget), 127.0f);
                sSwingTimer = 0;
            } else {
                thisx->shape.rot.y = thisx->world.rot.y = toTarget;
#if MIRROR_CLONE_BOW_ANIMATION
                int bow = MirrorClone_EquipBow(play, clone);
                if (bow == 1) {
                    // Saca o arco, puxa, mira e solta (a flecha sai quando a animação pede).
                    if (sBowPhase == 0 && (sSwingTimer++ % MIRROR_CLONE_ARROW_PERIOD) == 0) {
                        sBowPhase = 1;
                        Player_SetUpperActionFunc(clone, MirrorClone_UpperActionBow);
                        LinkAnimation_PlayOnce(play, &clone->upperSkelAnime,
                                               MirrorClone_Anim(gPlayerAnim_link_bow_bow_ready));
                    }
                } else if (bow == 0 && (sSwingTimer++ % MIRROR_CLONE_ARROW_PERIOD) == 0) {
                    MirrorClone_ShootArrow(play, thisx, target); // fallback sem animação
                }
#else
                if ((sSwingTimer++ % MIRROR_CLONE_ARROW_PERIOD) == 0) {
                    MirrorClone_ShootArrow(play, thisx, target);
                }
#endif
            }
        } else if (MirrorClone_EquipSword(play, clone), dist > MIRROR_CLONE_ATTACK_RANGE) {
            // TERRESTRE: corre até o alvo...
            MirrorClone_SetStick(play, toTarget, 127.0f);
            sSwingTimer = 0;
        } else if (MirrorClone_PlayerInWay(thisx, player, target, toTarget, MIRROR_CLONE_ATTACK_RANGE + 45.0f,
                                           0x3000)) {
            // Link no caminho: NÃO golpeia (pareceria atacar o Link). Contorna até ter linha livre.
            MirrorClone_SetStick(play, MirrorClone_SidestepAngle(thisx, player, toTarget), 127.0f);
            sSwingTimer = 0;
        } else {
            // ...e golpeia com a espada.
            thisx->shape.rot.y = thisx->world.rot.y = toTarget;
            if (clone->heldItemAction != PLAYER_IA_BOW && (sSwingTimer++ % MIRROR_CLONE_SWING_PERIOD) == 0) {
                sCloneInput.cur.button = BTN_B;
            }
        }
    } else {
        // MODO SEGUIR: vai para o lado do Link e espera lá.
        sSwingTimer = 0;
        MirrorClone_EquipSword(play, clone);
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
