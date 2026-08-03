#!/bin/sh
# IranCoJ voice relay entrypoint.
# - Ensures the key directory exists (keypair persists there for a stable pubkey).
# - Optionally imports a provided key (RELAY_KEY_FILE mount) or config.
# - Prints the pinnable public key clearly, then runs the relay.
set -e

: "${VOICE_PORT:=40000}"
: "${PLAYERS_JSON:=/data/players.json}"
: "${KEY_DIR:=/data/keys}"
: "${RELAY_KEY_FILE:=${KEY_DIR}/relay_key.bin}"

mkdir -p "${KEY_DIR}"

export PLAYERS_JSON
export RELAY_KEY_FILE

echo "=================================================================="
echo " IranCoJ Voice Relay"
echo "  port         : ${VOICE_PORT}/udp"
echo "  players.json : ${PLAYERS_JSON}"
echo "  key file     : ${RELAY_KEY_FILE}"
if [ -f "${PLAYERS_JSON}" ]; then
    echo "  players.json : FOUND"
else
    echo "  players.json : NOT FOUND yet (team routing off until the extractor writes it)"
fi
echo "=================================================================="

# The relay prints its relay_pubkey on startup. With TOFU clients you don't need
# to distribute it; for strict pinning, copy that line into clients' voice.cfg.
exec /usr/local/bin/voicerelay "${VOICE_PORT}"
