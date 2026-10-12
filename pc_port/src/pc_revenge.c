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
#include "bodyprog/anim.h"
#include "maps/characters/harry.h"
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

#include "dbg_overlay.h"
#include "pc_config.h"
#include "pc_merchant.h"
#include "pc_quick_options.h"
#include "pc_rage.h"
#include "pc_waves.h"
#include "pc_revenge.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

extern u8 g_PcNpcDebugSpawned[NPC_COUNT_MAX];
extern u8 g_Items_GunsMaxLoadAmmo[36];
extern s_AnimInfo HARRY_BASE_ANIM_INFOS[256];
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
#define REVENGE_BARK_TAKES    32
#define REVENGE_AREA_DELAY    Q12(2.5f) /* let the area fade in before Harry speaks */

static u8     s_isClone[NPC_COUNT_MAX];
static q19_12 s_barkCooldown = 0;
static q19_12 s_areaBarkIn   = -1;

static q19_12 s_time;
static q19_12 s_runFor;
static void   apply_capacity(void);

#define REVENGE_RUN_SPEED Q12(3.0f)  /* run speeds are 3.5-5.0; a walk is under 2 */
#define REVENGE_RUN_QUIP  Q12(14.0f) /* this long running without a break */
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
    static struct { char event[48]; int takes; } cache[64];
    static int cached = 0;
    char       path[512];
    struct stat st;
    int        i, n;

    for (i = 0; i < cached; i++)
        if (strcmp(cache[i].event, event) == 0)
            return cache[i].takes;

    for (n = 0; n < REVENGE_BARK_TAKES; n++)
    {
        snprintf(path, sizeof(path), "%s/revenge/bark_%s_%d.wav", PcPort_GetGameDataPath(), event, n + 1);
        if (stat(path, &st) != 0)
            break;
    }
    if (cached < (int)ARRAY_SIZE(cache))
    {
        snprintf(cache[cached].event, sizeof(cache[cached].event), "%s", event);
        cache[cached].takes = n;
        cached++;
    }
    return n;
}

static void play_line(const char* event, int force)
{
    char path[512];
    int  takes;

    if (!Pc_Revenge_Active() || !g_PcConfig.revengeBarks || Pc_MsgVoice_Holding() ||
        (!force && s_barkCooldown > 0))
        return;
    /* Only in free play, and never while the game's own voice task is busy:
     * a scripted scene waits on that task, and a file played into the middle
     * of it changes when the scene sees its line finish. The merchant panel
     * pauses play but is not scripted, so its lines are allowed through. */
    if (Sd_AudioStreamingCheck() ||
        (g_SysWork.sysFlags & (SysFlag_CutsceneActive | SysFlag_DemoActive)) ||
        (g_SysWork.sysState != SysState_Gameplay && !Pc_QuickOptions_IsOpen()))
        return;

    {
        /* The salty takes (bark_<event>_salty_<n>) join the pool when swearing is on. */
        char salty[48];
        int  clean, dirty, pick;

        snprintf(salty, sizeof(salty), "%s_salty", event);
        clean = bark_takes(event);
        dirty = g_PcConfig.revengeSwearing ? bark_takes(salty) : 0;
        takes = clean + dirty;
        if (takes == 0)
            return;

        pick = (int)(rnd() % (u32)takes);
        if (pick < clean)
            snprintf(path, sizeof(path), "%s/revenge/bark_%s_%d.wav", PcPort_GetGameDataPath(), event, pick + 1);
        else
            snprintf(path, sizeof(path), "%s/revenge/bark_%s_%d.wav", PcPort_GetGameDataPath(), salty,
                     pick - clean + 1);
    }
    if (XaPlayer_PlayFile(path))
        s_barkCooldown = REVENGE_BARK_COOLDOWN;
}

static void bark(const char* event, int pct)
{
    if (chance(pct))
        play_line(event, 0);
}

void Pc_Revenge_Bark(const char* event, int pct)
{
    bark(event, pct);
}

/* Merchant lines answer the player directly, so no cooldown or dice roll. */
void Pc_Revenge_Voice(const char* event)
{
    play_line(event, 1);
}

