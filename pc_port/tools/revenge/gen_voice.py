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

VOICE_ID = "SOYHLrjzK2X1ezoPC6cr"     # "Harry - Fierce Warrior" (ElevenLabs stock voice)
MERCHANT_VOICE_ID = "N2lVS1w4EtoT3dr4eOWO"  # "Callum - Husky Trickster"
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
        "[smug] And stay down. I've got a daughter to find.",
        "[deadpan] Should've stayed in the fog.",
        "[sarcastic] Welcome to Silent Hill. Population... one less.",
        "[dryly] I'm a writer. I've had harsher critics.",
        "[chuckles] I've had worse dates.",
        "[muttering] Somebody call animal control.",
        "[exasperated] Cheryl is so grounded after this.",
    ],
    "hurt": [
        "[strained] I'm not done yet...",
        "[panting] Just a scratch. Keep moving.",
        "[grunts] Still standing.",
        "[groans] Is that all you've got?",
        "[groans] My chiropractor is going to hear about this.",
        "[wincing] Okay. Ow. Noted.",
        "[through gritted teeth] That's going to leave a mark. And a lawsuit.",
    ],
    "reload": [
        "[muttering] Reloading.",
        "[quickly] Come on, come on...",
        "[calm] Fresh rounds.",
        "[annoyed] Hold that thought, I'm reloading.",
        "[indignant] Very rude to attack a man while he's reloading.",
    ],
    "area": [
        "[determined] Cheryl... I'm coming.",
        "[low] Whatever's in here... it's not leaving.",
        "[quietly] Let's finish this.",
        "[angry] This town took my daughter. Now I take it back.",
        "[sighs] Great. Another creepy hallway. Love that for me.",
        "[dryly] Note to self. Never vacation here again.",
        "[sarcastic] Lovely fog. Real tourist trap.",
    ],
    "ammo": [
        "[satisfied] Ammo. Good.",
        "I'll take that.",
        "[grim] More rounds. Perfect.",
        "[pleased] Ooh, bullets. Don't mind if I do.",
        "[cheerfully] Free ammo. This town isn't all bad.",
    ],
    "money": [
        "[pleased] Cha-ching.",
        "[puzzled] Where do monsters even keep their wallets?",
        "That's going in the Cheryl fund.",
        "[satisfied] Thanks for the tip.",
    ],
    "invader": [
        "[confused] Wait... you're not from around here.",
        "[annoyed] Who invited you?",
        "[sighs] Oh good, they brought friends.",
    ],
    "buy": [
        "Pleasure doing business. I think.",
        "[satisfied] Worth every penny.",
        "[muttering] Should've asked for a receipt.",
    ],
    "broke": [
        "[sighs] I'm broke. Story of my life.",
        "[dryly] Can't afford it. Writing doesn't pay.",
    ],
    # Salty takes, mixed in when revenge_swearing = 1 (pc_revenge.c).
    # The user's limit: mild swearing, the F-word at most, never the C-word.
    "kill_salty": [
        "[growls] Eat lead, you ugly son of a bitch.",
        "[spits] Stay dead, asshole.",
        "[shouts] Hell yeah! Next!",
        "[disgusted] Piece of shit.",
        "[smug] Go to hell. Oh wait... you're already here.",
        "[breathing hard] Damn right. Damn right!",
    ],
    "hurt_salty": [
        "[groans] Son of a bitch!",
        "[shouts] Goddammit, that hurts!",
        "[wincing] Ow! Shit, shit, shit.",
        "[shouts] Get the fuck off me!",
    ],
    "reload_salty": [
        "[muttering] Come on, you piece of crap...",
        "[frantic] Shit, reloading!",
    ],
    "area_salty": [
        "[sighs] What the hell is this place?",
        "[annoyed] This whole damn town can kiss my ass.",
        "[exhausted] Fuck this town.",
    ],
    "ammo_salty": [
        "[pleased] Hell yes. Ammo.",
    ],
    "money_salty": [
        "[delighted] Hot damn, cash.",
    ],
    "invader_salty": [
        "[confused] Oh, what the hell are you?",
        "[annoyed] Where the hell did you come from?",
    ],
    "broke_salty": [
        "[frustrated] Damn it. Broke again.",
    ],
    "merchant_poor_salty": [
        "[scoffs] No money, no goods. Now piss off, stranger.",
    ],
    "merchant_bye_salty": [
        "[cackles] Don't die out there, ya poor bastard.",
    ],
    # The merchant (MERCHANT_VOICE_ID).
    "merchant_hello": [
        "[creepy whisper] Ah... a customer. What're you buyin', stranger?",
        "[chuckles] The fog brings me all sorts. What'll it be?",
        "[gleefully] Still breathing? Good for business!",
    ],
    "merchant_buy": [
        "[delighted] Heh heh heh... thank you.",
        "[smooth] A fine choice, stranger.",
        "[gleefully] Pleasure doin' business.",
    ],
    "merchant_upgrade": [
        "[gleeful] Ooh, she'll hit harder now.",
        "[admiring] Now that's a weapon worth dyin' for. Or killin' for.",
    ],
    "merchant_poor": [
        "[tsk] Not enough, stranger. Not nearly enough.",
        "[chuckles] Come back when you're a little... richer.",
    ],
    "merchant_bye": [
        "[ominous] Come back... alive, preferably.",
        "[whispering] The fog will be waiting.",
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


def tts(key, text, voice=VOICE_ID):
    req = urllib.request.Request(
        f"https://api.elevenlabs.io/v1/text-to-speech/{voice}?output_format=pcm_{RATE}",
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
            voice = MERCHANT_VOICE_ID if event.startswith("merchant_") else VOICE_ID
            jobs.append((os.path.join(root, "revenge", f"bark_{event}_{i}.wav"), text, voice))
    for msg, text in MESSAGES.items():
        jobs.append((os.path.join(root, "load", "XA", f"msg_{msg.replace('.', '_')}.wav"), text, VOICE_ID))

    for path, text, voice in jobs:
        if os.path.exists(path) and not force:
            continue
        write_wav(path, tts(key, text, voice))
        print(f"{os.path.relpath(path, root)}  {text}")


if __name__ == "__main__":
    main()
