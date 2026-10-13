/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_scooter.c - Harry's Revenge: Harry's scooter.
 *
 * Found parked: the first time Harry comes out into an open-street area
 * without it, it stands a couple of metres from where he arrived, and walking
 * up to it claims it (saved with the game). From then on, in the open streets
 * a double tap of Run (or key_scooter) hops on and off. Riding, Harry moves at
 * revenge_scooter_speed percent of his run whenever he moves at all; aiming, taking a hit or leaving the
 * streets puts him back on foot.
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

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/view/vw_calc.h"
#include "main/fileinfo.h"
#include "main/fsqueue.h"

#include "dbg_overlay.h"
#include "pc_config.h"
#include "pc_merchant.h"
#include "pc_quick_options.h"
#include "pc_revenge.h"
#include "pc_scooter.h"
#include "sh_log.h"

#include "scooter_mesh.h"

#define SCOOTER_RUN_REF     Q12(4.0f) /* Harry's typical run, units per second (3.5-5.0) */
#define SCOOTER_MOVING      Q12(0.3f)
#define SCOOTER_SPEED_CAP   900       /* percent: walking starts well below run speed */
#define SCOOTER_DOUBLE_TAP  350u      /* ms between the two taps of Run */
#define SCOOTER_OT_SHIFT   1          /* OT0's own scale (see pc_decals.c) */

extern int  Pc_BloodFogKeep(s32 z);
extern int  g_PcBindsGen;
extern s_WorldEnvWork g_WorldEnvWork;
extern void Pc_Revenge_ScooterLine(int mounting);

#define SCOOTER_CLAIM_RANGE Q12(1.4f)
#define SCOOTER_PARK_SIDE   Q12(2.2f) /* parked this far beside the arrival point */

static int    s_riding;
static q19_12 s_lastHp;
static Uint32 s_lastRunTap;

/* Parked, waiting to be found: where, which way it faces, and on which map. */
static int    s_parkedMap = -1;
static int    s_parkPending;
static int    s_lastMap = -1;
static q19_12 s_parkX, s_parkY, s_parkZ;
static s16    s_parkYaw;

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

/* Riding, any movement at all becomes the scooter's cruising speed. */
int Pc_Scooter_SpeedPct(void)
{
    q19_12 speed = g_SysWork.playerWork.player.moveSpeed;
    int     pct;

    if (!s_riding || speed < SCOOTER_MOVING)
        return 100;
    pct = (int)(((s64)SCOOTER_RUN_REF * g_PcConfig.revengeScooterSpeed) / speed);
    return pct < 100 ? 100 : pct > SCOOTER_SPEED_CAP ? SCOOTER_SPEED_CAP : pct;
}

static void set_riding(int on)
{
    if (on == s_riding)
        return;
    s_riding = on;
    s_lastHp = g_SysWork.playerWork.player.health;
    Pc_Revenge_ScooterLine(on);
    SH_DBG("[SCOOTER] %s", on ? "on" : "off");
}

/* Beside Harry's arrival point, on walkable ground: his right, then his left,
 * then ahead. Facing the way he faces, so it reads as parked for him. */
static void park_near_harry(void)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    s16             yaw    = player->rotation.vy;
    static const s16 OFFSETS[] = { Q12_ANGLE(90.0f), Q12_ANGLE(-90.0f), 0 };
    int             i;

    for (i = 0; i < (int)ARRAY_SIZE(OFFSETS); i++)
    {
        s16    a = (s16)(yaw + OFFSETS[i]);
        q19_12 x = player->position.vx + (q19_12)(((s64)SCOOTER_PARK_SIDE * Math_Sin(a)) >> 12);
        q19_12 z = player->position.vz + (q19_12)(((s64)SCOOTER_PARK_SIDE * Math_Cos(a)) >> 12);

        if (func_800808AC(x, z) != 0)
        {
            s_parkX     = x;
            s_parkY     = player->position.vy;
            s_parkZ     = z;
            s_parkYaw   = yaw;
            s_parkedMap = g_SavegamePtr->mapIdx;
            SH_DBG("[SCOOTER] parked on map %d", s_parkedMap);
            return;
        }
    }
}