u32 Pc_Revenge_Rand(void)
{
    return rnd();
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
    if (Pc_Rage_Active())
        return 0;
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

    s_rng  += (u32)g_DeltaTimeRaw;
    s_time += g_DeltaTimeRaw;
    apply_capacity();

    /* A long run earns Harry a complaint about his cardio. */
    if (g_SysWork.sysState == SysState_Gameplay &&
        g_SysWork.playerWork.player.moveSpeed >= REVENGE_RUN_SPEED)
    {
        s_runFor += g_DeltaTimeRaw;
        if (s_runFor >= REVENGE_RUN_QUIP)
        {
            s_runFor = 0;
            bark("running", 80);
        }
    }
    else if (g_SysWork.sysState == SysState_Gameplay)
        s_runFor = 0;
    Pc_Rage_Update();

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

/* An extra monster in npc slot `c`. It borrows native row `row`'s field_40
 * (field_40 indexes the 32-bit row masks, so it must stay below 32) and is
 * flagged as a debug spawn, so killing it never writes the save's kill bits. */
static void spawn_extra(int c, int charaId, int stateStep, int row, q19_12 x, q19_12 z)
{
    s_SubCharacter*    npc = &g_SysWork.npcs[c];
    s_CollisionSurface surf;

    memset(npc, 0, sizeof(*npc));
    npc->model.charaId      = (s8)charaId;
    npc->model.controlState = 0;
    npc->model.stateStep    = (u8)stateStep;
    npc->field_40           = (s8)row;
    npc->position.vx        = x;
    npc->position.vz        = z;
    Collision_SurfaceGet(&surf, x, z);
    npc->position.vy        = surf.groundHeight;
    npc->rotation.vy        = (s16)(rnd() & 0xFFF);
    npc->model.anim.flags  |= AnimFlag_Visible;

    SET_FLAG(&g_SysWork.npcFlags, c);
    g_PcNpcDebugSpawned[c] = 1;
    s_isClone[c]           = 1;
    if (g_SysWork.npcFlagsId < (u8)(c + 1))
        g_SysWork.npcFlagsId = (u8)(c + 1);
}

void Pc_Revenge_OnNativeSpawn(int row, int slot)
{
    s_SubCharacter*    parent;
    int                n;

    if (!Pc_Revenge_Active() || slot < 0 || slot >= NPC_COUNT_MAX)
        return;

    parent = &g_SysWork.npcs[slot];
    if (!is_cloneable(parent->model.charaId))
        return;

    for (n = 1; n < g_PcConfig.revengeEnemyMultiplier; n++)
    {
        q19_12          x, z;
        int             charaId, stateStep;
        int             c = free_slot();

        if (c < 0 || !clone_position(parent->position.vx, parent->position.vz, n, &x, &z))
            return;

        pick_type(parent->model.charaId, parent->model.stateStep, &charaId, &stateStep);
        spawn_extra(c, charaId, stateStep, row, x, z);

        SH_DBG("[REVENGE] row %d -> clone npc[%d] charaId=%d%s", row, c, charaId,
               charaId != parent->model.charaId ? " (invader)" : "");
        if (charaId != parent->model.charaId)
            bark("invader", 60);
    }
}

/* ------------------------------------------------------------------ waves */

/* Wave monsters (pc_waves.c) appear out of the fog between these distances. */
#define WAVE_NEAR Q12(13.0f)
#define WAVE_FAR  Q12(20.0f)

static u8 s_isWave[NPC_COUNT_MAX];
static u8 s_waveKilled[NPC_COUNT_MAX];

int Pc_Revenge_MonstersWithin(q19_12 range)
{
    VECTOR3* p = &g_SysWork.playerWork.player.position;
    int      i, n = 0;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        s64             dx, dz;

        if (npc->model.charaId < Chara_AirScreamer || npc->model.charaId > Chara_MonsterCybil ||
            npc->health <= Q12(0.0f))
            continue;
        dx = (s64)npc->position.vx - p->vx;
        dz = (s64)npc->position.vz - p->vz;
        if (dx * dx + dz * dz < (s64)range * range)
            n++;
    }
    return n;
}

/* Spawn rows the map has live right now: a row of a regular monster whose kill
 * bit is still set is the map saying "monsters belong here now", so cleared,
 * safe and scripted areas stay as the story left them. */
