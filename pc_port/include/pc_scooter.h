/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_scooter.h - Harry's Revenge: Harry's scooter (pc_scooter.c).
 */
#ifndef PC_SCOOTER_H
#define PC_SCOOTER_H

#include "types.h"

void Pc_Scooter_Update(void);       /* per frame: hop on / hop off */
void Pc_Scooter_Draw(GsOT* ot);     /* the world OT, beside the decals */
int  Pc_Scooter_Riding(void);
int  Pc_Scooter_SpeedPct(void);     /* Harry's displacement while riding */

#endif /* PC_SCOOTER_H */
