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
import subprocess
import sys
import urllib.request
import wave

VOICE_ID = "SOYHLrjzK2X1ezoPC6cr"     # "Harry - Fierce Warrior" (ElevenLabs stock voice)
MERCHANT_VOICE_ID = "N2lVS1w4EtoT3dr4eOWO"  # "Callum - Husky Trickster"
VILLAIN_VOICE_ID = "5PWbsfogbLtky5sxqtBz"   # "Kayla - Evil Witch Villian", the voice on the radio
ANNOUNCER_VOICE_ID = "M5E055lOUxMi0kJpGyE9" # "Gravel Midnight - Deep Character Voice"
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
    # Harry's rage (pc_revenge.c) and kill streaks.
    "rage": [
        "[roaring] Now I'm angry!",
        "[screaming] You took my daughter!",
        "[shouts] Come get some!",
        "[furious] My turn!",
    ],
    "rage_salty": [
        "[roaring] Alright, you sons of bitches!",
    ],
    "rage_end": [
        "[panting] Okay... okay. Breathe.",
        "[exhales] Whew. That felt good.",
    ],
    "running": [
        "[panting] I'm a writer, not a marathon runner!",
        "[out of breath] Should've... taken up jogging... before this vacation.",
        "[wheezing] My cardio... is a crime.",
        "[panting] Okay... where am I even running to?",
        "[gasping] Note to self... buy a car.",
        "[panting] This town is way bigger on foot.",
    ],
    "running_salty": [
        "[panting] Holy shit, this town is huge.",
        "[wheezing] Damn it... my legs are on fire.",
    ],
    "streak": [
        "[laughing] Who's next? Anybody?",
        "[smug] I'm on fire.",
        "[cocky] And they say writers can't fight.",
        "[chuckles] I could do this all day.",
    ],
    # Harry answering the voice on the radio.
    "radio_reply": [
        "[annoyed] Lady, get off my radio.",
        "[sarcastic] Great. A talk show.",
        "[growls] Keep talking. I'm coming for you next.",
        "[dryly] Who gave you this frequency?",
        "[shouts] Send them! I've got bullets!",
        "[muttering] Should've bought a walkie-talkie.",
    ],
    "radio_reply_salty": [
        "[shouts] Shut the hell up!",
        "[annoyed] Lady, piss off.",
    ],
    # The voice on the radio (VILLAIN_VOICE_ID, radio-filtered).
    "radio": [
        "[sinister] Harry... Harry Mason. You're going the wrong way.",
        "[mocking laugh] Run, little writer. Run.",
        "[whispers] They can smell you, Harry.",
        "[cruel] Your daughter cries for you. Isn't that sweet?",
        "[amused] More of my children are coming to play.",
        "[sneering] You can't kill what this town keeps making.",
        "[singsong] Ready or not, Harry...",
        "[cold] I hear every bullet you fire.",
    ],
    "radio_salty": [
        "[mocking] Oh, Harry. You poor, stupid bastard.",
    ],
    # The merchant selling the big guns.
    "merchant_weapon": [
        "[impressed] Ooh, now you're talkin', stranger.",
        "[chuckles] Careful... she bites.",
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

# Announcer stings (ANNOUNCER_VOICE_ID), played over everything as UI sounds:
# revenge/sting_<name>.wav.
STINGS = {
    "double": "[booming] Double kill!",
    "triple": "[booming] Triple kill!",
    "rampage": "[booming, intense] Rampage!",
    "unstoppable": "[booming, intense] Unstoppable!",
    "revenge": "[booming, epic] Harry's... REVENGE!",
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


def radio_filter(path):
    """Make a clean take sound like it is coming through Harry's pocket radio."""
    tmp = path + ".tmp.wav"
    subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-y", "-i", path,
         "-filter_complex",
         "[0:a]highpass=f=350,lowpass=f=3200,acrusher=bits=8:mode=log:aa=1,volume=1.6[v];"
         "anoisesrc=color=white:amplitude=0.035:r=%d[n];"
         "[v][n]amix=inputs=2:duration=first:normalize=0" % RATE,
         "-ac", "1", "-ar", str(RATE), "-sample_fmt", "s16", tmp],
        check=True)
    os.replace(tmp, path)


def main():
    key = os.environ.get("ELEVENLABS_KEY")
    if not key:
        sys.exit("ELEVENLABS_KEY is not set")
    force = "--force" in sys.argv
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "gamedata")

    jobs = []
    for event, lines in BARKS.items():
        for i, text in enumerate(lines, 1):
            if event.startswith("merchant_"):
                voice = MERCHANT_VOICE_ID
            elif event == "radio" or event == "radio_salty":
                voice = VILLAIN_VOICE_ID
            else:
                voice = VOICE_ID
            jobs.append((os.path.join(root, "revenge", f"bark_{event}_{i}.wav"), text, voice))
    for name, text in STINGS.items():
        jobs.append((os.path.join(root, "revenge", f"sting_{name}.wav"), text, ANNOUNCER_VOICE_ID))
    for msg, text in MESSAGES.items():
        jobs.append((os.path.join(root, "load", "XA", f"msg_{msg.replace('.', '_')}.wav"), text, VOICE_ID))

    for path, text, voice in jobs:
        if os.path.exists(path) and not force:
            continue
        write_wav(path, tts(key, text, voice))
        if voice == VILLAIN_VOICE_ID:
            radio_filter(path)
        print(f"{os.path.relpath(path, root)}  {text}")


if __name__ == "__main__":
    main()
