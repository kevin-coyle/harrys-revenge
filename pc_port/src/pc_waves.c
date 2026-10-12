/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_waves.c - Harry's Revenge waves (see pc_waves.h).
 *
 * Runs only where the map itself has live monsters (Pc_Revenge_AreaHasMonsters)
 * and only in free play; every timer here is real time, paused outside play.
 * Leaving an area mid-wave forfeits the wave; a cleared area is remembered for
 * the session only, since the save has no room for 45 more flags.
 */

#include <SDL.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/savegame.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/text/text_draw.h"

#include "dbg_overlay.h"
#include "pc_config.h"
#include "pc_merchant.h"
#include "pc_rage.h"
#include "pc_revenge.h"
#include "pc_waves.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>

#define WAVE_GRACE       Q12(6.0f)  /* arriving in an area: this long before wave 1 */
#define WAVE_SPAWN_EVERY Q12(1.2f)
#define WAVE_BANNER_MS   5500u

enum { W_IDLE = 0, W_REST, W_ACTIVE, W_DONE };

static int    s_state;
static int    s_mapIdx = -1;
static int    s_wave;
static q19_12 s_timer;
static q19_12 s_spawnIn;
static int    s_quota, s_spawned, s_kills, s_headshots;
static q19_12 s_elapsed;
static q19_12 s_prevHp;
static int    s_hpLost;
static int    s_restBarked;
static u8     s_cleared[64];

/* Banner: up to three centred lines, shown for WAVE_BANNER_MS. */
static char   s_banner[3][40];
static Uint32 s_bannerUntil;

/* Announcer calls one after another, so "Wave cleared!" and the rank do not
 * talk over each other. */
static struct { const char* name; Uint32 at; } s_queue[3];

static void say_later(const char* name, Uint32 delayMs)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(s_queue); i++)
    {
        if (s_queue[i].name == NULL)
        {
            s_queue[i].name = name;
            s_queue[i].at   = SDL_GetTicks() + delayMs;
            return;
        }
    }
}

static void banner(const char* a, const char* b, const char* c)
{
    snprintf(s_banner[0], sizeof(s_banner[0]), "%s", a ? a : "");
    snprintf(s_banner[1], sizeof(s_banner[1]), "%s", b ? b : "");
    snprintf(s_banner[2], sizeof(s_banner[2]), "%s", c ? c : "");
    s_bannerUntil = SDL_GetTicks() + WAVE_BANNER_MS;
}

static int waves_per_area(void)
{
    return g_PcConfig.revengeWaves;
}

static void start_wave(void)
{
    char line[40];
    int  size = g_PcConfig.revengeHorde > 0 ? g_PcConfig.revengeHorde : 10;

    s_wave++;
    s_state     = W_ACTIVE;
    /* 6, 11, 16... at the default size of 10, scaled with revenge_horde. */
    s_quota     = ((6 + 5 * (s_wave - 1)) * size) / 10;
    if (s_quota < 3)
        s_quota = 3;
    s_spawned   = 0;
    s_kills     = 0;
    s_headshots = 0;
    s_hpLost    = 0;
    s_elapsed   = 0;
    s_spawnIn   = 0;
    s_prevHp    = g_SysWork.playerWork.player.health;

    snprintf(line, sizeof(line), "WAVE %d OF %d", s_wave, waves_per_area());
    banner(line, "HERE THEY COME", NULL);
    Pc_Rage_Sting("wave");
    Pc_Revenge_Voice("wave_start");
    SH_DBG("[WAVE] map %d wave %d: %d monsters", s_mapIdx, s_wave, s_quota);
}

static void clear_wave(void)
{
    static const struct { int perKill; const char* sting; char rank; } RANKS[] = {
        { 300, "rank_s", 'S' }, { 220, "rank_a", 'A' }, { 150, "rank_b", 'B' }, { 0, "rank_c", 'C' },
    };
    int  secs  = s_elapsed >> Q12_SHIFT;
    int  par   = s_quota * 6;
    int  score = s_kills * 100 + s_headshots * 150 + (par > secs ? (par - secs) * 20 : 0) - s_hpLost * 15;
    int  i, cash;
    char l1[40], l2[40], l3[40];

    if (score < 0)
        score = 0;
    for (i = 0; i < (int)ARRAY_SIZE(RANKS) - 1; i++)
        if (score >= RANKS[i].perKill * s_quota)
            break;
    cash = score / 8;
    Pc_Merchant_AddMoney(cash);

    if (s_wave >= waves_per_area())
    {
        s_state                 = W_DONE;
        s_cleared[s_mapIdx & 63] = 1;
        snprintf(l1, sizeof(l1), "AREA CLEARED");
        say_later("area_clear", 0);
        Pc_Revenge_Voice("area_clear");
    }
    else
    {
        s_state      = W_REST;
        s_timer      = (q19_12)g_PcConfig.revengeRest << Q12_SHIFT;
        s_restBarked = 0;
        snprintf(l1, sizeof(l1), "WAVE %d CLEARED", s_wave);
        say_later("wave_clear", 0);
        Pc_Revenge_Voice("wave_clear");
    }
    say_later(RANKS[i].sting, 1700);
    snprintf(l2, sizeof(l2), "SCORE %d  RANK %c", score, RANKS[i].rank);
    snprintf(l3, sizeof(l3), "BONUS %d", cash);
    banner(l1, l2, l3);
    SH_DBG("[WAVE] cleared wave %d: kills=%d heads=%d secs=%d hpLost=%d score=%d rank=%c",
           s_wave, s_kills, s_headshots, secs, s_hpLost, score, RANKS[i].rank);
}

