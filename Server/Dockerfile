# ============================================================================
# IranCoJ Voice Relay — container image (now with music streaming)
#
# BUILD:  docker build -t irancoj-voice-relay .
# RUN:    see docker-compose.yml. Mounts:
#   /data/players.json  (ro)  team routing
#   /data/keys          (rw)  persistent keypair
#   /data/music         (rw)  put .mp3 files here
#   /data/music_cmd.txt (rw)  write commands: play <f> | loop <f> | folder | stop | skip
# ============================================================================

# ---- build stage ----
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev libsodium-dev libopus-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY voicerelay_hs.c relay_music.c relay_music.h minimp3.h ./
# Link libsodium + libopus + pthread. (Dynamic link; runtime installs the .so's.)
RUN gcc -O2 voicerelay_hs.c relay_music.c -I. \
        $(pkg-config --cflags libsodium opus 2>/dev/null) \
        -o voicerelay \
        $(pkg-config --libs libsodium opus 2>/dev/null || echo -lsodium -lopus) -lpthread

# ---- runtime stage ----
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        libsodium23 libopus0 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /data
COPY --from=build /src/voicerelay /usr/local/bin/voicerelay
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENV VOICE_PORT=40000 \
    PLAYERS_JSON=/data/players.json \
    KEY_DIR=/data/keys \
    MUSIC_DIR=/data/music \
    MUSIC_CMD=/data/music_cmd.txt

EXPOSE 40000/udp
VOLUME ["/data/keys"]
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]