static int live_rows(int* rows)
{
    s32 states = g_SavegamePtr->ovlEnemyStates[g_SavegamePtr->mapIdx];
    int i, n = 0;

    if (g_SysWork.sysFlags & SysFlag_NoEnemySpawn)
        return 0;
    for (i = 0; i < 32; i++)
    {
        s_SpawnInfo* row = &g_MapOverlayHdr.charaSpawnInfos[i / 16][i % 16];
        int          id  = row->charaId > Chara_None ? row->charaId : g_MapOverlayHdr.charaGroupIds[i / 16];

        if (row->flags == 0 || !(states & (1 << i)) || !is_cloneable(id) ||
            g_SavegamePtr->gameDifficulty < row->gameDifficultyMin)
            continue;
        rows[n++] = i;
    }
    return n;
}

int Pc_Revenge_AreaHasMonsters(void)
{
    int rows[32];
    return Pc_Revenge_Active() && live_rows(rows) > 0;
}

int Pc_Revenge_SpawnWaveMonster(void)
{
    VECTOR3* p = &g_SysWork.playerWork.player.position;
    int      rows[32], nRows, i, c, pick, charaId, stateStep;

    nRows = live_rows(rows);
    if (nRows == 0)
        return -1;
    c = free_slot();
    if (c < 0)
        return -1;

    pick = rows[rnd() % (u32)nRows];
    {
        s_SpawnInfo* row = &g_MapOverlayHdr.charaSpawnInfos[pick / 16][pick % 16];
        int          id  = row->charaId > Chara_None ? row->charaId : g_MapOverlayHdr.charaGroupIds[pick / 16];
        pick_type(id, row->flags, &charaId, &stateStep);
    }

    for (i = 0; i < 12; i++)
    {
        q19_12 angle = (q19_12)(rnd() & 0xFFF);
        q19_12 dist  = WAVE_NEAR + (q19_12)(rnd() % (u32)(WAVE_FAR - WAVE_NEAR));
        q19_12 x     = p->vx + (q19_12)(((s64)dist * Math_Cos(angle)) >> 12);
        q19_12 z     = p->vz + (q19_12)(((s64)dist * Math_Sin(angle)) >> 12);

        if (func_800808AC(x, z) != 0)
        {
            spawn_extra(c, charaId, stateStep, pick, x, z);
            s_isWave[c]     = 1;
            s_waveKilled[c] = 0;
            SH_DBG("[WAVE] npc[%d] charaId=%d at %d units", c, charaId, dist >> 12);
            return c;
        }
    }
    return -1;
}

int Pc_Revenge_IsWaveMonster(int slot)
{
    return slot >= 0 && slot < NPC_COUNT_MAX && s_isWave[slot] &&
           g_SysWork.npcs[slot].model.charaId != Chara_None;
}

int Pc_Revenge_WaveMonstersAlive(void)
{
    int i, n = 0;
    for (i = 0; i < NPC_COUNT_MAX; i++)
        if (Pc_Revenge_IsWaveMonster(i) && g_SysWork.npcs[i].health > Q12(0.0f))
            n++;
    return n;
}

void Pc_Revenge_ForgetWave(void)
{
    memset(s_isWave, 0, sizeof(s_isWave));
    memset(s_waveKilled, 0, sizeof(s_waveKilled));
}

void Pc_Revenge_SlotReset(int slot)
{
    if (slot < 0 || slot >= NPC_COUNT_MAX)
        return;
    /* A wave monster freed without dying (it wandered past the despawn range)
     * goes back into the wave's quota, or the wave could end with it unkilled. */
    if (s_isWave[slot] && !s_waveKilled[slot])
        Pc_Waves_OnMonsterLost();
    s_isClone[slot]    = 0;
    s_isWave[slot]     = 0;
    s_waveKilled[slot] = 0;
}

