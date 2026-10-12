/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_revenge.c - Harry's Revenge (see pc_revenge.h).
 *
 * Extra enemies are cloned from native spawns rather than added as new spawn
 * rows: a clone appears only where and when the map would already spawn that
 * monster, so story masks, SysFlag_NoEnemySpawn, difficulty gates and the
 * distance trigger all still apply, and scripted rooms and boss fights are
 * left alone. Clones are flagged in g_PcNpcDebugSpawned so killing one never
 * writes the savegame's per-row kill bits, and they borrow the parent row's
 * field_40 because field_40 must stay below 32 (it indexes the 32-bit row masks).
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/items.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/map/map.h"
#include "bodyprog/math/math.h"
#include "bodyprog/savegame.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/screen/screen_data.h" /* g_DeltaTimeRaw */

#include "pc_config.h"
#include "pc_revenge.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern u8 g_PcNpcDebugSpawned[NPC_COUNT_MAX];
extern const char* PcPort_GetGameDataPath(void);
extern int         XaPlayer_PlayFile(const char* path);
extern int         Pc_MsgVoice_Holding(void);

/* Slots kept free for scripted Chara_Spawn calls (story actors, bosses), which
 * fail outright when every npcs[] slot is taken. */
#define REVENGE_SLOT_RESERVE 4

#define REVENGE_REGEN_DELAY Q12(5.0f) /* seconds without a hit before healing starts */
#define REVENGE_REGEN_RATE  Q12(5.0f) /* health per second */

/* Regular monsters only. Bosses, swarms (Larval Stalker), single-instance AIs
 * (Twinfeeler, Floatstinger, Split Head) and story actors read per-map statics
 * or assume they are alone, so they are never cloned. */
static const u8 CLONEABLE[] = {
    Chara_AirScreamer, Chara_NightFlutter, Chara_Groaner, Chara_Wormhead,
    Chara_Stalker, Chara_GreyChild, Chara_Mumbler, Chara_HangedScratcher,
    Chara_Creeper, Chara_Romper, Chara_PuppetNurse, Chara_PuppetDoctor,
};

/* Invader types with the room-spawn stateStep their init expects; a wrong value
 * can leave a monster inert (see RANDO_MONSTERS in pc_rando.c, the same table). */
typedef struct { u8 charaId; u8 stateStep; } s_Invader;
static const s_Invader INVADERS[] = {
    { Chara_GreyChild,   3  },
    { Chara_PuppetNurse, 17 },
    { Chara_Romper,      3  },
    { Chara_Groaner,     3  },
    { Chara_AirScreamer, 12 },
};

#define REVENGE_BARK_COOLDOWN Q12(6.0f)
#define REVENGE_BARK_TAKES    8
#define REVENGE_AREA_DELAY    Q12(2.5f) /* let the area fade in before Harry speaks */

static u8     s_isClone[NPC_COUNT_MAX];
static q19_12 s_barkCooldown = 0;
static q19_12 s_areaBarkIn   = -1;
static q19_12 s_sinceHit = 0;
static u32    s_rng      = 0x9E3779B9u;

/* Own generator: drawing from the game's RNG would shift every vanilla roll
 * that follows (enemy AI, item drops). */
static u32 rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int chance(int pct) { return (int)(rnd() % 100u) < pct; }

/* Harry's lines live in gamedata/revenge/bark_<event>_<n>.wav (n from 1); one
 * take is picked at random. They play through the voice source, which refuses
 * while a disc line is playing, so Harry never talks over a cutscene. */
static int bark_takes(const char* event)
{
    static struct { const char* event; int takes; } cache[8];
    static int cached = 0;
    char       path[512];
    struct stat st;
    int        i, n;

    for (i = 0; i < cached; i++)
        if (cache[i].event == event)
            return cache[i].takes;

    for (n = 0; n < REVENGE_BARK_TAKES; n++)
    {
        snprintf(path, sizeof(path), "%s/revenge/bark_%s_%d.wav", PcPort_GetGameDataPath(), event, n + 1);
        if (stat(path, &st) != 0)
            break;
    }
    if (cached < (int)ARRAY_SIZE(cache))
    {
        cache[cached].event = event;
        cache[cached].takes = n;
        cached++;
    }
    return n;
}