void Pc_Waves_OnKill(void)
{
    if (s_state == W_ACTIVE)
        s_kills++;
}

void Pc_Waves_OnMonsterLost(void)
{
    if (s_state == W_ACTIVE && s_spawned > 0)
        s_spawned--;
}

void Pc_Waves_OnHeadshot(void)
{
    if (s_state == W_ACTIVE)
        s_headshots++;
}

void Pc_Waves_Update(void)
{
    int    i;
    Uint32 now = SDL_GetTicks();

    for (i = 0; i < (int)ARRAY_SIZE(s_queue); i++)
    {
        if (s_queue[i].name != NULL && (Sint32)(now - s_queue[i].at) >= 0)
        {
            Pc_Rage_Sting(s_queue[i].name);
            s_queue[i].name = NULL;
        }
    }

    if (!Pc_Revenge_Active() || waves_per_area() <= 0 || g_GameWork.gameState != GameState_InGame)
        return;

    if (g_SavegamePtr->mapIdx != s_mapIdx)
    {
        s_mapIdx = g_SavegamePtr->mapIdx;
        s_wave   = 0;
        s_state  = s_cleared[s_mapIdx & 63] ? W_DONE : W_REST;
        s_timer  = WAVE_GRACE;
        Pc_Revenge_ForgetWave();
    }

    if (g_SysWork.sysState != SysState_Gameplay ||
        (g_SysWork.sysFlags & (SysFlag_CutsceneActive | SysFlag_DemoActive)) ||
        g_SysWork.playerWork.player.health <= Q12(0.0f))
        return;

    if (s_state == W_DONE)
        return;
    if (!Pc_Revenge_AreaHasMonsters())
    {
        /* The story emptied the area (or it never had monsters). */
        if (s_state == W_ACTIVE)
            s_state = W_REST;
        return;
    }

    if (s_state == W_REST)
    {
        s_timer -= g_DeltaTimeRaw;
        if (!s_restBarked && s_wave > 0 && s_timer < ((q19_12)g_PcConfig.revengeRest << Q12_SHIFT) - Q12(6.0f))
        {
            s_restBarked = 1;
            Pc_Revenge_Bark("rest", 70);
        }
        if (s_timer <= 0)
            start_wave();
        return;
    }

    /* W_ACTIVE */
    s_elapsed += g_DeltaTimeRaw;
    if (g_SysWork.playerWork.player.health < s_prevHp)
        s_hpLost += (s_prevHp - g_SysWork.playerWork.player.health) >> Q12_SHIFT;
    s_prevHp = g_SysWork.playerWork.player.health;

    s_spawnIn -= g_DeltaTimeRaw;
    if (s_spawned < s_quota && s_spawnIn <= 0 &&
        Pc_Revenge_WaveMonstersAlive() < (g_PcConfig.revengeHorde > 0 ? g_PcConfig.revengeHorde : 10))
    {
        s_spawnIn = WAVE_SPAWN_EVERY;
        if (Pc_Revenge_SpawnWaveMonster() >= 0)
            s_spawned++;
    }

    if (s_spawned >= s_quota && Pc_Revenge_WaveMonstersAlive() == 0)
        clear_wave();
}

/* Text space is top-left based, 320 wide; glyphs are 12 wide; '_' is a space.
 * The game font has letters, digits and little else, so the banners keep to those. */
static void draw_line(const char* text, int y, s16 color)
{
    char buf[40];
    int  i, n = (int)strlen(text);

    for (i = 0; i < n && i < (int)sizeof(buf) - 1; i++)
        buf[i] = (text[i] == ' ') ? '_' : text[i];
    buf[i] = 0;
    Gfx_StringSetPosition((320 - n * 12) / 2, y);
    Gfx_StringSetColor(color);
    Gfx_StringDraw(buf, 99);
}

void Pc_Waves_Draw(void)
{
    char line[40];

    if (!Pc_Revenge_Active() || waves_per_area() <= 0)
        return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay)
        return;

    if ((Sint32)(s_bannerUntil - SDL_GetTicks()) > 0)
    {
        draw_line(s_banner[0], 52, StringColorId_Red);
        if (s_banner[1][0])
            draw_line(s_banner[1], 70, StringColorId_White);
        if (s_banner[2][0])
            draw_line(s_banner[2], 88, StringColorId_Gold);
    }

    /* Status under the rage bar. */
    line[0] = 0;
    if (s_state == W_ACTIVE)
        snprintf(line, sizeof(line), "WAVE_%d__LEFT_%d", s_wave,
                 (s_quota - s_spawned) + Pc_Revenge_WaveMonstersAlive());
    else if (s_state == W_REST && s_wave > 0 && s_timer < Q12(10.0f) && s_timer > 0)
        snprintf(line, sizeof(line), "NEXT_WAVE_%d", (s_timer >> Q12_SHIFT) + 1);
    else if (s_state == W_DONE && Pc_Revenge_AreaHasMonsters())
        snprintf(line, sizeof(line), "AREA_CLEAR");
    if (line[0])
    {
        Gfx_StringSetPosition(10, 220);
        Gfx_StringSetColor(StringColorId_LightGrey);
        Gfx_StringDraw(line, 99);
    }
}
