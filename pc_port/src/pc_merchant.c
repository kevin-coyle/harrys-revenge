/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_merchant.c - Harry's Revenge money, shop and weapon upgrades (see pc_merchant.h).
 *
 * Savegame storage, all in bytes the original game never reads:
 *   0xA0..0xA3 (field_A0 + unused_A1[3])  money, little-endian u32
 *   ovlEnemyStates[43], [44]              upgrade levels, one nibble each
 *                                         (the two @unused test maps' slots);
 *                                         bit 31 of [44] is the scooter
 *   0x23E (unused_23E)                    MERCHANT_SAVE_MAGIC once initialised
 * A save from vanilla or an older build has no magic there, so it starts with
 * no money and no upgrades. New Game sets ovlEnemyStates[] to all ones and
 * zeroes the rest, which the same magic test catches.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/items.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/math/math.h"
#include "bodyprog/savegame.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"

#include "dbg_overlay.h"
#include "pc_merchant.h"
#include "pc_revenge.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>

#define MERCHANT_SAVE_MAGIC 0x52 /* 'R' */
#define MERCHANT_MONEY_MAX  999999
#define MERCHANT_SAFE_RANGE Q12(15.0f) /* the game's own "nearest enemy" camera radius */

extern void Pc_QuickOptions_OpenMerchant(void);

/* ------------------------------------------------------------------ save data */

static void save_init(void)
{
    if (g_SavegamePtr == NULL || (u8)g_SavegamePtr->unused_23E == MERCHANT_SAVE_MAGIC)
        return;
    memset(&g_SavegamePtr->field_A0, 0, 4);
    g_SavegamePtr->ovlEnemyStates[43] = 0;
    g_SavegamePtr->ovlEnemyStates[44] = 0;
    g_SavegamePtr->unused_23E         = (s8)MERCHANT_SAVE_MAGIC;
}

int Pc_Merchant_Money(void)
{
    u8* b;
    save_init();
    if (g_SavegamePtr == NULL)
        return 0;
    b = (u8*)&g_SavegamePtr->field_A0;
    return (int)(b[0] | (b[1] << 8) | (b[2] << 16) | ((u32)b[3] << 24));
}

static void set_money(int v)
{
    u8* b;
    save_init();
    if (g_SavegamePtr == NULL)
        return;
    if (v < 0) v = 0;
    if (v > MERCHANT_MONEY_MAX) v = MERCHANT_MONEY_MAX;
    b    = (u8*)&g_SavegamePtr->field_A0;
    b[0] = (u8)v;
    b[1] = (u8)(v >> 8);
    b[2] = (u8)(v >> 16);
    b[3] = (u8)(v >> 24);
}

void Pc_Merchant_AddMoney(int amount)
{
    set_money(Pc_Merchant_Money() + amount);
}

int Pc_Merchant_Level(int weapon, int stat)
{
    int nib = weapon * MERCHANT_STAT_COUNT + stat;
    u32 word;

    save_init();
    if (g_SavegamePtr == NULL || weapon < 0 || weapon >= MERCHANT_WPN_COUNT ||
        stat < 0 || stat >= MERCHANT_STAT_COUNT)
        return 0;
    word = (u32)g_SavegamePtr->ovlEnemyStates[43 + nib / 8];
    return (int)((word >> ((nib % 8) * 4)) & 0xF);
}

static void set_level(int weapon, int stat, int level)
{
    int nib = weapon * MERCHANT_STAT_COUNT + stat;
    u32 word, shift = (u32)((nib % 8) * 4);

    save_init();
    word = (u32)g_SavegamePtr->ovlEnemyStates[43 + nib / 8];
    word = (word & ~(0xFu << shift)) | ((u32)(level & 0xF) << shift);
    g_SavegamePtr->ovlEnemyStates[43 + nib / 8] = (s32)word;
}

#define MERCHANT_SCOOTER_BIT (1u << 31)

int Pc_Merchant_HasScooter(void)
{
    save_init();
    return g_SavegamePtr != NULL && ((u32)g_SavegamePtr->ovlEnemyStates[44] & MERCHANT_SCOOTER_BIT) != 0;
}

/* ------------------------------------------------------------------ inventory */

static int has_item(u8 id)
{
    int i;
    for (i = 0; i < g_SavegamePtr->inventorySlotCount && i < INV_ITEM_COUNT_MAX; i++)
        if (g_SavegamePtr->items[i].id_0 == id)
            return 1;
    return 0;
}

/* Inventory_AddSpecialItem appends a slot (the re-sort merges ammo and health
 * into an existing stack) without a bounds check, and counts a pickup for the
 * end-of-game ranking. A purchase or drop is not a pickup the player found, so
 * both counters are put back: the special count is a 3-bit field that carries
 * into the HyperBlaster's colour bits. Returns 0 when the inventory is full. */
static int give_item(u8 id, u8 count)
{
    u8 picked, special;

    if (g_SavegamePtr->inventorySlotCount >= INV_ITEM_COUNT_MAX)
        return 0;
    picked  = g_SavegamePtr->pickedUpItemCount;
    special = g_SavegamePtr->pickedUpSpecialItemCount;
    Pc_Revenge_QuietItems(1);
    Inventory_AddSpecialItem(id, count);
    Pc_Revenge_QuietItems(0);
    g_SavegamePtr->pickedUpItemCount        = picked;
    g_SavegamePtr->pickedUpSpecialItemCount = special;
    return 1;
}

void Pc_Revenge_GiveItem(u8 id, u8 count)
{
    give_item(id, count);
}

/* ------------------------------------------------------------------ rewards */

static int kill_reward(int charaId)
{
    switch (charaId)
    {
        case Chara_Creeper:        return 30;
        case Chara_LarvalStalker:  return 20;
        case Chara_GreyChild:
        case Chara_Mumbler:        return 50;
        case Chara_Groaner:
        case Chara_Wormhead:       return 60;
        case Chara_Stalker:        return 70;
        case Chara_AirScreamer:
        case Chara_NightFlutter:   return 80;
        case Chara_PuppetNurse:
        case Chara_PuppetDoctor:   return 120;
        case Chara_Romper:         return 140;
        case Chara_HangedScratcher: return 150;
        case Chara_SplitHead:
        case Chara_Twinfeeler:
        case Chara_Floatstinger:
        case Chara_Bloodsucker:
        case Chara_Incubus:
        case Chara_MonsterCybil:   return 1500;
        default:                   return 80;
    }
}

void Pc_Merchant_OnKill(int charaId)
{
    char line[64];
    int  base = kill_reward(charaId);
    int  amount;

    if (!Pc_Revenge_Active())
        return;

    /* +/- 25% so a run of the same monster does not pay a metronome. */
    amount = base * 3 / 4 + (int)(Pc_Revenge_Rand() % (u32)(base / 2 + 1));
    Pc_Merchant_AddMoney(amount);
    snprintf(line, sizeof(line), "+$%d    $%d", amount, Pc_Merchant_Money());
    DbgOverlay_ToastLine(line);
}

/* ------------------------------------------------------------------ shop */

enum
{
    ITEM_HANDGUN_AMMO = 0, ITEM_SHOTGUN_AMMO, ITEM_RIFLE_AMMO,
    ITEM_HEALTH_DRINK, ITEM_FIRST_AID, ITEM_AMPOULE,
    ITEM_SCOOTER,
    ITEM_WEAPON_FIRST, /* + index into WEAPONS_FOR_SALE */
    ITEM_UPGRADE_FIRST = ITEM_WEAPON_FIRST + 7 /* + weapon * MERCHANT_STAT_COUNT + stat */
};

/* The late-game and unlockable weapons, for sale from the start. Each also sets
 * its world pickup's flag, so the map copy is gone rather than a duplicate. */
static const struct { u8 item; int price; u16 pickupFlag; const char* name; } WEAPONS_FOR_SALE[] = {
    { InvItemId_SteelPipe,    300,  EventFlag_M2S00_PickupSteelPipe, "Steel pipe"   },
    { InvItemId_Hammer,       800,  EventFlag_M3S05_PickupHammer,    "Hammer"       },
    { InvItemId_Axe,          1200, EventFlag_M4S01_PickupAxe,       "Axe"          },
    { InvItemId_Katana,       2500, EventFlag_M0S02_PickupKatana,    "Katana"       },
    { InvItemId_RockDrill,    3500, EventFlag_M2S00_PickupRockDrill, "Rock drill"   },
    { InvItemId_Chainsaw,     4000, EventFlag_M2S00_PickupChainsaw,  "Chainsaw"     },
    { InvItemId_HyperBlaster, 9999, 0,                               "HyperBlaster" },
};

static const char* const WEAPON_NAMES[MERCHANT_WPN_COUNT] = { "Handgun", "Shotgun", "Rifle", "Melee" };
static const char* const STAT_NAMES[MERCHANT_STAT_COUNT]  = { "damage", "clip size", "reload speed" };

/* Base price per level; level n costs base * (n + 1). Melee only has damage. */
static const int UPGRADE_BASE[MERCHANT_WPN_COUNT][MERCHANT_STAT_COUNT] = {
    { 300, 250, 350 },
    { 500, 400, 450 },
    { 600, 450, 450 },
    { 250,   0,   0 },
};

static const u8 WEAPON_ITEM[MERCHANT_WPN_COUNT] = {
    InvItemId_Handgun, InvItemId_Shotgun, InvItemId_HuntingRifle, 0
};

static const int PAGE_SUPPLIES[] = { ITEM_HANDGUN_AMMO, ITEM_SHOTGUN_AMMO, ITEM_RIFLE_AMMO,
                                     ITEM_HEALTH_DRINK, ITEM_FIRST_AID, ITEM_AMPOULE, ITEM_SCOOTER };
static const int PAGE_WEAPONS[]  = { ITEM_WEAPON_FIRST + 0, ITEM_WEAPON_FIRST + 1, ITEM_WEAPON_FIRST + 2,
                                     ITEM_WEAPON_FIRST + 3, ITEM_WEAPON_FIRST + 4, ITEM_WEAPON_FIRST + 5,
                                     ITEM_WEAPON_FIRST + 6 };
#define UPG(w, s) (ITEM_UPGRADE_FIRST + (w) * MERCHANT_STAT_COUNT + (s))
static const int PAGE_GUNS[] = {
    UPG(MERCHANT_WPN_HANDGUN, MERCHANT_STAT_DAMAGE), UPG(MERCHANT_WPN_HANDGUN, MERCHANT_STAT_CAPACITY),
    UPG(MERCHANT_WPN_HANDGUN, MERCHANT_STAT_RELOAD),
    UPG(MERCHANT_WPN_SHOTGUN, MERCHANT_STAT_DAMAGE), UPG(MERCHANT_WPN_SHOTGUN, MERCHANT_STAT_CAPACITY),
    UPG(MERCHANT_WPN_SHOTGUN, MERCHANT_STAT_RELOAD),
};
static const int PAGE_MORE[] = {
    UPG(MERCHANT_WPN_RIFLE, MERCHANT_STAT_DAMAGE), UPG(MERCHANT_WPN_RIFLE, MERCHANT_STAT_CAPACITY),
    UPG(MERCHANT_WPN_RIFLE, MERCHANT_STAT_RELOAD),
    UPG(MERCHANT_WPN_MELEE, MERCHANT_STAT_DAMAGE),
};

int Pc_Merchant_PageCount(int page)
{
    switch (page)
    {
        case 0:  return (int)ARRAY_SIZE(PAGE_SUPPLIES);
        case 1:  return (int)ARRAY_SIZE(PAGE_WEAPONS);
        case 2:  return (int)ARRAY_SIZE(PAGE_GUNS);
        default: return (int)ARRAY_SIZE(PAGE_MORE);
    }
}

int Pc_Merchant_PageItem(int page, int row)
{
    switch (page)
    {
        case 0:  return PAGE_SUPPLIES[row];
        case 1:  return PAGE_WEAPONS[row];
        case 2:  return PAGE_GUNS[row];
        default: return PAGE_MORE[row];
    }
}

const char* Pc_Merchant_PageName(int page)
{
    static const char* const NAMES[MERCHANT_PAGES] = {
        "Merchant: Supplies", "Merchant: Weapons", "Merchant: Handgun & Shotgun", "Merchant: Rifle & Melee" };
    return NAMES[(page < 0 || page >= MERCHANT_PAGES) ? 0 : page];
}

static int item_price(int item)
{
    switch (item)
    {
        case ITEM_HANDGUN_AMMO: return 150;
        case ITEM_SHOTGUN_AMMO: return 250;
        case ITEM_RIFLE_AMMO:   return 300;
        case ITEM_HEALTH_DRINK: return 200;
        case ITEM_FIRST_AID:    return 450;
        case ITEM_AMPOULE:      return 1000;
        case ITEM_SCOOTER:      return 1500;
        default:
        {
            if (item < ITEM_UPGRADE_FIRST)
                return WEAPONS_FOR_SALE[item - ITEM_WEAPON_FIRST].price;
            int u = item - ITEM_UPGRADE_FIRST;
            int w = u / MERCHANT_STAT_COUNT, s = u % MERCHANT_STAT_COUNT;
            return UPGRADE_BASE[w][s] * (Pc_Merchant_Level(w, s) + 1);
        }
    }
}

const char* Pc_Merchant_ItemName(int item)
{
    static char buf[48];
    switch (item)
    {
        case ITEM_HANDGUN_AMMO: return "Handgun bullets x15";
        case ITEM_SHOTGUN_AMMO: return "Shotgun shells x10";
        case ITEM_RIFLE_AMMO:   return "Rifle shells x6";
        case ITEM_HEALTH_DRINK: return "Health drink";
        case ITEM_FIRST_AID:    return "First aid kit";
        case ITEM_AMPOULE:      return "Ampoule";
        case ITEM_SCOOTER:      return "Scooter";
        default:
        {
            if (item < ITEM_UPGRADE_FIRST)
                return WEAPONS_FOR_SALE[item - ITEM_WEAPON_FIRST].name;
            int u = item - ITEM_UPGRADE_FIRST;
            snprintf(buf, sizeof(buf), "%s %s", WEAPON_NAMES[u / MERCHANT_STAT_COUNT],
                     STAT_NAMES[u % MERCHANT_STAT_COUNT]);
            return buf;
        }
    }
}

/* Why an item cannot be bought right now, or NULL. */
static const char* unavailable(int item)
{
    switch (item)
    {
        case ITEM_HANDGUN_AMMO: return has_item(InvItemId_Handgun) ? NULL : "Need handgun";
        case ITEM_SHOTGUN_AMMO: return has_item(InvItemId_Shotgun) ? NULL : "Need shotgun";
        case ITEM_RIFLE_AMMO:   return has_item(InvItemId_HuntingRifle) ? NULL : "Need rifle";
        case ITEM_HEALTH_DRINK:
        case ITEM_FIRST_AID:
        case ITEM_AMPOULE:      return NULL;
        case ITEM_SCOOTER:      return Pc_Merchant_HasScooter() ? "Owned" : NULL;
        default:
        {
            if (item < ITEM_UPGRADE_FIRST)
                return has_item(WEAPONS_FOR_SALE[item - ITEM_WEAPON_FIRST].item) ? "Owned" : NULL;
            int u = item - ITEM_UPGRADE_FIRST;
            int w = u / MERCHANT_STAT_COUNT, s = u % MERCHANT_STAT_COUNT;
            if (WEAPON_ITEM[w] != 0 && !has_item(WEAPON_ITEM[w]))
                return w == MERCHANT_WPN_RIFLE ? "Need rifle" : w == MERCHANT_WPN_SHOTGUN ? "Need shotgun" : "Need handgun";
            if (Pc_Merchant_Level(w, s) >= MERCHANT_LEVEL_MAX)
                return "MAX";
            return NULL;
        }
    }
}

const char* Pc_Merchant_ItemLabel(int item, char* buf, int n)
{
    const char* why = unavailable(item);

    if (item >= ITEM_UPGRADE_FIRST)
    {
        int u = item - ITEM_UPGRADE_FIRST;
        int lv = Pc_Merchant_Level(u / MERCHANT_STAT_COUNT, u % MERCHANT_STAT_COUNT);
        if (why)
            snprintf(buf, (size_t)n, "Lv %d/%d   %s", lv, MERCHANT_LEVEL_MAX, why);
        else
            snprintf(buf, (size_t)n, "Lv %d/%d   $%d", lv, MERCHANT_LEVEL_MAX, item_price(item));
    }
    else if (why)
        snprintf(buf, (size_t)n, "%s", why);
    else
        snprintf(buf, (size_t)n, "$%d", item_price(item));
    return buf;
}

void Pc_Merchant_Buy(int item)
{
    int price = item_price(item);

    if (unavailable(item))
    {
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        return;
    }
    if (Pc_Merchant_Money() < price)
    {
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        Pc_Revenge_Voice("merchant_poor");
        return;
    }

    if (item == ITEM_SCOOTER)
    {
        set_money(Pc_Merchant_Money() - price);
        g_SavegamePtr->ovlEnemyStates[44] = (s32)((u32)g_SavegamePtr->ovlEnemyStates[44] | MERCHANT_SCOOTER_BIT);
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
        Pc_Revenge_Voice("merchant_scooter");
        DbgOverlay_ToastLine("Scooter: sprint in the open streets to ride.");
        return;
    }
    if (item < ITEM_UPGRADE_FIRST)
    {
        u8 id = 0, count = 1;
        switch (item)
        {
            case ITEM_HANDGUN_AMMO: id = InvItemId_HandgunBullets; count = 15; break;
            case ITEM_SHOTGUN_AMMO: id = InvItemId_ShotgunShells;  count = 10; break;
            case ITEM_RIFLE_AMMO:   id = InvItemId_RifleShells;    count = 6;  break;
            case ITEM_HEALTH_DRINK: id = InvItemId_HealthDrink;    break;
            case ITEM_FIRST_AID:    id = InvItemId_FirstAidKit;    break;
            case ITEM_AMPOULE:      id = InvItemId_Ampoule;        break;
            default:                id = WEAPONS_FOR_SALE[item - ITEM_WEAPON_FIRST].item; break;
        }
        if (!give_item(id, count))
        {
            Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
            DbgOverlay_ToastLine("Inventory full.");
            return;
        }
        set_money(Pc_Merchant_Money() - price);
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
        if (item >= ITEM_WEAPON_FIRST)
        {
            if (WEAPONS_FOR_SALE[item - ITEM_WEAPON_FIRST].pickupFlag != 0)
                Savegame_EventFlagSet(WEAPONS_FOR_SALE[item - ITEM_WEAPON_FIRST].pickupFlag);
            Pc_Revenge_Voice("merchant_weapon");
        }
        else
            Pc_Revenge_Voice("merchant_buy");
        return;
    }

    set_money(Pc_Merchant_Money() - price);
    switch (item)
    {
        default:
        {
            int u = item - ITEM_UPGRADE_FIRST;
            int w = u / MERCHANT_STAT_COUNT, s = u % MERCHANT_STAT_COUNT;
            set_level(w, s, Pc_Merchant_Level(w, s) + 1);
            Pc_Revenge_ApplyUpgrades();
            Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
            Pc_Revenge_Voice("merchant_upgrade");
            SH_DBG("[MERCHANT] %s -> Lv %d", Pc_Merchant_ItemName(item), Pc_Merchant_Level(w, s));
            return;
        }
    }
}

/* ------------------------------------------------------------------ open */

static int monsters_nearby(void)
{
    VECTOR3* p = &g_SysWork.playerWork.player.position;
    int      i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        s64 dx, dz;

        if (npc->model.charaId < Chara_AirScreamer || npc->model.charaId > Chara_MonsterCybil ||
            npc->health <= Q12(0.0f))
            continue;
        dx = (s64)npc->position.vx - p->vx;
        dz = (s64)npc->position.vz - p->vz;
        if (dx * dx + dz * dz < (s64)MERCHANT_SAFE_RANGE * MERCHANT_SAFE_RANGE)
            return 1;
    }
    return 0;
}

void Pc_Merchant_TryOpen(void)
{
    if (!Pc_Revenge_Active() || g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState != SysState_Gameplay ||
        (g_SysWork.sysFlags & (SysFlag_DemoActive | SysFlag_CutsceneActive)) ||
        g_SysWork.playerWork.player.health <= Q12(0.0f))
        return;

    if (monsters_nearby())
    {
        Sd_PlaySfx(Sfx_MenuCancel, 0, 64);
        DbgOverlay_ToastLine("The merchant won't come out with monsters nearby.");
        return;
    }

    Pc_QuickOptions_OpenMerchant();
    Pc_Revenge_Voice("merchant_hello");
}

void Pc_Merchant_OnClose(void)
{
    Pc_Revenge_Voice("merchant_bye");
}
