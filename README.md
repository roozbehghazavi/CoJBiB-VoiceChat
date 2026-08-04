# Call of Juarez: Bound in Blood — Voice Chat Revival

**Bring real in-game voice chat back to Call of Juarez: Bound in Blood — encrypted, team-aware, and dead simple for players to use.**

---

## What is this?

Call of Juarez: Bound in Blood originally shipped with in-game voice chat, but it relied on Microsoft's old DirectPlay Voice and the long-dead GameSpy online service. On modern Windows, that system is gone — the voice feature has been broken for years.

This mod brings voice chat back, rebuilt from the ground up with modern technology. Hold a key, talk to your teammates, and actually coordinate again — just like the game was meant to be played. It works on today's Windows (7, 8, 10, 11), sounds clean, and requires almost nothing from players: **drop in one file and play.**

Whether you're storming a town in Manhunt or dueling in Shootout, you can finally hear your posse.

---

## Features at a glance

- 🎙️ **Real in-game voice** — press a key to talk, no alt-tabbing, no separate program to run.
- 👥 **Team-aware** — in team modes you talk to your own team; the enemy can't hear your callouts.
- 📢 **All-talk key** — a separate key to talk to *everyone* when you want to (great for banter or organizing matches).
- 🔇 **Mute others** — one key to silence everyone if you need quiet, with a sound cue so you know it's on.
- 🔊 **Clear, modern audio** — powered by the Opus codec (the same tech used by Discord), with smooth playback even on imperfect connections.
- 🔐 **Encrypted & private** — your voice is encrypted end-to-end to the server. Nobody on the network can listen in.
- 🛡️ **Protected against tampering** — built-in safeguards stop anyone from spoofing or hijacking voice.
- 🌍 **Works anywhere** — tested across continents with crystal-clear quality.
- 🔌 **Auto-connect** — the moment you join a server that supports voice, you're connected. Leave, and it disconnects. Switch servers, and it follows you.
- ♻️ **Rock-solid reconnection** — if the connection drops mid-match, it quietly reconnects. On servers without voice support, it stops trying so it never wastes your resources.
- 🪶 **Featherweight** — a single small file, no installers, no dependencies, no account required.

---

## For Players

### Controls

| Key | Action |
|-----|--------|
| **V** (hold) | Talk to your **team** |
| **B** (hold) | Talk to **everyone** (all-talk) |
| **M** (press) | Mute / unmute everyone else |

In free-for-all modes (Shootout and Wanted) there are no teams, so **V** and **B** both talk to everyone.

### Installing (it's this easy)

