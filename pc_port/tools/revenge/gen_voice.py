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
        "[smug] Tell your friends. Actually, don't.",
        "[deadpan] One star. Would not recommend.",
        "[cocky] Somebody's getting a bad review.",
        "[dryly] That's going in the book.",
        "[satisfied] Nap time, ugly.",
        "[chuckles] Should've RSVP'd no.",
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
        "[groans] Okay, that one I felt.",
        "[pained] I bruise like a peach!",
        "[wincing] Ow! Not the face!",
        "[strained] I'm not done yet...",
        "[panting] Just a scratch. Keep moving.",
        "[grunts] Still standing.",
        "[groans] Is that all you've got?",
        "[groans] My chiropractor is going to hear about this.",
        "[wincing] Okay. Ow. Noted.",
        "[through gritted teeth] That's going to leave a mark. And a lawsuit.",
    ],
    "reload": [
        "[muttering] Click click. Love that sound.",
        "[calm] New clip, same bad attitude.",
        "[muttering] Reloading.",
        "[quickly] Come on, come on...",
        "[calm] Fresh rounds.",
        "[annoyed] Hold that thought, I'm reloading.",
        "[indignant] Very rude to attack a man while he's reloading.",
    ],
    "area": [
        "[sighs] Oh look. More fog. Shocking.",
        "[dryly] Charming. Real estate's gotta be cheap here.",
        "[muttering] If I find a gift shop, I'm burning it down.",
        "[determined] Cheryl... I'm coming.",
        "[low] Whatever's in here... it's not leaving.",
        "[quietly] Let's finish this.",
        "[angry] This town took my daughter. Now I take it back.",
        "[sighs] Great. Another creepy hallway. Love that for me.",
        "[dryly] Note to self. Never vacation here again.",
        "[sarcastic] Lovely fog. Real tourist trap.",
    ],
    "ammo": [
        "[pleased] Christmas came early.",
        "[satisfied] Ammo. Good.",
        "I'll take that.",
        "[grim] More rounds. Perfect.",
        "[pleased] Ooh, bullets. Don't mind if I do.",
        "[cheerfully] Free ammo. This town isn't all bad.",
    ],
    "money": [
        "[pleased] Don't mind if I do.",
        "[smug] Monster tax.",
        "[pleased] Cha-ching.",
        "[puzzled] Where do monsters even keep their wallets?",
        "That's going in the Cheryl fund.",
        "[satisfied] Thanks for the tip.",
    ],
    "invader": [
        "[confused] Are you lost, buddy?",
        "[annoyed] Wrong neighbourhood, pal.",
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
        "[spits] Rot in hell, ugly.",
        "[shouts] Hell yes! Who's next?",
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
        "[screaming] You want some? Come get some!",
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
    # Waves (pc_waves.c).
    "wave_start": [
        "[cracks knuckles] Here they come. Let's dance.",
        "[sighs] Oh good. More of you.",
        "[cocky] Round two? I was just warming up.",
        "[determined] Come on then. All of you.",
        "[dryly] Ah. The second act.",
    ],
    "wave_clear": [
        "[exhales] Is that all you've got?",
        "[satisfied] Clean sweep.",
        "[smug] And that's how it's done.",
        "[panting] Okay. Okay. Still alive.",
    ],
    "wave_clear_salty": [
        "[laughing] Hell yeah! Who's next?",
        "[panting] Damn, I'm good.",
    ],
    "rest": [
        "[sighs] Breather. Thank god.",
        "[muttering] Reload, breathe, repeat.",
        "[dryly] Quick, somebody bring me a sandwich.",
        "[exhales] Five minutes. I just need five minutes.",
    ],
    "area_clear": [
        "[satisfied] Neighbourhood's clean. You're welcome.",
        "[smug] Area cleared. I should charge for this.",
        "[cocky] That's this street done. Next.",
    ],
    "area_clear_salty": [
        "[satisfied] Hell of a job, Harry. Hell of a job.",
    ],
    "streak": [
        "[laughing] I'm actually good at this!",
        "[smug] Somebody stop me. Seriously, nobody?",
        "[laughing] Who's next? Anybody?",
        "[smug] I'm on fire.",
        "[cocky] And they say writers can't fight.",
        "[chuckles] I could do this all day.",
    ],
    # Harry answering the voice on the radio.
    "radio_reply": [
        "[indignant] At least my books have a plot!",
        "[offended] Leave my pancakes out of this!",
        "[annoyed] I do not snore!",
        "[dryly] Everybody's a critic.",
        "[shouts] Alan Wake had it easy!",
        "[annoyed] Lady, get off my radio.",
        "[sarcastic] Great. A talk show.",
        "[growls] Keep talking. I'm coming for you next.",
        "[dryly] Who gave you this frequency?",
        "[shouts] Send them! I've got bullets!",
        "[muttering] Should've bought a walkie-talkie.",
    ],
    "radio_reply_salty": [
        "[shouts] Oh, piss off, lady!",
        "[shouts] Shut the hell up!",
        "[annoyed] Lady, piss off.",
    ],
    # The voice on the radio (VILLAIN_VOICE_ID, radio-filtered).
    "radio": [
        "[mocking] Oh, Harry. You're hardly Alan Wake.",
        "[amused] A writer, Harry? Stephen King you are not.",
        "[sneering] I read your last novel, Harry. Even the monsters fell asleep.",
        "[cruel] Your agent called, Harry. He's dropping you.",
        "[mocking] Write a book about it, Harry. Nobody will read that one either.",
        "[amused] Cheryl says your pancakes are terrible.",
        "[singsong] Cheryl likes it here, Harry. We let her stay up past nine.",
        "[mocking] Cheryl tells me you snore. Like a chainsaw.",
        "[sneering] Father of the year, Harry. You lost her in under five minutes.",
        "[amused] Your bedtime stories bore her, Harry. Mine have demons.",
        "[mocking] Running again, Harry? Your cardio is embarrassing.",
        "[sinister] Is that a gun, Harry? How very... compensating.",
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
        "[mocking] Your prose is shit, Harry. And so is your aim.",
        "[mocking] Oh, Harry. You poor, stupid bastard.",
    ],
    # The merchant selling the big guns.
    "merchant_weapon": [
        "[impressed] Ooh, now you're talkin', stranger.",
        "[chuckles] Careful... she bites.",
    ],
    # Harry's scooter (pc_scooter.c).
    "scooter_on": [
        "[delighted] Beep beep, monsters!",
        "[smug] Vespa mode, activated.",
        "[laughing] I'm a novelist on a scooter. Peak dignity.",
        "[shouts] Out of the way, coming through!",
        "[cheerful] Should've rented one of these on day one.",
    ],
    "scooter_on_salty": [
        "[laughing] Hell yeah, wheels!",
    ],
    "scooter_off": [
        "[sighs] Back on foot. Great.",
        "[dryly] Parking's free, at least.",
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
    "merchant_scooter": [
        "[chuckles] A scooter, stranger? Mind the fog. And the monsters.",
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

# First visit to each location this session (pc_revenge.c REVENGE_PLACES maps
# map indices onto these keys): bark_place_<key>_<n>.wav.
PLACES = {
    "oldtown": ["[sighs] Old Silent Hill. Quaint. Murdery, but quaint.",
                "[dryly] Charming town. Great fog. Zero people.",
                "[muttering] If I see one more closed shop, I'm writing a strongly worded review."],
    "cafe": ["[relieved] A cafe. Please tell me there's coffee."],
    "school": ["[groans] Great. School. I hated this the first time.",
               "[dryly] Midwich Elementary. Home of the world's creepiest hall monitors.",
               "[muttering] I'm not here for parent-teacher night, I swear."],
    "school_other": ["[disgusted] Oh, they redecorated. Rust and blood. Bold choice.",
                     "[sighs] Same school, worse plumbing.",
                     "[nervous] Who let the nightmare decorate?"],
    "school_boss": ["[nervous] Okay. Big room. Never a good sign."],
    "church": ["[suspicious] A church. Let's see who's preaching.",
               "[dryly] Quiet. Too quiet. Churchy quiet."],
    "central": ["[determined] Central Silent Hill. Bigger town, bigger targets.",
                "[dryly] Downtown. Still no Starbucks.",
                "[sighs] More streets, more fog, more problems."],
    "police": ["[relieved] A police station. Finally, some backup. Oh. Nope. Empty."],
    "hospital": ["[groans] Lovely. I always wanted surgery from a monster.",
                 "[dryly] Alchemilla Hospital. One star on Yelp, I bet.",
                 "[nervous] Hospitals are creepy on a good day."],
    "hospital_other": ["[disgusted] Hospital, Otherworld edition. Now with extra gore.",
                       "[sighs] I really need to stop falling asleep in these places.",
                       "[muttering] Wheelchairs. Why is it always wheelchairs."],
    "antiques": ["[curious] Green Lion Antiques. Let's go antiquing. With a shotgun."],
    "central_other": ["[groans] And the whole town went rusty. Fantastic.",
                      "[dryly] Ah. Hell's version of downtown."],
    "mall": ["[sarcastic] A mall. Finally, some retail therapy.",
             "[dryly] Nothing says shopping like a giant worm."],
    "sewers": ["[groans] Of course there's a sewer level. There's always a sewer level.",
               "[disgusted] Oh, that smell. That is not a vacation smell.",
               "[muttering] Watch your step, Harry. Watch everything."],
    "resort": ["[dryly] The Resort Area. Five stars for murder.",
               "[sarcastic] Lovely lakeside resort. Do they do a monster discount?"],
    "bar": ["[sighs] Annie's Bar. Pour me a double. Make it a triple."],
    "motel": ["[suspicious] Norman's Motel. I'm not checking in."],
    "resort_other": ["[groans] The resort's gone full nightmare. Checkout's early."],
    "boat": ["[nervous] A boat. In the fog. In this town. Sure."],
    "lighthouse": ["[determined] The lighthouse. Time to end this."],
    "amusement": ["[dryly] An amusement park. I'm not amused.",
                  "[sarcastic] Fun for the whole family. If your family is demons.",
                  "[sighs] I hate clowns. Please no clowns."],
    "nowhere": ["[confused] Where the hell am I? Nowhere. Great, that's helpful.",
                "[determined] This is it. Cheryl, I'm coming.",
                "[dryly] Ah. The final dungeon. Every story has one."],
}

# Picking up an item (Inventory_AddSpecialItem): bark_item_<InvItemId>_1.wav.
ITEMS = {
    64: "[dryly] A lobby key. Riveting.",
    65: "[satisfied] House key. Breaking and entering, legally.",
    66: "[confused] Key of Lion. Somebody here really loves The Wizard of Oz.",
    67: "[dryly] Woodman key. If I meet a talking scarecrow, I'm leaving.",
    68: "[sighs] Scarecrow key. Yep. Definitely Oz. Great.",
    69: "[whispers] Library reserve key. Shh.",
    70: "[dryly] Classroom key. Detention's in session.",
    71: "[reading] K. Gordon. Thanks, K. Gordon, whoever you are.",
    72: "[pleased] Drawbridge key. Finally, a way across.",
    73: "[sighs] Basement key. Nothing good ever happens in basements.",
    74: "[dryly] A storeroom key for the basement. A basement inside a basement. Lovely.",
    75: "[nervous] Examination room key. I'm not taking my shirt off.",
    76: "[satisfied] Antique shop key. Time to go shopping.",
    77: "[groans] Sewer key. Why is it always the sewer?",
    78: "[confused] Key of Ophiel. Who names these things?",
    79: "[dryly] Key of Hagith. Sounds like a sneeze.",
    80: "[muttering] Key of Phaleg. Gesundheit.",
    81: "[sighs] Key of Bethor. Collect all five, I guess.",
    82: "[exasperated] Key of Aratron. Okay, the fancy key thing is getting old.",
    83: "[reading] To school. Thanks for the riveting directions.",
    84: "[dryly] A note about a doghouse. Sure. Why not.",
    85: "[puzzled] A picture card. Is this a key or a trading card?",
    87: "[relieved] Sewer exit key. Best thing I've found all day.",
    88: "[curious] Channeling stone. Feels... spacey.",
    96: "[sniffs] Some kind of chemical. Smells like my college apartment.",
    97: "[impressed] Gold medallion. Finally, some loot.",
    98: "[satisfied] Silver medallion. Second place, but I'll take it.",
    99: "[deadpan] A rubber ball. I'm on a rampage with a bouncy ball.",
    100: "[suspicious] The Flauros. Pointy, glowy, cursed. Perfect.",
    101: "[dryly] An empty plastic bottle. I'm a hero, not a recycling center.",
    102: "[nervous] Unknown red liquid. I am not drinking that.",
    103: "[confused] Plate of Turtle. Alice in Wonderland now? Pick a theme!",
    104: "[sighs] Plate of the Hatter. Mad, like me.",
    105: "[dryly] Plate of the Cat. Somebody really loves Alice.",
    106: "[smug] Plate of the Queen. Off with their heads. On it.",
    107: "[disgusted] A blood pack. Gross. Keeping it anyway.",
    108: "[pleased] Disinfecting alcohol. Party time. Kidding.",
    109: "[satisfied] A lighter. Things are about to get toasty.",
    110: "[curious] A videotape. Hope it's not the cursed kind.",
    112: "[smug] Kaufmann's key. Let's see what the creep is hiding.",
    113: "[reading] A receipt. Even out here they keep receipts.",
    114: "[pleased] Safe key. Here comes the loot.",
    115: "[dryly] A magnet. Fridge art, here I come.",
    116: "[excited] Motorcycle key! Wait. No bike. Damn.",
    117: "[dryly] Bird cage key. For the giant flying monsters, I assume.",
    118: "[satisfied] Pliers. Every hero needs pliers.",
    119: "[satisfied] A screwdriver. Time to fix things. Violently.",
    120: "[pleased] A camera. Say cheese, monsters.",
    121: "[suspicious] Ring of Contract. Did I just marry a cult?",
    122: "[curious] Stone of Time. Can it get me back to Florida?",
    123: "[dryly] Amulet of Solomon. Fancy.",
    124: "[dryly] Crest of Mercury. Shiny cult jewellery.",
    125: "[sighs] An ankh. Egyptian too? This cult has range.",
    126: "[satisfied] Dagger of Melchior. Ooh. Stabby.",
    127: "[dryly] Disk of Ouroboros. A snake eating itself. Relatable.",
    128: "[dryly] Kitchen knife. For when you really want to get close.",
    129: "[satisfied] A steel pipe. Classic.",
    130: "[excited] A rock drill! Oh, this is gonna be messy.",
    132: "[satisfied] Hammer time.",
    133: "[laughing] A chainsaw. Now we're talking. Groovy.",
    134: "[impressed] A katana. I feel like a samurai. A tired, out of shape samurai.",
    135: "[smug] An axe. Heeere's Harry!",
    160: "[satisfied] A handgun. Now it's a fair fight.",
    161: "[pleased] Hunting rifle. Open season, ugly.",
    162: "[cocky] A shotgun. Boomstick. I love you already.",
    163: "[excited] A laser gun? This vacation just got awesome.",
    224: "[relieved] A flashlight. Let there be light.",
    225: "[suspicious] A pocket radio. Why is it hissing at me?",
    226: "[satisfied] Gasoline. Something's getting set on fire.",
}
for _key, _lines in PLACES.items():
    BARKS["place_" + _key] = _lines
for _id, _line in ITEMS.items():
    BARKS["item_%d" % _id] = [_line]

# Announcer stings (ANNOUNCER_VOICE_ID), played over everything as UI sounds:
# revenge/sting_<name>.wav.
STINGS = {
    "double": "[booming] Double kill!",
    "triple": "[booming] Triple kill!",
    "rampage": "[booming, intense] Rampage!",
    "unstoppable": "[booming, intense] Unstoppable!",
    "revenge": "[booming, epic] Harry's... REVENGE!",
    "wave": "[booming] Here they come!",
    "wave_clear": "[booming] Wave cleared!",
    "area_clear": "[booming, epic] Area... cleared!",
    "rank_s": "[booming, excited] S rank! Sensational!",
    "rank_a": "[booming] A rank! Awesome!",
    "rank_b": "[booming] B rank. Not bad.",
    "rank_c": "[unimpressed] C rank. Try harder.",
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

    # Which text each file was recorded from: a list edit that shifts the
    # numbering re-records the files whose line changed, not just new numbers.
    manifest_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "voice_manifest.json")
    manifest = json.load(open(manifest_path)) if os.path.exists(manifest_path) else {}

    for path, text, voice in jobs:
        rel = os.path.relpath(path, root)
        if os.path.exists(path) and not force and manifest.get(rel, text) == text:
            manifest[rel] = text
            continue
        write_wav(path, tts(key, text, voice))
        if voice == VILLAIN_VOICE_ID:
            radio_filter(path)
        manifest[rel] = text
        json.dump(manifest, open(manifest_path, "w"), indent=1, sort_keys=True)
        print(f"{rel}  {text}")
    json.dump(manifest, open(manifest_path, "w"), indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
