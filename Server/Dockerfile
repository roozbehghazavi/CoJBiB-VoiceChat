# ============================================================================
# IranCoJ Voice Relay — container image
#
# Builds the libsodium-based UDP voice relay and runs it as a service. The relay
# reads players.json (name->team, mode) for team-based routing, and persists its
# long-term keypair in a mounted volume so the pinned public key is stable.
#
# BUILD:  docker build -t irancoj-voice-relay .
# RUN:    see docker-compose.yml (recommended) or:
#   docker run -d --name coj-voice \
#     -p 40000:40000/udp \
#     -v /path/to/players.json:/data/players.json:ro \
#     -v coj-voice-keys:/data/keys \
#     irancoj-voice-relay
# ============================================================================

# ---- build stage ----
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev libsodium-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY voicerelay_hs.c .
# Static-link libsodium so the runtime image needs no dev packages.
RUN gcc -O2 voicerelay_hs.c -o voicerelay \
        $(pkg-config --cflags --libs libsodium 2>/dev/null || echo -lsodium)

# ---- runtime stage ----
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsodium23 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /data
COPY --from=build /src/voicerelay /usr/local/bin/voicerelay
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Config via env (see entrypoint.sh):
ENV VOICE_PORT=40000 \
    PLAYERS_JSON=/data/players.json \
    KEY_DIR=/data/keys

EXPOSE 40000/udp
# Persist the relay keypair here so the pinned pubkey stays stable across restarts.
VOLUME ["/data/keys"]

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
