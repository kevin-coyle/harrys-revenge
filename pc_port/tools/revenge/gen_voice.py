#!/usr/bin/env python3
"""Generate Harry's Revenge voice lines with ElevenLabs (eleven_v4).

Writes 16-bit mono WAVs into pc_port/assets/gamedata:
  revenge/bark_<event>_<n>.wav  combat lines (pc_revenge.c)
  load/XA/msg_<KEY>.wav         voices for text boxes the game never voiced (pc_msg_voice.c)

Existing files are kept, so re-running only fills in new or deleted lines.
Usage: ELEVENLABS_KEY=... python3 gen_voice.py [--force]
"""
import json
import os
import sys
import urllib.request
import wave

VOICE_ID = "SOYHLrjzK2X1ezoPC6cr"  # "Harry - Fierce Warrior" (ElevenLabs stock voice)
MODEL_ID = "eleven_v4"
RATE = 24000

BARKS = {
    "kill": [
        "[growls] Stay down.",
        "[breathing hard] That's for Cheryl.",
        "[coldly] Next.",
        "[exhales] Who's next?",
        "You picked the wrong man.",
        "[through gritted teeth] Not today.",
    ],
    "hurt": [
        "[strained] I'm not done yet...",
        "[panting] Just a scratch. Keep moving.",
        "[grunts] Still standing.",
        "[groans] Is that all you've got?",
    ],
    "reload": [
        "[muttering] Reloading.",
        "[quickly] Come on, come on...",
        "[calm] Fresh rounds.",
    ],
    "area": [
        "[determined] Cheryl... I'm coming.",
        "[low] Whatever's in here... it's not leaving.",
        "[quietly] Let's finish this.",
        "[angry] This town took my daughter. Now I take it back.",
    ],
    "ammo": [
        "[satisfied] Ammo. Good.",
        "I'll take that.",
        "[grim] More rounds. Perfect.",
    ],
}

# Text boxes the original game leaves silent (pc_port/localization keys).
MESSAGES = {
    "COMMON.3": "[frustrated] I don't have the map for this place.",
    "COMMON.4": "[squinting] Too dark to look at the map here.",
    "COMMON.5": "[relieved] A first aid kit.",
    "COMMON.6": "A health drink.",
    "COMMON.7": "An ampoule.",
    "COMMON.8": "[satisfied] Handgun bullets.",
    "COMMON.9": "Rifle shells.",
    "COMMON.10": "Shotgun shells.",
    "COMMON.11": "[frustrated] The lock's jammed. This door can't be opened.",
    "COMMON.12": "[sighs] It's locked.",
    "COMMON.13": "It's unlocked.",
}


def tts(key, text):
    req = urllib.request.Request(
        f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}?output_format=pcm_{RATE}",
        data=json.dumps({"text": text, "model_id": MODEL_ID}).encode(),
        headers={"xi-api-key": key, "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def write_wav(path, pcm):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


def main():
    key = os.environ.get("ELEVENLABS_KEY")
    if not key:
        sys.exit("ELEVENLABS_KEY is not set")
    force = "--force" in sys.argv
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "gamedata")

    jobs = []
    for event, lines in BARKS.items():
        for i, text in enumerate(lines, 1):
            jobs.append((os.path.join(root, "revenge", f"bark_{event}_{i}.wav"), text))
    for msg, text in MESSAGES.items():
        jobs.append((os.path.join(root, "load", "XA", f"msg_{msg.replace('.', '_')}.wav"), text))

    for path, text in jobs:
        if os.path.exists(path) and not force:
            continue
        write_wav(path, tts(key, text))
        print(f"{os.path.relpath(path, root)}  {text}")


if __name__ == "__main__":
    main()
