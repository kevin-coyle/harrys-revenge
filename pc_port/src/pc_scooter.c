/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_scooter.c - Harry's Revenge: Harry's scooter.
 *
 * Bought from the merchant. In the open streets Harry hops on when he sprints
 * with no monster on top of him, rides at REVENGE_SCOOTER_SPEED, and hops off
 * when he stops, aims or a monster closes in.
 *
 * The model (scooter_mesh.h, from tools/revenge/make_scooter.py) is drawn the
 * way pc_decals.c draws bullet holes: vertices as Q8 offsets from the world-
 * screen matrix at Harry's position, RotTransPers, gouraud triangles into the
 * world OT on its own depth scale. Lighting is per vertex, from the rotated
 * normals, so the shading follows the scooter as it turns; the area's ambient
 * tint and fog ramp are applied the way the game colours its own geometry.
 *
 * "Low-Poly scooter" (https://skfb.ly/oIXJA) by supakorn.pim, CC BY 4.0.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/view/vw_calc.h"

#include "pc_config.h"
#include "pc_merchant.h"
#include "pc_revenge.h"
#include "pc_scooter.h"
#include "sh_log.h"

#include "scooter_mesh.h"

#define SCOOTER_SPEED_PCT  190
#define SCOOTER_RUN_SPEED  Q12(3.0f) /* run speeds are 3.5-5.0 */
#define SCOOTER_STOP_SPEED Q12(0.6f)
#define SCOOTER_MOUNT_RUN  Q12(0.5f) /* sprinting this long hops on */
#define SCOOTER_DISMOUNT   Q12(0.6f) /* stopped this long hops off */
#define SCOOTER_CLEARANCE  Q12(4.0f) /* a monster this close keeps Harry on foot */
#define SCOOTER_OT_SHIFT   1          /* OT0's own scale (see pc_decals.c) */

extern int  Pc_BloodFogKeep(s32 z);
extern s_WorldEnvWork g_WorldEnvWork;
extern void Pc_Revenge_ScooterLine(int mounting);

static int    s_riding;
static q19_12 s_runFor;
static q19_12 s_stopFor;

/* Light from above and slightly in front, in game space (y grows downwards). */
static const s32 LIGHT_X = 1229, LIGHT_Y = -3482, LIGHT_Z = 1638; /* ~ (0.3, -0.85, 0.4) Q12 */

static int outdoors(void)
{
    switch (g_SavegamePtr->mapIdx)
    {
        case MapIdx_MAP2_S00: /* Old Silent Hill streets */
        case MapIdx_MAP2_S02: /* Central Silent Hill streets */
        case MapIdx_MAP4_S02: /* the same, Otherworld */
        case MapIdx_MAP5_S01: /* Resort Area */
        case MapIdx_MAP6_S00: /* the same, Otherworld */
        case MapIdx_MAP6_S02: /* pier and lighthouse */
        case MapIdx_MAP6_S04: /* amusement park */
            return 1;
    }
    return 0;
}

int Pc_Scooter_Riding(void)
{
    return s_riding;
}

int Pc_Scooter_SpeedPct(void)
{
    return s_riding ? SCOOTER_SPEED_PCT : 100;
}

static void set_riding(int on)
{
    if (on == s_riding)
        return;
    s_riding  = on;
    s_runFor  = 0;
    s_stopFor = 0;
    Pc_Revenge_ScooterLine(on);
    SH_DBG("[SCOOTER] %s", on ? "on" : "off");
}

void Pc_Scooter_Update(void)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    int             can;

    can = Pc_Revenge_Active() && Pc_Merchant_HasScooter() && outdoors() &&
          g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
          !(g_SysWork.sysFlags & SysFlag_CutsceneActive) && player->health > Q12(0.0f) &&
          !g_SysWork.playerCombat.isAiming;

    if (!can || Pc_Revenge_MonstersWithin(SCOOTER_CLEARANCE) > 0)
    {
        set_riding(0);
        return;
    }

    if (!s_riding)
    {
        s_runFor = (player->moveSpeed >= SCOOTER_RUN_SPEED) ? s_runFor + g_DeltaTimeRaw : 0;
        if (s_runFor >= SCOOTER_MOUNT_RUN)
            set_riding(1);
        return;
    }

    s_stopFor = (player->moveSpeed < SCOOTER_STOP_SPEED) ? s_stopFor + g_DeltaTimeRaw : 0;
    if (s_stopFor >= SCOOTER_DISMOUNT)
        set_riding(0);
}

