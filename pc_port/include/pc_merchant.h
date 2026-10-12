/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_merchant.h - Harry's Revenge money, shop and weapon upgrades.
 *
 * Money and upgrade levels live inside the savegame, in bytes the original
 * game never reads, so they are saved, loaded, autosaved and reset on New Game
 * by the game's own code. The shop is a mode of the quick options overlay.
 */
#ifndef PC_MERCHANT_H
#define PC_MERCHANT_H

#include "types.h"

enum { MERCHANT_WPN_HANDGUN = 0, MERCHANT_WPN_SHOTGUN, MERCHANT_WPN_RIFLE, MERCHANT_WPN_MELEE,
       MERCHANT_WPN_COUNT };
enum { MERCHANT_STAT_DAMAGE = 0, MERCHANT_STAT_CAPACITY, MERCHANT_STAT_RELOAD, MERCHANT_STAT_COUNT };
#define MERCHANT_LEVEL_MAX 5

int  Pc_Merchant_Money(void);
void Pc_Merchant_AddMoney(int amount);
int  Pc_Merchant_Level(int weapon, int stat);

/* Money for a kill, by monster type. */
void Pc_Merchant_OnKill(int charaId);

/* Opens the shop when it is safe (no monsters nearby, normal gameplay). */
void Pc_Merchant_TryOpen(void);
void Pc_Merchant_OnClose(void);

/* Shop pages for the quick options overlay. */
#define MERCHANT_PAGES 4
int         Pc_Merchant_PageCount(int page);
int         Pc_Merchant_PageItem(int page, int row);
const char* Pc_Merchant_PageName(int page);
const char* Pc_Merchant_ItemName(int item);
const char* Pc_Merchant_ItemLabel(int item, char* buf, int n);
void        Pc_Merchant_Buy(int item);

#endif /* PC_MERCHANT_H */
