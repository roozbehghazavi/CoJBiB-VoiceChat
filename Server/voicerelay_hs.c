// voicerelay_hs.c — IranCoJ voice relay, step 4a: X25519 handshake.
//
// Adds a key-exchange handshake with each client (libsodium crypto_kx), and
// derives per-client session keys. In 4a we DO NOT encrypt voice yet — voice is
// still forwarded in cleartext. This step just proves the handshake works and
// keys are established. 4b will turn on AEAD.
//
// The relay has a LONG-TERM keypair saved to relay_key.bin (generated on first
// run). Its PUBLIC key is printed at startup — copy that into the client so it
// can PIN the relay (MITM protection).
//
// Packet types (1st byte):
//   0x00 heartbeat        (register only)
//   0x01 handshake HELLO  (client->relay: client ephemeral pubkey)
//   0x02 voice            (header+payload; cleartext in 4a)
// Relay replies to 0x01 with a WELCOME (0x01) carrying relay long-term pubkey +
// relay session (ephemeral) pubkey.
//
// BUILD (Linux VPS):
//   sudo apt install libsodium-dev
//   gcc voicerelay_hs.c -o voicerelay_hs -lsodium
// RUN:
//   ./voicerelay_hs 40000
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <sodium.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib,"ws2_32.lib")
typedef int socklen_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#define MAX_CLIENTS    32
#define CLIENT_TIMEOUT 10
#define PKT_MAX        2048

#define PT_HEARTBEAT 0x00
#define PT_HANDSHAKE 0x01
#define PT_VOICE     0x02

// Voice header is 15 bytes (magic4+id4+seq2+ts4+flags1). Keep in sync with client.
#define HDR_LEN 15
#define NPUB crypto_aead_xchacha20poly1305_ietf_NPUBBYTES   // 24
#define MAC  crypto_aead_xchacha20poly1305_ietf_ABYTES      // 16
#define FLAG_ALLTALK 0x01     // header flag: sender wants all-talk (skip team filter)
#define HDR_FLAGS_OFF 14      // flags byte offset within VoiceHeader (magic4+id4+seq2+ts4)

// ---- Team map (server-authoritative, read from players.json) ----
// We read the scoreboard's players.json and build name->team plus the mode id.
// Identity is by PLAYER NAME (NAT-proof: players behind one IP have distinct
// names). The voice client presents its name in the handshake.
#define TEAM_MAP_MAX 64
#define VNAME_MAX 32
typedef struct { char name[VNAME_MAX]; int team; } TeamEntry;
static TeamEntry g_teamMap[TEAM_MAP_MAX];
static int g_teamCount = 0;
static int g_mode = -1;            // mode_id; -1 unknown
static const char* g_jsonPath = "players.json";

// team modes require same-team; single modes are all-talk
static int isTeamMode(int m) { return (m == 0 || m == 1 || m == 4); }

// Minimal, tolerant scan of players.json for mode_id and each player's name+team.
// Not a full JSON parser: relies on the scoreboard's stable key names.
static void loadTeamMap(void) {
    FILE* f = fopen(g_jsonPath, "rb");
    if (!f) return;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    if (n == 0) return; buf[n] = 0;

    int newMode = -1, newCount = 0;
    // mode_id
    char* mp = strstr(buf, "\"mode_id\"");
    if (mp) { mp = strchr(mp, ':'); if (mp) newMode = atoi(mp + 1); }

    // Walk each "name": "X" ... "team": T  (team appears before name in the
    // scoreboard dict, so search both directions within a small window).
    char* p = buf;
    while ((p = strstr(p, "\"name\"")) != NULL && newCount < TEAM_MAP_MAX) {
        char* colon = strchr(p, ':'); if (!colon) { p += 5;continue; }
        char* q1 = strchr(colon, '\"'); if (!q1) { p += 5;continue; }
        char* q2 = strchr(q1 + 1, '\"'); if (!q2) { p += 5;continue; }
        int len = (int)(q2 - (q1 + 1)); if (len <= 0 || len >= VNAME_MAX) { p = q2 + 1;continue; }
        char nm[VNAME_MAX]; memcpy(nm, q1 + 1, len); nm[len] = 0;

        // Find "team" belonging to THIS player: search FORWARD from the name, but
        // stop before the NEXT player's "name" so we never grab a neighbour's team.
        // (Scoreboard format is {"name":"X","team":N,...} - team follows name.)
        int team = 0;
        char* nextName = strstr(q2 + 1, "\"name\"");
        char* t = strstr(q2 + 1, "\"team\"");
        if (t && (!nextName || t < nextName)) {
            char* c = strchr(t, ':'); if (c) team = atoi(c + 1);
        }

        if (team == 1 || team == 2) {
            strncpy(g_teamMap[newCount].name, nm, VNAME_MAX - 1);
            g_teamMap[newCount].name[VNAME_MAX - 1] = 0;
            g_teamMap[newCount].team = team; newCount++;
        }
        p = q2 + 1;
    }
    g_teamCount = newCount; g_mode = newMode;
}