static u8 shade(s32 base, s32 light, s32 tint, s32 fogKeep, s32 fogCol)
{
    s32 c = (base * light) >> 12;      /* lit */
    c     = (c * tint) >> 7;           /* the area's ambient tint, 128 = neutral */
    c     = (c * fogKeep + fogCol * (256 - fogKeep)) >> 8;
    return (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
}

void Pc_Scooter_Draw(GsOT* ot)
{
    static s16 sx[SCOOTER_VERT_COUNT], sy[SCOOTER_VERT_COUNT];
    static s32 sz[SCOOTER_VERT_COUNT];
    static u8  col[SCOOTER_VERT_COUNT][3];
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    POLY_G3*        poly;
    MATRIX          mat;
    s32             c, s, tr, tg, tb;
    int             i;

    if (!s_riding)
        return;

    /* Model front is +z; heading 0 faces +z and grows towards +x. */
    c = Math_Cos(player->rotation.vy);
    s = Math_Sin(player->rotation.vy);

    tr = Q12_MULT(g_WorldEnvWork.worldTintColor.r, g_WorldEnvWork.field_20);
    tg = Q12_MULT(g_WorldEnvWork.worldTintColor.g, g_WorldEnvWork.field_20);
    tb = Q12_MULT(g_WorldEnvWork.worldTintColor.b, g_WorldEnvWork.field_20);
    tr = tr < 24 ? 24 : tr > 160 ? 160 : tr;
    tg = tg < 24 ? 24 : tg > 160 ? 160 : tg;
    tb = tb < 24 ? 24 : tb > 160 ? 160 : tb;

    Vw_WorldScreenMatrixAtPositionGet(&mat, player->position.vx, player->position.vy, player->position.vz);
    SetRotMatrix(&mat);
    SetTransMatrix(&mat);

    for (i = 0; i < SCOOTER_VERT_COUNT; i++)
    {
        const s_ScooterVert* v = &SCOOTER_VERTS[i];
        SVECTOR p;
        s32     x, z, nx, nz, ndl, light, keep, sxy;
        long    depth, flag;

        x     = (v->x * c + v->z * s) >> 12;
        z     = (v->z * c - v->x * s) >> 12;
        p.vx  = (s16)(x >> 4); /* Q12 -> Q8, the space the WS matrix translation is in */
        p.vy  = (s16)(v->y >> 4);
        p.vz  = (s16)(z >> 4);
        p.pad = 0;
        sz[i] = RotTransPers(&p, (int*)&sxy, &depth, &flag);
        sx[i] = (s16)(sxy & 0xFFFF);
        sy[i] = (s16)((u32)sxy >> 16);

        nx    = (v->nx * c + v->nz * s) >> 12;
        nz    = (v->nz * c - v->nx * s) >> 12;
        ndl   = (nx * LIGHT_X + v->ny * LIGHT_Y + nz * LIGHT_Z) >> 12;
        light = Q12(0.45f) + ((ndl > 0 ? ndl : 0) * 3) / 5;

        keep      = Pc_BloodFogKeep(sz[i] << 2);
        col[i][0] = shade(v->r, light, tr, keep, g_WorldEnvWork.fog.color.r);
        col[i][1] = shade(v->g, light, tg, keep, g_WorldEnvWork.fog.color.g);
        col[i][2] = shade(v->b, light, tb, keep, g_WorldEnvWork.fog.color.b);
    }

    poly = (POLY_G3*)GsOUT_PACKET_P;
    for (i = 0; i < SCOOTER_TRI_COUNT; i++)
    {
        int a = SCOOTER_TRIS[i][0], b = SCOOTER_TRIS[i][1], d = SCOOTER_TRIS[i][2];
        s32 bucket;

        if (sz[a] <= 8 || sz[b] <= 8 || sz[d] <= 8)
            continue; /* at or behind the near plane */
        if (ABS(sx[a]) > 2048 || ABS(sy[a]) > 2048 || ABS(sx[b]) > 2048 || ABS(sy[b]) > 2048 ||
            ABS(sx[d]) > 2048 || ABS(sy[d]) > 2048)
            continue;

        bucket = ((sz[a] + sz[b] + sz[d]) / 3) >> SCOOTER_OT_SHIFT;
        if (bucket < 1)
            bucket = 1;
        if (bucket >= (1 << ot->length))
            bucket = (1 << ot->length) - 1;

        setPolyG3(poly);
        setXY3(poly, sx[a], sy[a], sx[b], sy[b], sx[d], sy[d]);
        setRGB0(poly, col[a][0], col[a][1], col[a][2]);
        setRGB1(poly, col[b][0], col[b][1], col[b][2]);
        setRGB2(poly, col[d][0], col[d][1], col[d][2]);
        addPrim(&ot->org[bucket], poly);
        poly++;
    }
    GsOUT_PACKET_P = (PACKET*)poly;
}
