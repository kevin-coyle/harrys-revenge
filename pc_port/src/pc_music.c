/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_music.c - Harry's Revenge combat music.
 *
 * Looping WAVs (gamedata/revenge/music_combat_<n>.wav), one picked at random
 * for each fight, on their own SDL audio
 * device, outside the emulated SPU, faded in when a monster closes in and out
 * a few seconds after the last one is gone. The game's own BGM is ducked
 * underneath it through Sd_SetVolBgm, so the two never fight.
 */

#include <SDL.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/sound/sound_system.h"

#include "pc_config.h"
#include "pc_quick_options.h"
#include "pc_rage.h"
#include "pc_revenge.h"
#include "sh_log.h"

#include <stdio.h>
#include <stdlib.h>

extern const char*  PcPort_GetGameDataPath(void);
extern void         Pc_Sd_ReapplyBgmVolume(void);

#define COMBAT_RANGE   Q12(12.0f) /* a monster this close starts the music */
#define COMBAT_LINGER  Q12(5.0f)  /* keeps playing this long after the last one */
#define FADE_IN_MS     1200
#define FADE_OUT_MS    3500
#define BGM_DUCK_PCT   30
#define REWIND_AFTER   Q12(12.0f) /* silent this long: the next fight starts from the top */

#define MUSIC_TRACKS_MAX 8

static SDL_AudioDeviceID s_dev;
static Uint8*            s_tracks[MUSIC_TRACKS_MAX];
static Uint32            s_trackLen[MUSIC_TRACKS_MAX];
static int               s_trackCount;
static int               s_track = -1;
static Uint8*            s_pcm;   /* the current track, read by the audio thread */
static Uint32            s_len;
static Uint32            s_pos;
static volatile float    s_gain;     /* read by the audio thread */
static float             s_level;    /* 0..1 fade position */
static q19_12            s_linger;
static q19_12            s_silentFor = REWIND_AFTER + 1; /* the first fight picks at random too */
static int               s_tried;

int g_PcBgmDuckPct = 100;

static void SDLCALL fill(void* user, Uint8* out, int len)
{
    Sint16* dst  = (Sint16*)out;
    int     n    = len / 2;
    float   gain = s_gain;
    int     i;

    (void)user;
    for (i = 0; i < n; i++)
    {
        Sint16 v = *(Sint16*)(s_pcm + s_pos);
        dst[i]   = (Sint16)((float)v * gain);
        s_pos   += 2;
        if (s_pos >= s_len)
            s_pos = 0;
    }
}

/* All tracks must share the first one's format: the device is opened once. */
static int open_music(void)
{
    char          path[512];
    SDL_AudioSpec want, have, spec;
    int           n;

    s_tried = 1;
    for (n = 1; n <= MUSIC_TRACKS_MAX; n++)
    {
        Uint8* pcm;
        Uint32 len;

        snprintf(path, sizeof(path), "%s/revenge/music_combat_%d.wav", PcPort_GetGameDataPath(), n);
        if (!SDL_LoadWAV(path, &spec, &pcm, &len))
            break;
        if (spec.format != AUDIO_S16LSB || len < 4 ||
            (s_trackCount > 0 && (spec.freq != want.freq || spec.channels != want.channels)))
        {
            SH_DBG("[MUSIC] %s: skipped (needs 16-bit PCM in the first track's format)", path);
            SDL_FreeWAV(pcm);
            continue;
        }
        if (s_trackCount == 0)
            want = spec;
        s_tracks[s_trackCount]   = pcm;
        s_trackLen[s_trackCount] = len;
        s_trackCount++;
    }
    if (s_trackCount == 0)
    {
        SH_DBG("[MUSIC] no combat music in %s/revenge", PcPort_GetGameDataPath());
        return 0;
    }
    s_track = 0;
    s_pcm   = s_tracks[0];
    s_len   = s_trackLen[0];
    if (!SDL_WasInit(SDL_INIT_AUDIO))
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    want.callback = fill;
    want.userdata = NULL;
    want.samples  = 2048;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_dev == 0)
    {
        SH_DBG("[MUSIC] audio device: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

static int monster_near(void)
{
    VECTOR3* p = &g_SysWork.playerWork.player.position;
    int      i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        s64             dx, dz;

        if (npc->model.charaId < Chara_AirScreamer || npc->model.charaId > Chara_MonsterCybil ||
            npc->health <= Q12(0.0f))
            continue;
        dx = (s64)npc->position.vx - p->vx;
        dz = (s64)npc->position.vz - p->vz;
        if (dx * dx + dz * dz < (s64)COMBAT_RANGE * COMBAT_RANGE)
            return 1;
    }
    return 0;
}

static void set_bgm_duck(int pct)
{
    if (pct == g_PcBgmDuckPct)
        return;
    g_PcBgmDuckPct = pct;
    Pc_Sd_ReapplyBgmVolume();
}

void Pc_Music_Update(void)
{
    int    playing = g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
                     !Pc_QuickOptions_IsOpen() && !(g_SysWork.sysFlags & SysFlag_CutsceneActive);
    int    wantOn;
    float  step;
    Uint32 dtMs = (Uint32)(((s64)g_DeltaTimeRaw * 1000) >> 12);

    if (!Pc_Revenge_Active() || g_PcConfig.revengeMusic <= 0)
    {
        if (s_dev)
            SDL_PauseAudioDevice(s_dev, 1);
        set_bgm_duck(100);
        return;
    }
    if (!s_dev && (s_tried || !open_music()))
        return;

    if (playing && (monster_near() || Pc_Rage_Active()))
        s_linger = COMBAT_LINGER;
    else if (s_linger > 0)
        s_linger -= g_DeltaTimeRaw;
    wantOn = playing && s_linger > 0;

    if (wantOn)
    {
        step     = (float)dtMs / FADE_IN_MS;
        s_level += step;
        if (s_level > 1.0f) s_level = 1.0f;
    }
    else
    {
        step     = (float)dtMs / FADE_OUT_MS;
        s_level -= step;
        if (s_level < 0.0f) s_level = 0.0f;
    }

    /* Equal-power curve so the fade does not seem to hang at the top. */
    s_gain = s_level * s_level * (g_PcConfig.revengeMusic / 100.0f) * g_PcConfig.xaVolume;

    if (s_level > 0.0f)
    {
        /* A new fight after a quiet spell gets a different track. */
        if (s_silentFor > REWIND_AFTER && s_trackCount > 1)
        {
            int next = (s_track + 1 + rand() % (s_trackCount - 1)) % s_trackCount;
            SDL_LockAudioDevice(s_dev);
            s_track = next;
            s_pcm   = s_tracks[next];
            s_len   = s_trackLen[next];
            s_pos   = 0;
            SDL_UnlockAudioDevice(s_dev);
        }
        s_silentFor = 0;
        SDL_PauseAudioDevice(s_dev, 0);
    }
    else
    {
        SDL_PauseAudioDevice(s_dev, 1);
        if (s_silentFor <= REWIND_AFTER)
            s_silentFor += g_DeltaTimeRaw;
    }

    set_bgm_duck(100 - (int)((100 - BGM_DUCK_PCT) * s_level + 0.5f));
}