void Pc_Revenge_OnKillSlot(int slot)
{
    if (slot < 0 || slot >= NPC_COUNT_MAX)
        return;
    if (s_isWave[slot] && !s_waveKilled[slot])
    {
        s_waveKilled[slot] = 1;
        Pc_Waves_OnKill();
    }
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
        Pc_Revenge_GiveItem(InvItemId_HandgunBullets, (u8)(4 + rnd() % 4u));
        dropped = 1;
    }
    if (has_item(InvItemId_Shotgun) && chance(20))
    {
        Pc_Revenge_GiveItem(InvItemId_ShotgunShells, 3);
        dropped = 1;
    }
    if (has_item(InvItemId_HuntingRifle) && chance(15))
    {
        Pc_Revenge_GiveItem(InvItemId_RifleShells, 2);
        dropped = 1;
    }
    if (dropped)
    {
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 48);
        bark("ammo", 50);
    }
    bark("money", 12);
    bark("kill", 35);
}

/* ------------------------------------------------------------------ combat */

#define REVENGE_KICK_WINDOW Q12(1.5f) /* how long a staggered enemy stays kickable */

static q19_12 s_kickableUntil[NPC_COUNT_MAX];
static q19_12 s_lastStaggerAt[NPC_COUNT_MAX];

static int merchant_weapon(int weaponAttack)
{
    switch (weaponAttack)
    {
        case WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap):      return MERCHANT_WPN_HANDGUN;
        case WEAPON_ATTACK(EquippedWeaponId_Shotgun, AttackInputType_Tap):      return MERCHANT_WPN_SHOTGUN;
        case WEAPON_ATTACK(EquippedWeaponId_HuntingRifle, AttackInputType_Tap): return MERCHANT_WPN_RIFLE;
    }
    /* Packed melee attacks are 0..29 (id + input * 10); the kick and stomp are
     * Harry's own, not the weapon he holds. */
    if (weaponAttack >= 0 && weaponAttack < 30 &&
        WEAPON_ATTACK_ID_GET(weaponAttack) != EquippedWeaponId_Kick &&
        WEAPON_ATTACK_ID_GET(weaponAttack) != EquippedWeaponId_Stomp)
        return MERCHANT_WPN_MELEE;
    return -1;
}

/* Push the enemy past its own flinch threshold rather than setting its state:
 * each AI's states assume their own animations and keyframes. */
static void force_stagger(s_SubCharacter* npc)
{
    switch (npc->model.charaId)
    {
        case Chara_Stalker:
        case Chara_GreyChild:
        case Chara_Mumbler:
            npc->properties.stalker.timer_10C += Q12(200.0f);
            break;
        case Chara_PuppetNurse:
        case Chara_PuppetDoctor:
            npc->properties.puppetNurse.field_114 += Q12(1000.0f);
            break;
        case Chara_Groaner:
        case Chara_Wormhead:
            /* A charging dog ignores hits; let this one stop it. */
            npc->properties.groaner.flags.val16[0] &= ~(1 << 10);
            break;
        default:
            /* Romper, Creeper, Hanged Scratcher and the fliers react to any
             * solid hit already. */
            break;
    }
}

void Pc_Revenge_OnHit(struct _SubCharacter* attackerV, struct _SubCharacter* targetV, int weaponAttack,
                      const void* hitPosV, s32* damage, u32* knockback)
{
    s_SubCharacter* attacker = (s_SubCharacter*)attackerV;
    s_SubCharacter* target   = (s_SubCharacter*)targetV;
    const VECTOR3*  hit      = (const VECTOR3*)hitPosV;
    int             slot, weapon, stagger = 0, kick;

    if (!Pc_Revenge_Active() || attacker != &g_SysWork.playerWork.player)
        return;
    slot = (int)(target - g_SysWork.npcs);
    if (slot < 0 || slot >= NPC_COUNT_MAX)
        return;

    weapon = merchant_weapon(weaponAttack);
    kick   = weaponAttack == WEAPON_ATTACK(EquippedWeaponId_Kick, AttackInputType_Tap);

    if (weapon >= 0)
        *damage = scale(*damage, 100 + 20 * Pc_Merchant_Level(weapon, MERCHANT_STAT_DAMAGE));
    *damage = scale(*damage, Pc_Rage_DamagePct());

    if (weapon == MERCHANT_WPN_HANDGUN || weapon == MERCHANT_WPN_SHOTGUN || weapon == MERCHANT_WPN_RIFLE)
    {
        /* Free aim has no body parts, only where the bullet trace met the
         * enemy's cylinder. Y grows downwards and box.top is the head offset. */
        q19_12 feet   = target->position.vy;
        q19_12 height = -(q19_12)target->collision.box.top;

        if (hit != NULL && height > Q12(0.4f))
        {
            q19_12 up   = feet - hit->vy;
            int    frac = (int)(((s64)up * 100) / height);

            if (frac >= 78)
            {
                *damage = scale(*damage, 200);
                stagger = 1;
                DbgOverlay_ToastLine("HEADSHOT");
                Pc_Rage_OnHeadshot();
                Pc_Waves_OnHeadshot();
            }
            else if (frac <= 30)
                stagger = 1;
        }
        /* The heavy guns stagger wherever they land. */
        if (weapon != MERCHANT_WPN_HANDGUN)
            stagger = 1;
    }
    else if (kick || weapon == MERCHANT_WPN_MELEE)
    {
        stagger = 1;
        if (*knockback != 0)
            *knockback *= 2;
    }

    /* Every shotgun pellet is its own call in the same frame. */
    if (stagger && s_lastStaggerAt[slot] != s_time)
    {
        s_lastStaggerAt[slot] = s_time;
        force_stagger(target);
        /* A kick lands on a staggered enemy; it must not open a new window, or
         * one stagger would chain kicks forever. */
        if (!kick)
            s_kickableUntil[slot] = s_time + REVENGE_KICK_WINDOW;
    }
}