static void bark(const char* event, int pct)
{
    char path[512];
    int  takes;

    if (!Pc_Revenge_Active() || !g_PcConfig.revengeBarks || s_barkCooldown > 0 ||
        Pc_MsgVoice_Holding() || !chance(pct))
        return;

    takes = bark_takes(event);
    if (takes == 0)
        return;

    snprintf(path, sizeof(path), "%s/revenge/bark_%s_%d.wav", PcPort_GetGameDataPath(), event,
             (int)(rnd() % (u32)takes) + 1);
    if (XaPlayer_PlayFile(path))
        s_barkCooldown = REVENGE_BARK_COOLDOWN;
}

void Pc_Revenge_OnReload(void)
{
    bark("reload", 25);
}

void Pc_Revenge_OnMapLoad(void)
{
    s_areaBarkIn = REVENGE_AREA_DELAY;
}

int Pc_Revenge_Active(void)
{
    return g_PcConfig.revengeMode != 0;
}

static s32 scale(s32 amount, int pct)
{
    return (pct == 100) ? amount : (s32)(((s64)amount * pct) / 100);
}

s32 Pc_Revenge_ScaleWeaponDamage(s32 damageAmount, int targetIsPlayer)
{
    if (!Pc_Revenge_Active() || targetIsPlayer)
        return damageAmount;
    return scale(damageAmount, g_PcConfig.revengeWeaponDamagePct);
}

s32 Pc_Revenge_ScaleDamageTaken(s32 damageAmount)
{
    if (!Pc_Revenge_Active())
        return damageAmount;
    return scale(damageAmount, g_PcConfig.revengeDamageTakenPct);
}

void Pc_Revenge_OnDamageTaken(void)
{
    s_sinceHit = 0;
    if (g_SysWork.playerWork.player.health > Q12(0.0f) &&
        g_SysWork.playerWork.player.health < Q12(30.0f))
        bark("hurt", 70);
}

void Pc_Revenge_Update(void)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    q19_12          cap;

    if (!Pc_Revenge_Active() || g_GameWork.gameState != GameState_InGame)
        return;

    s_rng += (u32)g_DeltaTimeRaw;

    if (s_barkCooldown > 0)
        s_barkCooldown -= g_DeltaTimeRaw;
    if (s_areaBarkIn >= 0)
    {
        s_areaBarkIn -= g_DeltaTimeRaw;
        if (s_areaBarkIn < 0)
            bark("area", 40);
    }

    if (g_PcConfig.revengeRegenCap <= 0 || player->health <= Q12(0.0f))
        return;

    if (s_sinceHit < REVENGE_REGEN_DELAY)
    {
        s_sinceHit += g_DeltaTimeRaw;
        return;
    }

    cap = (Q12(100.0f) / 100) * g_PcConfig.revengeRegenCap;
    if (player->health < cap)
    {
        player->health += (q19_12)(((s64)REVENGE_REGEN_RATE * g_DeltaTimeRaw) >> 12);
        if (player->health > cap)
            player->health = cap;
    }
}

static int is_cloneable(int charaId)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(CLONEABLE); i++)
        if (CLONEABLE[i] == charaId)
            return 1;
    return 0;
}

static int free_slot(void)
{
    int i, used = 0, slot = -1;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        if (g_SysWork.npcs[i].model.charaId != Chara_None || HAS_FLAG(&g_SysWork.npcFlags, i))
            used++;
        else if (slot < 0)
            slot = i;
    }
    return (used >= NPC_COUNT_MAX - REVENGE_SLOT_RESERVE) ? -1 : slot;
}

/* A walkable spot in a ring around the parent; overlapping a clone with its
 * parent would push both through walls on the first collision pass. */
static int clone_position(q19_12 px, q19_12 pz, int n, q19_12* outX, q19_12* outZ)
{
    static const q19_12 RADII[] = { Q12(2.0f), Q12(1.25f) };
    int r, a;

    for (r = 0; r < (int)ARRAY_SIZE(RADII); r++)
    {
        for (a = 0; a < 8; a++)
        {
            /* Spread successive clones around the ring rather than stacking them. */
            q19_12 angle = (q19_12)(((a * 3 + n * 5) & 7) * (Q12(1.0f) / 8));
            q19_12 x     = px + (q19_12)(((s64)RADII[r] * Math_Cos(angle)) >> 12);
            q19_12 z     = pz + (q19_12)(((s64)RADII[r] * Math_Sin(angle)) >> 12);

            if (func_800808AC(x, z) != 0)
            {
                *outX = x;
                *outZ = z;
                return 1;
            }
        }
    }
    return 0;
}