// team for a given player name; 0 if unknown/AFK.
static int teamForName(const char* name) {
    if (!name || !name[0]) return 0;
    for (int i = 0;i < g_teamCount;i++) if (strcmp(g_teamMap[i].name, name) == 0) return g_teamMap[i].team;
    return 0;
}

// Voice framing sizes (must match the client's VoiceHeader)
#define HDR_LEN   15                                   // sizeof(VoiceHeader): 4+4+2+4+1
#define NONCE_LEN crypto_aead_xchacha20poly1305_ietf_NPUBBYTES  // 24
#define MAC_LEN   crypto_aead_xchacha20poly1305_ietf_ABYTES     // 16

typedef struct {
    struct sockaddr_in addr;
    time_t   lastSeen;
    int      active;
    int      hasKeys;                               // handshake completed?
    unsigned char rx[crypto_kx_SESSIONKEYBYTES];    // key to DECRYPT from this client
    unsigned char tx[crypto_kx_SESSIONKEYBYTES];    // key to ENCRYPT to this client
    // Anti-replay sliding window (per sender), keyed on header seq (16-bit).
    int      rpInit;        // window initialised?
    uint16_t rpMax;         // highest seq accepted so far
    uint64_t rpMask;        // bitmap of the 64 seqs at/below rpMax (bit0 = rpMax)
    char     name[VNAME_MAX];// player name presented in handshake (identity for teams)
} Client;

static Client g_clients[MAX_CLIENTS];

// relay long-term keypair
static unsigned char g_relayPk[crypto_kx_PUBLICKEYBYTES];
static unsigned char g_relaySk[crypto_kx_SECRETKEYBYTES];

static int sameAddr(const struct sockaddr_in* a, const struct sockaddr_in* b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}
static int findClient(const struct sockaddr_in* from) {
    for (int i = 0;i < MAX_CLIENTS;i++)
        if (g_clients[i].active && sameAddr(&g_clients[i].addr, from)) return i;
    return -1;
}
static int touchClient(const struct sockaddr_in* from, time_t now) {
    int idx = findClient(from);
    if (idx >= 0) { g_clients[idx].lastSeen = now; return idx; }
    for (int i = 0;i < MAX_CLIENTS;i++) {
        if (!g_clients[i].active) {
            memset(&g_clients[i], 0, sizeof(Client));
            g_clients[i].addr = *from; g_clients[i].lastSeen = now; g_clients[i].active = 1;
            printf("[relay] client JOINED %s:%d (slot %d)\n",
                inet_ntoa(from->sin_addr), ntohs(from->sin_port), i); fflush(stdout);
            return i;
        }
    }
    return -1;
}
static void expireClients(time_t now) {
    for (int i = 0;i < MAX_CLIENTS;i++)
        if (g_clients[i].active && now - g_clients[i].lastSeen > CLIENT_TIMEOUT) {
            printf("[relay] client LEFT %s:%d (slot %d)\n",
                inet_ntoa(g_clients[i].addr.sin_addr), ntohs(g_clients[i].addr.sin_port), i);
            fflush(stdout);
            g_clients[i].active = 0;
        }
}

