# IranCoJ Voice — Server-side (Relay + Extractor)

Modern, encrypted, team-aware voice chat for Call of Juarez: Bound in Blood.
This is the **server side**: a containerized **relay** plus the **extractor** that
feeds it team/mode info. Players just drop one `dinput8.dll` next to their game.

## Pieces

| Component | Runs where | Role |
|-----------|-----------|------|
| **voice-relay** (this image) | its own container | Receives encrypted voice, routes by team, forwards. |
| **voice_extractor.py** | inside the game-server (DS) container | Reads DS memory + mode, writes `players.json`. |
| **dinput8.dll** | each player's game folder | Injected client: auto-connects, encrypts, PTT/mute. |

Data flow:
```
extractor --writes--> players.json --read by--> relay --routes voice--> players
(in DS container)      (shared vol)              (relay container)
```

## Quick start

1. **Run the extractor** in your DS container so it writes `players.json` to a
   shared location (e.g. a mounted volume). Example (inside the DS container):
   ```
   python3 voice_extractor.py --out /shared/players.json --interval 1
   ```
   In the container `127.0.0.1:27632` reaches the DS, so no --query-ip needed.

2. **Point the relay at that file.** In `docker-compose.yml`, set the left side of
   the `players.json` mount to wherever the extractor writes it, then:
   ```
   docker compose up -d --build
   docker compose logs -f          # note the "relay_pubkey = ..." line
   ```

3. **Open UDP 40000** on the server host / firewall (the published port).

4. **Players**: drop `dinput8.dll` in their game folder. It auto-connects voice to
   `<game server IP>:40000`, learns the relay key on first connect (TOFU), and it
   just works. Hold **V** to talk, **M** to mute others.

## Security (keys)

- The relay generates its own keypair on first run, saved in the `coj-voice-keys`
  volume (`/data/keys/relay_key.bin`). The **public key is printed at startup**.
- Clients use **TOFU** (trust-on-first-use): they remember each server's relay key
  and refuse if it later changes (MITM protection). No key distribution needed.
- **Strict pinning (optional):** to remove the first-use trust window, give players
  a `voice.cfg` next to the DLL containing:
  ```
  pin=<the relay_pubkey hex from the logs>
  ```
  Keep the `coj-voice-keys` volume so the key (and thus the pin) stays stable.

## Configuration (env)

| Env | Default | Meaning |
|-----|---------|---------|
| `VOICE_PORT` | `40000` | UDP port the relay listens on (must be published + firewalled). |
| `PLAYERS_JSON` | `/data/players.json` | Path to the extractor's output (mount it). |
| `KEY_DIR` | `/data/keys` | Where the keypair persists (volume). |

## Team routing rules (server-authoritative, NAT-proof)

The relay reads `players.json` (`name -> team`, `mode_id`) and routes by **player
name** presented by the client (read from game memory, unspoofable):

- **Team modes** (WWL=0, Manhunt=1, Posse=4): you only hear your **own team**.
- **Single modes** (Wanted=2, Shootout=3): **everyone** hears everyone.
- **AFK / not in players.json**: hears no one, talks to no one (in team modes).
- Team changes are picked up within ~1s (extractor re-reads memory each cycle).

Because identity is by **name**, players behind the **same NAT/IP** are still
distinguished correctly.

## Files in this image

- `voicerelay_hs.c` — the relay (built statically against libsodium).
- `Dockerfile`, `docker-compose.yml`, `entrypoint.sh` — packaging.

The `dinput8.dll` client and `voice_extractor.py` are distributed separately (the
extractor goes in the DS container; the DLL goes to players).