static void pick_type(int parentId, int parentStep, int* charaId, int* stateStep)
{
    int i, n = 0;
    const s_Invader* options[ARRAY_SIZE(INVADERS)];

    *charaId   = parentId;
    *stateStep = parentStep;

    /* Invaders need every monster's model and AI resident, which only the
     * global chara pool provides. */
    if (!g_PcConfig.globalCharaPool || !chance(g_PcConfig.revengeInvaderPct))
        return;

    for (i = 0; i < (int)ARRAY_SIZE(INVADERS); i++)
    {
        if (INVADERS[i].charaId != parentId &&
            g_MapOverlayHdr.charaUpdateFuncs[INVADERS[i].charaId] != NULL)
            options[n++] = &INVADERS[i];
    }
    if (n > 0)
    {
        const s_Invader* pick = options[rnd() % (u32)n];
        *charaId   = pick->charaId;
        *stateStep = pick->stateStep;
    }
}

void Pc_Revenge_OnNativeSpawn(int row, int slot)
{
    s_SubCharacter*    parent;
    s_CollisionSurface surf;
    int                n;

    if (!Pc_Revenge_Active() || slot < 0 || slot >= NPC_COUNT_MAX)
        return;

    parent = &g_SysWork.npcs[slot];
    if (!is_cloneable(parent->model.charaId))
        return;

    for (n = 1; n < g_PcConfig.revengeEnemyMultiplier; n++)
    {
        s_SubCharacter* clone;
        q19_12          x, z;
        int             charaId, stateStep;
        int             c = free_slot();

        if (c < 0 || !clone_position(parent->position.vx, parent->position.vz, n, &x, &z))
            return;

        pick_type(parent->model.charaId, parent->model.stateStep, &charaId, &stateStep);

        clone = &g_SysWork.npcs[c];
        memset(clone, 0, sizeof(*clone));
        clone->model.charaId      = (s8)charaId;
        clone->model.controlState = 0;
        clone->model.stateStep    = (u8)stateStep;
        clone->field_40           = (s8)row;
        clone->position.vx        = x;
        clone->position.vz        = z;
        Collision_SurfaceGet(&surf, x, z);
        clone->position.vy        = surf.groundHeight;
        clone->rotation.vy        = (s16)(rnd() & 0xFFF);
        clone->model.anim.flags  |= AnimFlag_Visible;

        SET_FLAG(&g_SysWork.npcFlags, c);
        g_PcNpcDebugSpawned[c] = 1;
        s_isClone[c]           = 1;
        if (g_SysWork.npcFlagsId < (u8)(c + 1))
            g_SysWork.npcFlagsId = (u8)(c + 1);

        SH_DBG("[REVENGE] row %d -> clone npc[%d] charaId=%d%s", row, c, charaId,
               charaId != parent->model.charaId ? " (invader)" : "");
    }
}

void Pc_Revenge_SlotReset(int slot)
{
    if (slot >= 0 && slot < NPC_COUNT_MAX)
        s_isClone[slot] = 0;
}

int Pc_Revenge_IsClone(int slot)
{
    return slot >= 0 && slot < NPC_COUNT_MAX && s_isClone[slot];
}

static int has_item(u8 id)
{
    int i;
    for (i = 0; i < g_SavegamePtr->inventorySlotCount && i < INV_ITEM_COUNT_MAX; i++)
        if (g_SavegamePtr->items[i].id_0 == id)
            return 1;
    return 0;
}

void Pc_Revenge_OnEnemyKilled(void)
{
    int dropped = 0;

    if (!Pc_Revenge_Active())
        return;
    if (!g_PcConfig.revengeAmmoDrops)
    {
        bark("kill", 35);
        return;
    }

    /* Only ammo for guns Harry already carries: a new item type would take an
     * inventory slot the player never chose to fill. */
    if (has_item(InvItemId_Handgun) && chance(45))
    {
        Inventory_AddSpecialItem(InvItemId_HandgunBullets, (u8)(4 + rnd() % 4u));
        dropped = 1;
    }
    if (has_item(InvItemId_Shotgun) && chance(20))
    {
        Inventory_AddSpecialItem(InvItemId_ShotgunShells, 3);
        dropped = 1;
    }
    if (has_item(InvItemId_HuntingRifle) && chance(15))
    {
        Inventory_AddSpecialItem(InvItemId_RifleShells, 2);
        dropped = 1;
    }
    if (dropped)
    {
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 48);
        bark("ammo", 50);
    }
    bark("kill", 35);
}