// Load or generate the relay long-term keypair.
static void loadOrCreateKeys(void) {
    FILE* f = fopen("relay_key.bin", "rb");
    if (f) {
        size_t n1 = fread(g_relayPk, 1, sizeof(g_relayPk), f);
        size_t n2 = fread(g_relaySk, 1, sizeof(g_relaySk), f);
        fclose(f);
        if (n1 == sizeof(g_relayPk) && n2 == sizeof(g_relaySk)) {
            printf("[relay] loaded key from relay_key.bin\n"); return;
        }
    }
    crypto_kx_keypair(g_relayPk, g_relaySk);
    f = fopen("relay_key.bin", "wb");
    if (f) { fwrite(g_relayPk, 1, sizeof(g_relayPk), f); fwrite(g_relaySk, 1, sizeof(g_relaySk), f); fclose(f); }
    printf("[relay] generated new key -> relay_key.bin\n");
}
static void printHex(const char* label, const unsigned char* b, int n) {
    printf("%s", label); for (int i = 0;i < n;i++) printf("%02x", b[i]); printf("\n");
}

// Returns 1 if seq is fresh (accept + record), 0 if replay/too-old (reject).
// 16-bit seq with wraparound; window of 64.
static int replayCheck(Client* c, uint16_t seq) {
    if (!c->rpInit) { c->rpInit = 1; c->rpMax = seq; c->rpMask = 1ULL; return 1; }
    // distance forward (seq newer than rpMax) with wrap
    uint16_t fwd = (uint16_t)(seq - c->rpMax);
    uint16_t bwd = (uint16_t)(c->rpMax - seq);
    if (fwd != 0 && fwd < 0x8000) {
        // seq is newer -> shift window forward
        if (fwd >= 64) c->rpMask = 1ULL;               // window fully past; reset
        else          c->rpMask = (c->rpMask << fwd) | 1ULL;
        c->rpMax = seq;
        return 1;
    }
    else if (bwd < 0x8000) {
        // seq is older or equal
        if (bwd == 0) return 0;                         // exact replay of rpMax
        if (bwd >= 64) return 0;                        // too old -> reject
        uint64_t bit = 1ULL << bwd;
        if (c->rpMask & bit) return 0;                  // already seen -> replay
        c->rpMask |= bit;                              // record
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("Usage: %s <listenPort>\n", argv[0]); return 1; }
    int port = atoi(argv[1]);
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
    if (sodium_init() < 0) { printf("sodium_init failed\n"); return 1; }
    loadOrCreateKeys();
    printf("[relay] ==== PIN THIS RELAY PUBLIC KEY IN THE CLIENT ====\n");
    printHex("[relay] relay_pubkey = ", g_relayPk, sizeof(g_relayPk));
    printf("[relay] =================================================\n");

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local; memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_ANY); local.sin_port = htons((unsigned short)port);
    if (bind(s, (struct sockaddr*)&local, sizeof(local)) != 0) { printf("bind failed %d\n", port); return 1; }
    printf("[relay] listening UDP %d\n", port); fflush(stdout);

    unsigned char buf[PKT_MAX];
    time_t lastExpire = time(NULL);
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(s, (char*)buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
        time_t now = time(NULL);
        if (n >= 1) {
            unsigned char type = buf[0];
            int idx = touchClient(&from, now);
            if (idx < 0) { /* table full */ }
            else if (type == PT_HANDSHAKE) {
                // HELLO: [0x01][client ephemeral pubkey(32)]
                if (n >= 1 + crypto_kx_PUBLICKEYBYTES) {
                    const unsigned char* clientPk = buf + 1;
                    // optional name after pubkey: [len][bytes]
                    int off = 1 + crypto_kx_PUBLICKEYBYTES;
                    if (n > off) {
                        int nl = buf[off]; off++;
                        if (nl > 0 && nl < VNAME_MAX && off + nl <= n) {
                            memcpy(g_clients[idx].name, buf + off, nl);
                            g_clients[idx].name[nl] = 0;
                            printf("[relay] slot %d name=\"%s\"\n", idx, g_clients[idx].name);
                        }
                    }
                    // Relay makes an ephemeral session keypair for THIS handshake.
                    unsigned char sesPk[crypto_kx_PUBLICKEYBYTES], sesSk[crypto_kx_SECRETKEYBYTES];
                    crypto_kx_keypair(sesPk, sesSk);
                    // Relay = server side of crypto_kx (rx=decrypt-from-client, tx=encrypt-to-client).
                    if (crypto_kx_server_session_keys(g_clients[idx].rx, g_clients[idx].tx,
                        sesPk, sesSk, clientPk) == 0) {
                        g_clients[idx].hasKeys = 1;
                        printf("[relay] handshake OK with slot %d\n", idx); fflush(stdout);
                    }
                    // WELCOME: [0x01][relay long-term pk(32)][relay session pk(32)]
                    unsigned char out[1 + crypto_kx_PUBLICKEYBYTES * 2];
                    out[0] = PT_HANDSHAKE;
                    memcpy(out + 1, g_relayPk, crypto_kx_PUBLICKEYBYTES);
                    memcpy(out + 1 + crypto_kx_PUBLICKEYBYTES, sesPk, crypto_kx_PUBLICKEYBYTES);
                    sendto(s, (char*)out, sizeof(out), 0, (struct sockaddr*)&from, sizeof(from));
                    sodium_memzero(sesSk, sizeof(sesSk));
                }
            }
            else if (type == PT_VOICE) {
                // 4b: decrypt from sender, re-encrypt to each recipient (Model A).
                // Wire: [0x02][VoiceHeader H][nonce 24][ciphertext+tag]
                // H is authenticated as AAD.
                if (!g_clients[idx].hasKeys) { /* no keys yet, drop */ }
                else if (n >= 1 + HDR_LEN + NONCE_LEN + MAC_LEN) {
                    const unsigned char* H = buf + 1;
                    const unsigned char* nonce = buf + 1 + HDR_LEN;
                    const unsigned char* ct = buf + 1 + HDR_LEN + NONCE_LEN;
                    unsigned long long ctLen = n - (1 + HDR_LEN + NONCE_LEN);

                    unsigned char plain[PKT_MAX];
                    unsigned long long plainLen = 0;
                    // decrypt with sender's rx key; H (header) as AAD.
                    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
                        plain, &plainLen, NULL,
                        ct, ctLen, H, HDR_LEN, nonce, g_clients[idx].rx) == 0) {
                        // Re-encrypt to every OTHER client that has keys.
                        for (int i = 0;i < MAX_CLIENTS;i++) {
                            if (!g_clients[i].active || i == idx || !g_clients[i].hasKeys) continue;
                            unsigned char out[1 + HDR_LEN + NONCE_LEN + PKT_MAX + MAC_LEN];
                            out[0] = PT_VOICE;
                            memcpy(out + 1, H, HDR_LEN);                 // same header (cleartext, AAD)
                            unsigned char* on = out + 1 + HDR_LEN;
                            randombytes_buf(on, NONCE_LEN);            // fresh nonce per recipient
                            unsigned char* oc = on + NONCE_LEN;
                            unsigned long long oclen = 0;
                            crypto_aead_xchacha20poly1305_ietf_encrypt(
                                oc, &oclen, plain, plainLen, H, HDR_LEN, NULL, on, g_clients[i].tx);
                            int total = 1 + HDR_LEN + NONCE_LEN + (int)oclen;
                            sendto(s, (char*)out, total, 0, (struct sockaddr*)&g_clients[i].addr, sizeof(g_clients[i].addr));
                        }
                    }
                    // else: auth failed (tampered/replayed/garbage) -> silently drop.
                }
            }
            // PT_HEARTBEAT: registration only (touchClient already did it).
        }
        if (now - lastExpire >= 1) { expireClients(now); loadTeamMap(); lastExpire = now; }
    }
    CLOSESOCK(s);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}