int Pc_Revenge_Kickable(int slot)
{
    return Pc_Revenge_Active() && slot >= 0 && slot < NPC_COUNT_MAX &&
           s_time < s_kickableUntil[slot] && g_SysWork.npcs[slot].health > Q12(0.0f);
}

/* Clip sizes live in one table every reader indexes by weapon id, so writing
 * it is all an upgrade needs. The HUD draws two digits: stay below 100. */
static void apply_capacity(void)
{
    int on = Pc_Revenge_Active() && g_SavegamePtr != NULL;

    g_Items_GunsMaxLoadAmmo[EquippedWeaponId_Handgun] =
        (u8)(15 + (on ? 3 * Pc_Merchant_Level(MERCHANT_WPN_HANDGUN, MERCHANT_STAT_CAPACITY) : 0));
    g_Items_GunsMaxLoadAmmo[EquippedWeaponId_Shotgun] =
        (u8)(6 + (on ? 2 * Pc_Merchant_Level(MERCHANT_WPN_SHOTGUN, MERCHANT_STAT_CAPACITY) : 0));
    g_Items_GunsMaxLoadAmmo[EquippedWeaponId_HuntingRifle] =
        (u8)(6 + (on ? 2 * Pc_Merchant_Level(MERCHANT_WPN_RIFLE, MERCHANT_STAT_CAPACITY) : 0));
}

/* The reload is HARRY_BASE_ANIM_INFOS[63], re-copied from the weapon's table
 * on every equip. Its keyframe rate scales with the upgrade; reload completion
 * and its sound are keyed to keyframes, so they stay in step. */
static q19_12 s_reloadBase;
static int    s_reloadWeapon = -1;

static void apply_reload_speed(void)
{
    s_AnimInfo* reload = &HARRY_BASE_ANIM_INFOS[ANIM_STATUS(HarryAnim_HandgunRecoil, true)];
    int         lv;

    if (s_reloadWeapon < 0 || reload->hasVariableDuration)
        return;
    lv = Pc_Revenge_Active() ? Pc_Merchant_Level(s_reloadWeapon, MERCHANT_STAT_RELOAD) : 0;
    reload->duration.constant = scale(s_reloadBase, 100 + 15 * lv);
}

void Pc_Revenge_OnWeaponAnimsLoaded(void)
{
    s_reloadWeapon = merchant_weapon(g_SysWork.playerCombat.weaponAttack);
    if (s_reloadWeapon == MERCHANT_WPN_MELEE)
        s_reloadWeapon = -1;
    s_reloadBase = HARRY_BASE_ANIM_INFOS[ANIM_STATUS(HarryAnim_HandgunRecoil, true)].duration.constant;
    apply_reload_speed();
}

void Pc_Revenge_ApplyUpgrades(void)
{
    apply_capacity();
    apply_reload_speed();
}
