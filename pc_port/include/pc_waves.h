/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_waves.h - Harry's Revenge waves: rest, a wave of monsters out of the fog,
 * a score and rank when it is cleared, and after the last wave the area stays
 * clear for the session.
 */
#ifndef PC_WAVES_H
#define PC_WAVES_H

void Pc_Waves_Update(void);       /* per frame */
void Pc_Waves_Draw(void);         /* per frame, overlay layer */
void Pc_Waves_OnKill(void);       /* a wave monster died */
void Pc_Waves_OnMonsterLost(void);/* a wave monster despawned unkilled */
void Pc_Waves_OnHeadshot(void);

#endif /* PC_WAVES_H */
