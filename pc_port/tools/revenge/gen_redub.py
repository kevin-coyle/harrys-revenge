#!/usr/bin/env python3
"""Generate the Harry's Revenge redub from the rewritten script.

Input: script/part_*.json next to this file, each an array of
  {"key", "xa", "speaker", "orig", "text", "spoken"}
(key = language-pack message key, xa = disc voice line index).

Output, under pc_port/assets/gamedata:
  load/XA/xa_NNNN.wav   the new take for disc line NNNN (xa_wav.c override)
  lang/en_revenge.lang  the new subtitles, laid over the disc's English

A take noticeably longer than the original recording is sped up (up to 1.25x)
so cutscenes keep their pacing. Existing WAVs are kept, so a re-run only voices
new or deleted lines. Lines with no spoken words keep the original audio.
Usage: ELEVENLABS_KEY=... python3 gen_redub.py [--dry-run]
"""
import concurrent.futures
import glob
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "..", "assets", "gamedata")
MODEL_ID = "eleven_v4"
RATE = 24000
MAX_SPEEDUP = 1.25

# Stock ElevenLabs voices; none imitates the original cast.
VOICES = {
    "HARRY": "SOYHLrjzK2X1ezoPC6cr",     # Harry - Fierce Warrior
    "CYBIL": "EXAVITQu4vr4xnSDxMaL",     # Sarah - Mature, Reassuring, Confident
    "DAHLIA": "5PWbsfogbLtky5sxqtBz",    # Kayla - Evil Witch (the voice on the radio)
    "KAUFMANN": "cjVigY5qzO86Huf0OWal",  # Eric - Smooth, Trustworthy
    "LISA": "cgSgspJ2msm6clMCkdW9",      # Jessica - Playful, Bright, Warm
    "ALESSA": "FGY2WhTYpPnrIDTdsKH5",    # Laura - Enthusiast, Quirky
    "CHERYL": "FGY2WhTYpPnrIDTdsKH5",
}
DEFAULT_VOICE = "nPczCjzI2devNBz1zQrb"  # Brian - Deep, Resonant


def has_words(spoken):
    """A line that is only a delivery tag ([gasps]) keeps the original take."""
    return re.search(r"[A-Za-z]", re.sub(r"\[[^\]]*\]", "", spoken)) is not None


def lines():
    out = []
    for p in sorted(glob.glob(os.path.join(HERE, "script", "part_*.json"))):
        out += json.load(open(p, encoding="utf-8"))
    return out


def tts(key, text, voice):
    req = urllib.request.Request(
        f"https://api.elevenlabs.io/v1/text-to-speech/{voice}?output_format=pcm_{RATE}",
        data=json.dumps({"text": text, "model_id": MODEL_ID}).encode(),
        headers={"xi-api-key": key, "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=180) as r:
        return r.read()


def write_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


def fit(path, target_s):
    """Speed a take up (pitch kept) when it overruns the original line."""
    with wave.open(path) as w:
        dur = w.getnframes() / w.getframerate()
    if target_s <= 0 or dur <= target_s * 1.1:
        return dur
    tempo = min(dur / target_s, MAX_SPEEDUP)
    tmp = path + ".tmp.wav"
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", path, "-filter:a", f"atempo={tempo:.3f}",
                    "-ac", "1", "-ar", str(RATE), "-sample_fmt", "s16", tmp], check=True)
    os.replace(tmp, path)
    return dur / tempo


def lang_value(text):
    # The pack format is one line per key: the game's own line breaks are the ~N
    # codes, so the cosmetic newline + indent after them is dropped.
    return text.replace("\n", "").replace("\t", " ")


def main():
    dry = "--dry-run" in sys.argv
    key = os.environ.get("ELEVENLABS_KEY")
    if not key and not dry:
        sys.exit("ELEVENLABS_KEY is not set")

    seconds = {}
    xa_json = os.path.join(HERE, "script", "xa_seconds.json")
    if os.path.exists(xa_json):
        seconds = {int(k): v for k, v in json.load(open(xa_json)).items()}

    todo, chars = [], 0
    xa_dir = os.path.join(ASSETS, "load", "XA")
    os.makedirs(xa_dir, exist_ok=True)
    for ln in lines():
        if not has_words(ln["spoken"]):
            continue
        path = os.path.join(xa_dir, "xa_%04d.wav" % ln["xa"])
        if not os.path.exists(path):
            todo.append((ln, path))
            chars += len(ln["spoken"])
    print(f"{len(todo)} lines to voice, {chars} characters")
    if dry:
        return

    def voice_one(job):
        ln, path = job
        voice = VOICES.get(ln["speaker"], DEFAULT_VOICE)
        for attempt in range(4):
            try:
                pcm = tts(key, ln["spoken"], voice)
                break
            except Exception as e:  # rate limits and timeouts: back off and retry
                if attempt == 3:
                    raise
                time.sleep(5 * (attempt + 1))
        write_wav(path, pcm)
        dur = fit(path, seconds.get(ln["xa"], 0))
        return f"xa {ln['xa']:4d} {ln['speaker']:9s} {dur:5.2f}s  {ln['spoken'][:70]}"

    # The plan allows a few concurrent requests.
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for i, msg in enumerate(pool.map(voice_one, todo), 1):
            print(f"[{i}/{len(todo)}] {msg}", flush=True)

    lang = os.path.join(ASSETS, "lang", "en_revenge.lang")
    with open(lang, "w", encoding="utf-8") as f:
        f.write("# Harry's Revenge -- the redubbed story's subtitles (generated by gen_redub.py).\n")
        f.write("# Only the rewritten lines; everything else falls back to the disc's English.\n")
        f.write("!font=latin\n!code=en_revenge\n!name=Harry's Revenge\n!menu=Harry's_Revenge\n")
        for ln in lines():
            if has_words(ln["spoken"]):
                f.write(f"{ln['key']}={lang_value(ln['text'])}\n")
    print("wrote", lang)


if __name__ == "__main__":
    main()