1. Download the voice chat file (`dinput8.dll`) and the included sound-effects folder.
2. Copy them into your Call of Juarez: Bound in Blood game folder (where the game's `.exe` is). The sounds go in the `CoJ2\Data\Sounds` folder that comes with the download.
3. Launch the game and join a server that has voice enabled.
4. Hold **V** and start talking!

That's it. No setup, no configuration, no account. The mod automatically figures out which server you joined and connects your voice for you. When you leave, it disconnects. When you join a different server, it follows along.

### Do I need this if I just want to play normally?

The mod is completely optional and only does something on servers that run the voice service. On any other server it stays quiet and out of the way. It won't interfere with your game.

### Is it safe?

Yes. Your voice is **encrypted** the entire way to the server, so nobody snooping on the network can hear you. The mod also protects against common attacks — nobody can impersonate a server to capture your voice, and nobody can inject fake audio. The first time you connect to a server, the mod remembers that server's secure identity and warns you if it ever changes unexpectedly.

### Sound cues

You'll hear short sounds so you always know what's happening:
- A **connect** sound when voice links up on a server.
- A **disconnect** sound when it drops.
- **Mute / unmute** sounds when you toggle hearing others with **M**.

You can even swap these out for your own sound files if you like — they live in the `CoJ2\Data\Sounds` folder.

---

## For Server Owners

Want voice chat on your server? You run a small, self-contained **voice service** alongside your game server. Players who have the mod will connect to it automatically. Players who don't are unaffected.

### How it works (the short version)

Your game server already knows who's playing and which team they're on. The voice service reads that information and uses it to route voice correctly — teammates hear each other, enemies don't, and everyone hears the all-talk channel. When someone switches teams, voice follows within about a second.

Because routing is based on **player names** (not network addresses), it works perfectly even when several players share the same internet connection — like friends at the same house or a gaming café.

### What you deploy

The voice service comes as a ready-to-run package (a Docker container) plus a small helper that reads your server's current players and game mode. Setup is a few commands:

1. Start the voice service (it generates its own secure key automatically).
2. Run the helper so it publishes the current players and mode.
3. Open one network port so players can reach the voice service.

Full step-by-step instructions are included with the download.

### Runs anywhere your server runs

The helper that feeds player/team info to the voice service works whether your dedicated server runs:
- inside a **Docker container** (the recommended, easiest setup),
- on **plain Linux**, or
- on **Windows**.

### Security for server owners

Each server automatically gets its own unique secure identity — you don't share keys with anyone, and you don't coordinate anything. Players' apps learn your server's identity on first connect and protect against impersonation from then on. If you want the strongest possible guarantee, you can optionally hand players a tiny config file to lock your server's identity in from the very first connection.

---

## Voice routing rules (how team chat behaves)

| Game mode | Voice behavior |
|-----------|----------------|
| **Wild West Legends** (team) | You hear your **own team** only |
| **Manhunt** (team) | You hear your **own team** only |
| **Posse** (team) | You hear your **own team** only |
| **Wanted** (free-for-all) | **Everyone** hears everyone |
| **Shootout** (free-for-all) | **Everyone** hears everyone |

- The **all-talk** key (**B**) lets you reach everyone even in team modes.
- Players sitting in spectator/AFK don't hear or transmit team voice (so no accidental leaks).
- Team changes are reflected almost instantly.

---

## Technical highlights (for the curious)

You don't need to know any of this to use the mod, but for those interested:

- **Audio codec:** Opus at 48 kHz — excellent quality at low bandwidth, with built-in resilience to packet loss.
- **Smoothing:** a jitter buffer keeps audio smooth even when the network isn't perfect.
- **Encryption:** modern authenticated encryption (X25519 key exchange + XChaCha20-Poly1305), with replay protection and anti-tampering.
- **Architecture:** players connect to a lightweight relay running on the game server's host; the relay handles secure routing by team.
- **Compatibility:** a single, self-contained file that works on Windows 7 through 11 with no runtime dependencies to install.

The original game's voice system was reverse-engineered to confirm it could not be revived on modern systems, and this modern replacement was built to take its place — no dependence on any dead service.

---

## Frequently asked questions

**Does this give me an unfair advantage?**
No. It only restores the voice chat the game originally had. Team voice stays within your team, exactly as intended.

**Will it get me banned?**
It's a voice chat mod for community servers — it doesn't touch gameplay, aiming, or scoring. But always follow the rules of the specific server you play on.

**Do I need to run any extra program?**
No. The voice runs inside the game itself. Just drop in the file and play.

**What if a server doesn't support voice?**
Nothing happens — the mod stays quiet and doesn't affect your game.

**Can I use my own sounds?**
Yes. Replace the files in the `CoJ2\Data\Sounds` folder with your own (keep the same file names).

**Is my voice recorded or sent anywhere else?**
No. Voice goes only to the server you're playing on, encrypted the whole way. There's no account, no tracking, no third party.

---

## Credits & thanks

Built by Thomas (Roozbeh) for the Call of Juarez community, to bring one of the game's lost features back to life. Special thanks to everyone who tested across different countries and connections.

Saddle up and call out your shots, partner. 🤠

---

*Call of Juarez: Bound in Blood is a trademark of its respective owners. This is an unofficial community mod and is not affiliated with or endorsed by the game's publisher or developer.*