static void update_parked(void)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    s64             dx, dz;

    if (Pc_Merchant_HasScooter())
    {
        s_parkedMap = -1;
        return;
    }
    if (g_SavegamePtr->mapIdx != s_lastMap)
    {
        s_lastMap     = g_SavegamePtr->mapIdx;
        s_parkPending = outdoors() && s_parkedMap < 0;
    }
    if (g_SysWork.sysState != SysState_Gameplay || (g_SysWork.sysFlags & SysFlag_CutsceneActive))
        return;
    if (s_parkPending)
    {
        s_parkPending = 0;
        park_near_harry();
    }
    if (s_parkedMap != g_SavegamePtr->mapIdx)
        return;

    dx = (s64)player->position.vx - s_parkX;
    dz = (s64)player->position.vz - s_parkZ;
    if (dx * dx + dz * dz < (s64)SCOOTER_CLAIM_RANGE * SCOOTER_CLAIM_RANGE)
    {
        Pc_Merchant_GiveScooter();
        s_parkedMap = -1;
        Pc_Revenge_Voice("scooter_found");
        DbgOverlay_ToastLine("Found a scooter! Double-tap Run in the open streets to ride.");
    }
}

/* A double tap of the game's own Run control (Square, or L1 on the alternate
 * cameras), whichever scheme is live, or key_scooter on the keyboard. */
static int toggle_pressed(void)
{
    static SDL_Scancode s_sc = SDL_SCANCODE_UNKNOWN;
    static int          s_res, s_prevKey;
    const Uint8*        ks = SDL_GetKeyboardState(NULL);
    u16                 runBtn = g_GameWorkPtr->config.controllerConfig.run;
    int                 key, hit = 0;
    Uint32              now = SDL_GetTicks();

    if (s_res != g_PcBindsGen + 1)
    {
        s_sc  = SDL_GetScancodeFromName(g_PcConfig.keyScooter);
        s_res = g_PcBindsGen + 1;
    }
    key = (s_sc != SDL_SCANCODE_UNKNOWN && ks) ? ks[s_sc] : 0;
    if (key && !s_prevKey)
        hit = 1;
    s_prevKey = key;

    if (g_Controller0 != NULL && (g_Controller0->clickedBtnFlags & runBtn))
    {
        if (s_lastRunTap != 0 && now - s_lastRunTap < SCOOTER_DOUBLE_TAP)
        {
            hit          = 1;
            s_lastRunTap = 0;
        }
        else
            s_lastRunTap = now;
    }
    return hit;
}

void Pc_Scooter_Update(void)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;
    int             can, pressed;

    if (Pc_Revenge_Active() && g_GameWork.gameState == GameState_InGame)
        update_parked();

    can = Pc_Revenge_Active() && Pc_Merchant_HasScooter() && outdoors() &&
          g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
          !(g_SysWork.sysFlags & SysFlag_CutsceneActive) && player->health > Q12(0.0f) &&
          !g_SysWork.playerCombat.isAiming && !Pc_QuickOptions_IsOpen();
    pressed = can && toggle_pressed();

    if (!can || (s_riding && player->health < s_lastHp))
    {
        set_riding(0); /* aiming, a hit, a door or a scene puts him on foot */
        return;
    }
    if (pressed)
        set_riding(!s_riding);
    s_lastHp = player->health;
}

/* ------------------------------------------------------------------ seat */

/* Harry has no riding animation; the cafe intro sits him in a booth. Its file
 * (HB_M0S01.ANM) is raw keyframes that continue HB_BASE's numbering from 676,
 * so a copy of HB_BASE's header (bind poses included) followed by the seated
 * keyframes makes an anim whose keyframe 0 is the seat. Read straight off the
 * disc image, not through the game's file queue, so riding can never disturb
 * a read the map has in flight. */
#define SEAT_FILE_FIRST_KF 676 /* map anim files start at this keyframe */
#define SEAT_HDR_BYTES     404 /* HB_BASE header + bind poses (dataOffset) */
#define SEAT_KF_BYTES      156 /* one Harry keyframe */
#define SEAT_KF_FIRST      827 /* the cafe's seated range */
#define SEAT_KF_COUNT      90
#define SEAT_KF            914 /* sitting upright, after the slumped frames */
#define SEAT_LIFT          Q12(0.30f) /* drawn this much higher, onto the seat */

extern unsigned char* Pc_LangReadDiscFile(unsigned int sector, unsigned int size);

static u8* s_seatAnm;
static int s_seatTried;

