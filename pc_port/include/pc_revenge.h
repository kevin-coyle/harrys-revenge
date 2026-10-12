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

/* A voice line played now (merchant), and the module's RNG. */
void Pc_Revenge_Voice(const char* event);
u32  Pc_Revenge_Rand(void);

/* func_8008B714, before damage is applied: weapon upgrades, head and leg
 * shots, staggers. `attacker` and `target` are s_SubCharacter*. */
struct _SubCharacter;
void Pc_Revenge_OnHit(struct _SubCharacter* attacker, struct _SubCharacter* target, int weaponAttack,
                      const void* hitPos, s32* damage, u32* knockback);

/* A recently staggered enemy can be kicked like a downed one. */
int  Pc_Revenge_Kickable(int slot);

/* GameFs_WeaponInfoUpdate copied the equipped weapon's animations. */
void Pc_Revenge_OnWeaponAnimsLoaded(void);

/* Re-apply clip sizes and reload speed after a purchase. */
void Pc_Revenge_ApplyUpgrades(void);

/* Give an item without counting it as a pickup for the ranking. */
void Pc_Revenge_GiveItem(u8 id, u8 count);

#endif /* PC_REVENGE_H */
