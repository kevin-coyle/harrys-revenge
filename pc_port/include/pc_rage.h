/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_rage.h - Harry's Revenge: rage meter, kill streaks, the voice on the radio.
 */
#ifndef PC_RAGE_H
#define PC_RAGE_H

int  Pc_Rage_Active(void);
void Pc_Rage_Update(void);          /* per frame, from Pc_Revenge_Update */
void Pc_Rage_Draw(void);            /* per frame, the overlay layer */
void Pc_Rage_OnKill(void);
void Pc_Rage_OnHeadshot(void);

/* An announcer call (gamedata/revenge/sting_<name>.wav) over everything else. */
void Pc_Rage_Sting(const char* name);

/* Rage effects: Harry's damage, his move speed against the slowed world, and
 * the world clock itself (after the frame's dt is computed). */
int  Pc_Rage_DamagePct(void);
int  Pc_Rage_HarrySpeedPct(void);
void Pc_Rage_ScaleWorldTime(void);

/* Game_NpcUpdate: how many enemies the radio is tracking (0..3) this frame, and
 * how much to attenuate the static while the voice on the radio speaks. */
void Pc_Rage_OnRadioTick(int trackedEnemies);
int  Pc_Rage_RadioDuck(void);

#endif /* PC_RAGE_H */
