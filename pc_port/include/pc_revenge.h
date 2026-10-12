/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_revenge.h - Harry's Revenge: a power-fantasy rebalance.
 *
 * Harry hits harder, takes less damage and recovers between fights, while
 * rooms fill with more monsters, some of them "invaders" from other parts of
 * the game, and kills drop ammo. Every knob is a revenge_* config key; with
 * revenge_mode = 0 each hook returns early and play is vanilla.
 */
#ifndef PC_REVENGE_H
#define PC_REVENGE_H

#include "types.h"

int  Pc_Revenge_Active(void);

/* Per frame, from the main loop (regeneration). */
void Pc_Revenge_Update(void);

/* Player weapon damage and damage taken by Harry, both before they are applied. */
s32  Pc_Revenge_ScaleWeaponDamage(s32 damageAmount, int targetIsPlayer);
s32  Pc_Revenge_ScaleDamageTaken(s32 damageAmount);
void Pc_Revenge_OnDamageTaken(void);

/* Game_NpcRoomInitSpawn: native row `row` just spawned into npc slot `slot`. */
void Pc_Revenge_OnNativeSpawn(int row, int slot);

/* An npc slot was (re)filled by a native spawn or emptied: forget its clone mark. */
void Pc_Revenge_SlotReset(int slot);

/* Clones share their parent row's field_40, so freeing one must not clear the
 * parent's session spawn bit or the parent could spawn a second time. */
int  Pc_Revenge_IsClone(int slot);

/* Once per corpse (ammo drops, kill lines). */
void Pc_Revenge_OnEnemyKilled(void);

/* Harry's voice lines for these moments (gamedata/revenge/bark_*.wav). */
void Pc_Revenge_OnReload(void);
void Pc_Revenge_OnMapLoad(void);

#endif /* PC_REVENGE_H */
