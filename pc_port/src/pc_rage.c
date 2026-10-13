/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_rage.c - Harry's Revenge: the rage meter, kill streaks and the voice on
 * the radio (see pc_rage.h).
 *
 * Rage slows the world rather than speeding Harry up: g_DeltaTime is scaled
 * after the frame clock is computed and Harry's displacement is scaled back
 * up by the inverse, so monsters crawl while he moves at his normal pace.
 * g_DeltaTimeRaw is left alone, so voices, menus, fades and every timer in
 * this file keep real time.
 */

#include <SDL.h>
#include <PsyX/PsyX_public.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/savegame.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/text/text_draw.h"

#include "control_style.h"
#include "dbg_overlay.h"
#include "pc_config.h"
#include "pc_merchant.h"
#include "pc_quick_options.h"
#include "pc_rage.h"
#include "pc_revenge.h"
#include "pc_ui_sound.h"
#include "sh_log.h"

#include <stdio.h>

extern const char* PcPort_GetGameDataPath(void);
extern int         XaPlayer_IsFilePlaying(void);
extern bool        PC_RawControllerButtonClicked(int sdlButton);
extern int         g_PcBindsGen;

#define RAGE_MAX        100
#define RAGE_PER_KILL   12
#define RAGE_PER_HEAD   5
#define RAGE_DURATION   Q12(8.0f)
#define RAGE_TIME_PCT   55  /* world speed while raging */
#define RAGE_DAMAGE_PCT 300

#define STREAK_WINDOW_MS 4000u

#define RADIO_QUIET      Q12(8.0f)  /* monsters must have been out of range this long */
#define RADIO_COOLDOWN   Q12(75.0f) /* between taunts */
#define RADIO_REPLY_WAIT Q12(0.4f)

static int    s_rage;
static q19_12 s_rageLeft;

static int    s_streak;
static Uint32 s_lastKillMs;

static q19_12 s_radioQuiet = RADIO_QUIET;
static q19_12 s_radioCooldown;
static int    s_radioPrevInRange;
static int    s_villainSpeaking;
static q19_12 s_replyIn = -1;

/* ------------------------------------------------------------------ stings */

/* The announcer plays over everything (outside the voice channel), so a
 * streak call never waits for Harry to finish talking. */
void Pc_Rage_Sting(const char* name)
{
    static struct { const char* name; PcUiSound* snd; int loaded; } cache[16];
    char path[512];
    int  i;

    for (i = 0; i < (int)ARRAY_SIZE(cache) && cache[i].name; i++)
    {
        if (strcmp(cache[i].name, name) == 0)
            break;
    }
    if (i == (int)ARRAY_SIZE(cache))
        return;
    if (!cache[i].loaded)
    {
        snprintf(path, sizeof(path), "%s/revenge/sting_%s.wav", PcPort_GetGameDataPath(), name);
        cache[i].name   = name;
        cache[i].snd    = PcUiSound_Load(path);
        cache[i].loaded = 1;
    }
    PcUiSound_PlayGain(cache[i].snd, g_PcConfig.xaVolume);
}

/* ------------------------------------------------------------------ rage */

int Pc_Rage_Active(void)
{
    return Pc_Revenge_Active() && s_rageLeft > 0;
}

static void rage_add(int amount)
{
    int was = s_rage;

    if (!Pc_Revenge_Active() || !g_PcConfig.revengeRage || Pc_Rage_Active())
        return;
    s_rage += amount;
    if (s_rage > RAGE_MAX)
        s_rage = RAGE_MAX;
    if (was < RAGE_MAX && s_rage == RAGE_MAX)
    {
        char line[96];
        snprintf(line, sizeof(line), "RAGE READY  -  press %s", g_PcConfig.keyRage);
        DbgOverlay_ToastLine(line);
    }
}

static void rage_start(void)
{
    s_rage     = 0;
    s_rageLeft = RAGE_DURATION;
    Pc_Rage_Sting("revenge");
    Pc_Revenge_Voice("rage");
    DbgOverlay_ToastLine("HARRY'S REVENGE!");
}

int Pc_Rage_DamagePct(void)
{
    return Pc_Rage_Active() ? RAGE_DAMAGE_PCT : 100;
}

int Pc_Rage_HarrySpeedPct(void)
{
    return Pc_Rage_Active() ? (100 * 100) / RAGE_TIME_PCT : 100;
}

