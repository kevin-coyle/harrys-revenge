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
#include "pc_rage.h"
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
#define REVENGE_BARK_TAKES    16
#define REVENGE_AREA_DELAY    Q12(2.5f) /* let the area fade in before Harry speaks */

static u8     s_isClone[NPC_COUNT_MAX];
static q19_12 s_barkCooldown = 0;
static q19_12 s_areaBarkIn   = -1;

static q19_12 s_time;
static void   apply_capacity(void);
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
    static struct { char event[48]; int takes; } cache[32];
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
        if (charaId != parent->model.charaId)
            bark("invader", 60);
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