static int seat_load(void)
{
    const s_FileInfo* fe = &g_FileTable[FILE_ANIM_HB_M0S01_ANM];
    unsigned int      size, need;
    u8*               file;

    if (s_seatAnm != NULL || s_seatTried)
        return s_seatAnm != NULL;
    s_seatTried = 1;

    size = (unsigned int)fe->blockCount << 8;
    need = (SEAT_KF_FIRST - SEAT_FILE_FIRST_KF + SEAT_KF_COUNT) * SEAT_KF_BYTES;
    if (size < need)
        return 0;
    file = Pc_LangReadDiscFile(fe->startSector, size);
    if (file == NULL)
        return 0;

    s_seatAnm = (u8*)malloc(SEAT_HDR_BYTES + SEAT_KF_COUNT * SEAT_KF_BYTES);
    if (s_seatAnm != NULL)
    {
        memcpy(s_seatAnm, (const void*)FS_BUFFER_0, SEAT_HDR_BYTES);
        memcpy(s_seatAnm + SEAT_HDR_BYTES, file + (SEAT_KF_FIRST - SEAT_FILE_FIRST_KF) * SEAT_KF_BYTES,
               SEAT_KF_COUNT * SEAT_KF_BYTES);
        SH_DBG("[SCOOTER] seat pose loaded");
    }
    free(file);
    return s_seatAnm != NULL;
}

/* Player_Update, after every other pose write: hold the seat while riding and
 * lift the drawn body onto it. The state machines and anim timers run on
 * underneath untouched, so dismounting simply hands the pose back. */
void Pc_Scooter_PoseHarry(GsCOORDINATE2* coords)
{
    const int kf = SEAT_KF;

    if (!s_riding || !seat_load())
        return;

    g_SysWork.playerWork.extra.disabledAnimBones = 0;
    Anim_BoneUpdate((s_AnmHeader*)s_seatAnm, coords, kf - SEAT_KF_FIRST, kf - SEAT_KF_FIRST, Q12(0.0f));
    coords[0].coord.t[1] = Q12_TO_Q8(g_SysWork.playerWork.player.position.vy - SEAT_LIFT);
}

static u8 shade(s32 base, s32 light, s32 tint, s32 fogKeep, s32 fogCol)
{
    s32 c = (base * light) >> 12;      /* lit */
    c     = (c * tint) >> 7;           /* the area's ambient tint, 128 = neutral */
    c     = (c * fogKeep + fogCol * (256 - fogKeep)) >> 8;
    return (u8)(c < 0 ? 0 : c > 255 ? 255 : c);
}

static void draw_at(GsOT* ot, q19_12 px, q19_12 py, q19_12 pz, s16 yaw)
{
    static s16 sx[SCOOTER_VERT_COUNT], sy[SCOOTER_VERT_COUNT];
    static s32 sz[SCOOTER_VERT_COUNT];
    static u8  col[SCOOTER_VERT_COUNT][3];
    POLY_G3*        poly;
    MATRIX          mat;
    s32             c, s, tr, tg, tb;
    int             i;

    /* Model front is +z; heading 0 faces +z and grows towards +x. */
    c = Math_Cos(yaw);
    s = Math_Sin(yaw);

    tr = Q12_MULT(g_WorldEnvWork.worldTintColor.r, g_WorldEnvWork.field_20);
    tg = Q12_MULT(g_WorldEnvWork.worldTintColor.g, g_WorldEnvWork.field_20);
    tb = Q12_MULT(g_WorldEnvWork.worldTintColor.b, g_WorldEnvWork.field_20);
    tr = tr < 24 ? 24 : tr > 160 ? 160 : tr;
    tg = tg < 24 ? 24 : tg > 160 ? 160 : tg;
    tb = tb < 24 ? 24 : tb > 160 ? 160 : tb;

    Vw_WorldScreenMatrixAtPositionGet(&mat, px, py, pz);
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

void Pc_Scooter_Draw(GsOT* ot)
{
    s_SubCharacter* player = &g_SysWork.playerWork.player;

    if (s_riding)
        draw_at(ot, player->position.vx, player->position.vy, player->position.vz, player->rotation.vy);
    else if (s_parkedMap >= 0 && s_parkedMap == g_SavegamePtr->mapIdx)
        draw_at(ot, s_parkX, s_parkY, s_parkZ, s_parkYaw);
}