void Pc_Rage_ScaleWorldTime(void)
{
    static s32 rem;
    s32        num;

    if (!Pc_Rage_Active() || g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState != SysState_Gameplay)
    {
        rem = 0;
        return;
    }
    /* Carry the remainder: at high frame rates dt is a handful of Q12 units
     * and flooring each frame would lose several percent. */
    num            = g_DeltaTime * RAGE_TIME_PCT + rem;
    g_DeltaTime    = num / 100;
    rem            = num % 100;
    g_GravitySpeed = Q12_MULT(g_DeltaTime, Q12(9.8f));
}

/* ------------------------------------------------------------------ kills */

void Pc_Rage_OnKill(void)
{
    static const struct { int count; const char* name; const char* text; int bonus; } STREAKS[] = {
        { 2, "double",      "DOUBLE KILL  +$50",   50  },
        { 3, "triple",      "TRIPLE KILL  +$100",  100 },
        { 5, "rampage",     "RAMPAGE  +$250",      250 },
        { 8, "unstoppable", "UNSTOPPABLE  +$500",  500 },
    };
    Uint32 now = SDL_GetTicks();
    int    i;

    if (!Pc_Revenge_Active())
        return;

    rage_add(RAGE_PER_KILL);

    s_streak     = (s_lastKillMs != 0 && now - s_lastKillMs < STREAK_WINDOW_MS) ? s_streak + 1 : 1;
    s_lastKillMs = now;

    for (i = 0; i < (int)ARRAY_SIZE(STREAKS); i++)
    {
        if (STREAKS[i].count == s_streak)
        {
            Pc_Rage_Sting(STREAKS[i].name);
            DbgOverlay_ToastLine(STREAKS[i].text);
            Pc_Merchant_AddMoney(STREAKS[i].bonus);
            Pc_Revenge_Bark("streak", 45);
            break;
        }
    }
}

void Pc_Rage_OnHeadshot(void)
{
    rage_add(RAGE_PER_HEAD);
}

/* ------------------------------------------------------------------ radio */

static int has_radio(void)
{
    int i;
    if (!(g_SavegamePtr->itemToggleFlags & ItemToggleFlag_RadioOn))
        return 0;
    for (i = 0; i < g_SavegamePtr->inventorySlotCount && i < INV_ITEM_COUNT_MAX; i++)
        if (g_SavegamePtr->items[i].id_0 == InvItemId_PocketRadio)
            return 1;
    return 0;
}

void Pc_Rage_OnRadioTick(int trackedEnemies)
{
    int inRange = trackedEnemies > 0;

    if (!Pc_Revenge_Active() || !g_PcConfig.revengeRadio)
        return;

    if (!inRange)
        s_radioQuiet += g_DeltaTimeRaw;

    /* Our own edge, not the static's keyon: closing the inventory or map
     * re-keys the static next to a monster that has been there all along. */
    if (inRange && !s_radioPrevInRange && s_radioQuiet >= RADIO_QUIET && s_radioCooldown <= 0 &&
        has_radio() && g_SysWork.sysState == SysState_Gameplay &&
        !(g_SysWork.sysFlags & SysFlag_CutsceneActive))
    {
        Pc_Revenge_Voice("radio");
        if (XaPlayer_IsFilePlaying())
        {
            s_villainSpeaking = 1;
            s_radioCooldown   = RADIO_COOLDOWN;
        }
    }

    if (inRange)
        s_radioQuiet = 0;
    s_radioPrevInRange = inRange;
}

int Pc_Rage_RadioDuck(void)
{
    return s_villainSpeaking ? 0xA0 : 0;
}

/* ------------------------------------------------------------------ input */

static void poll_rage_button(void)
{
    static SDL_Scancode s_sc  = SDL_SCANCODE_UNKNOWN;
    static int          s_pad = -1;
    static int          s_res = 0;
    static int          s_prevKey;
    const Uint8*        ks = SDL_GetKeyboardState(NULL);
    int                 key, pad;

    if (s_res != g_PcBindsGen + 1)
    {
        s_sc  = SDL_GetScancodeFromName(g_PcConfig.keyRage);
        s_pad = g_PcConfig.padRage[0] ? PsyX_LookupGameControllerMapping(g_PcConfig.padRage, -1) : -1;
        s_res = g_PcBindsGen + 1;
    }
    key = (s_sc != SDL_SCANCODE_UNKNOWN && ks) ? ks[s_sc] : 0;
    /* On the Classic camera the pad's face buttons are all taken (Square runs). */
    pad = s_pad >= 0 && PC_RawControllerButtonClicked(s_pad) && g_ControlStyle != ControlStyle_Classic;

    if (((key && !s_prevKey) || pad) && s_rage >= RAGE_MAX && !Pc_Rage_Active() &&
        g_SysWork.sysState == SysState_Gameplay && !Pc_QuickOptions_IsOpen() &&
        g_SysWork.playerWork.player.health > Q12(0.0f))
        rage_start();
    s_prevKey = key;
}

