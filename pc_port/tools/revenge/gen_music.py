#!/usr/bin/env python3
"""Generate Harry's Revenge combat music with Eleven Music.

Writes pc_port/assets/gamedata/revenge/music_combat_<n>.wav: 44.1 kHz stereo,
looped seamlessly by crossfading the last two seconds into the start.
Existing tracks are kept. Usage: ELEVENLABS_KEY=... python3 gen_music.py
"""
import json
import os
import subprocess
import sys
import urllib.request

import numpy as np
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "assets", "gamedata", "revenge")
RATE = 44100
COMMON = ("Instrumental combat music for a horror action video game. Steady high energy "
          "throughout with no intro and no fade-out so it loops seamlessly. ")
TRACKS = {
    1: "Aggressive industrial metal: heavily distorted down-tuned guitars, pounding double-kick "
       "drums, dark analog synth bass, eerie siren-like synth stabs, 140 BPM, relentless, a little "
       "cheesy 90s action-movie swagger.",
    2: "Darksynth chase: pulsing analog bass arpeggios, gated 80s drums, ominous retro horror synth "
       "leads, 128 BPM, driving and menacing like a neon slasher movie chase.",
    3: "Horror punk and psychobilly: fast slap-back surf guitar riffs, upright-bass slaps, punk "
       "drums, 180 BPM, gritty B-movie zombie flick energy, fun and frantic.",
    4: "Breakbeat drum and bass horror: fast chopped amen breaks at 174 BPM, growling reese bass, "
       "eerie choir pads, metallic percussion hits, tense and urgent.",
}


def compose(key, prompt):
    req = urllib.request.Request(
        "https://api.elevenlabs.io/v1/music?output_format=mp3_44100_128",
        data=json.dumps({"prompt": COMMON + prompt, "music_length_ms": 45000,
                         "force_instrumental": True}).encode(),
        headers={"xi-api-key": key, "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=600) as r:
        return r.read()


def loop_wav(mp3, path):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", "pipe:0", "-ac", "2", "-ar", str(RATE),
                          "-f", "s16le", "pipe:1"], input=mp3, capture_output=True, check=True).stdout
    a = np.frombuffer(raw, dtype=np.int16).reshape(-1, 2).astype(np.float32)
    xf = 2 * RATE
    out = a[:len(a) - xf].copy()
    t = np.linspace(0, 1, xf)[:, None]
    out[:xf] = a[:xf] * np.sqrt(t) + a[len(a) - xf:] * np.sqrt(1 - t)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.clip(out, -32768, 32767).astype(np.int16).tobytes())


def main():
    key = os.environ.get("ELEVENLABS_KEY")
    if not key:
        sys.exit("ELEVENLABS_KEY is not set")
    os.makedirs(OUT, exist_ok=True)
    for n, prompt in TRACKS.items():
        path = os.path.join(OUT, f"music_combat_{n}.wav")
        if os.path.exists(path):
            continue
        loop_wav(compose(key, prompt), path)
        print("wrote", path, flush=True)


if __name__ == "__main__":
    main()