/* ------------------------------------------------------------------ frame */

void Pc_Rage_Update(void)
{
    if (!Pc_Revenge_Active())
        return;

    if (s_radioCooldown > 0)
        s_radioCooldown -= g_DeltaTimeRaw;

    /* Harry answers once the voice on the radio has finished. */
    if (s_villainSpeaking && !XaPlayer_IsFilePlaying())
    {
        s_villainSpeaking = 0;
        s_replyIn         = RADIO_REPLY_WAIT;
    }
    if (s_replyIn >= 0)
    {
        s_replyIn -= g_DeltaTimeRaw;
        if (s_replyIn < 0)
            Pc_Revenge_Bark("radio_reply", 75);
    }

    if (g_SysWork.playerWork.player.health <= Q12(0.0f))
    {
        s_rageLeft = 0;
        s_streak   = 0;
    }
    else if (s_rageLeft > 0 && g_SysWork.sysState == SysState_Gameplay)
    {
        s_rageLeft -= g_DeltaTimeRaw;
        if (s_rageLeft <= 0)
        {
            s_rageLeft = 0;
            Pc_Revenge_Bark("rage_end", 70);
        }
    }

    if (g_PcConfig.revengeRage)
        poll_rage_button();
}

/* ------------------------------------------------------------------ HUD */

/* Bottom-left bar in the overlay layer, centre-origin 320x240 space. */
void Pc_Rage_Draw(void)
{
    static TILE     s_frame[2], s_fill[2];
    static DR_TPAGE s_tp[2];
    extern int      g_PsxCutsceneActive;
    extern int      g_PsxPresentLastFrame;
    /* Clear of the bottom edge: the widescreen view crops the PSX frame's
     * lowest rows, and the wave status (pc_waves.c) sits below this. */
    const int       x = -150, y = 75, w = 80, h = 5;
    int             buf, fillW, flash;
    u8              r, g, b;

    if (!Pc_Revenge_Active() || !g_PcConfig.revengeRage)
        return;
    if (g_GameWork.gameState != GameState_InGame || g_SysWork.sysState != SysState_Gameplay)
        return;
    if (g_PsxCutsceneActive || g_PsxPresentLastFrame)
        return;

    flash = (SDL_GetTicks() / 250) & 1;
    if (Pc_Rage_Active())
    {
        fillW = (int)(((s64)w * s_rageLeft) / RAGE_DURATION);
        r = 255; g = 200; b = 40;
    }
    else
    {
        fillW = (w * s_rage) / RAGE_MAX;
        r = (s_rage >= RAGE_MAX && flash) ? 255 : 190;
        g = 16; b = 24;
    }

    buf = g_ActiveBufferIdx;
    setTile(&s_fill[buf]);
    setXY0(&s_fill[buf], x, y);
    setWH(&s_fill[buf], fillW, h);
    setRGB0(&s_fill[buf], r, g, b);
    AddPrim(&g_OtTags0[buf][4], &s_fill[buf]);

    setTile(&s_frame[buf]);
    setSemiTrans(&s_frame[buf], 1);
    setXY0(&s_frame[buf], x - 1, y - 1);
    setWH(&s_frame[buf], w + 2, h + 2);
    setRGB0(&s_frame[buf], 40, 40, 40);
    AddPrim(&g_OtTags0[buf][4], &s_frame[buf]);
    /* Added last so it is drawn first: 50/50 blend for the frame. */
    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 0, 0, 0));
    AddPrim(&g_OtTags0[buf][4], &s_tp[buf]);

    /* Text space is top-left based (it subtracts 160,112). */
    Gfx_StringSetPosition(x + 160, y + 112 - 14);
    if (Pc_Rage_Active())
    {
        Gfx_StringSetColor(StringColorId_Gold);
        Gfx_StringDraw("REVENGE!", 99);
    }
    else if (s_rage >= RAGE_MAX)
    {
        Gfx_StringSetColor(flash ? StringColorId_Red : StringColorId_White);
        Gfx_StringDraw("RAGE_READY", 99);
    }
    else
    {
        Gfx_StringSetColor(StringColorId_LightGrey);
        Gfx_StringDraw("RAGE", 99);
    }
}
