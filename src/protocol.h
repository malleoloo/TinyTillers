#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
//  Il mondo e' INFINITO su X e Z, diviso in chunk 16 x WORLD_Y x 16.
//  Solo l'altezza (Y) e' limitata.
//  Layout dei blocchi in un chunk: blocks[x][z][y]  (le colonne sono contigue)
// ---------------------------------------------------------------------------
constexpr int WORLD_Y = 96;
constexpr int CHUNK_SIZE = 16;
constexpr int CHUNK_SHIFT = 4;                                   // log2(CHUNK_SIZE)
constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * WORLD_Y;  // 8192 byte
constexpr int RENDER_DISTANCE = 8;                               // in chunk

constexpr int MAX_PLAYERS = 2;
constexpr uint16_t SERVER_PORT = 25565;

constexpr float SPAWN_X = 0.5f;
constexpr float SPAWN_Z = 0.5f;

constexpr int MAX_STACK = 64;                                   // oggetti per slot

// Giorno/notte: 24000 "tick" di Minecraft = 20 minuti reali. Il tempo e' deciso dal server.
constexpr double DAY_LENGTH_SECONDS = 1200.0;
constexpr double START_TIME_SECONDS = DAY_LENGTH_SECONDS * 1000.0 / 24000.0;   // mattina

enum Block : uint8_t
{
    AIR = 0,
    GRASS,
    DIRT,
    STONE,
    WOOD,
    SAND,
    LEAVES,
    WOOD_HORIZ_X,
    WOOD_HORIZ_Z,
    FALLING_SAND   // solo messaggio di rete: "inizia animazione", mai salvato nel mondo
};

enum MessageType : uint8_t
{
    MSG_CHUNK_DATA = 1,   // server -> client: un chunk (compresso RLE)
    MSG_BLOCK_CHANGE,
    MSG_PLAYER_STATE,
    MSG_PLAYER_JOINED,
    MSG_PLAYER_LEFT,
    MSG_CHUNK_REQUEST,    // client -> server: "mandami questo chunk"
    MSG_WELCOME,          // server -> client: il tuo id + ora del mondo
    MSG_TIME_SYNC,        // server -> client: ora del mondo (cielo e nuvole uguali per tutti)
    MSG_ITEM_STATE,       // server -> client: oggetto a terra creato/spostato
    MSG_ITEM_REMOVE,      // server -> client: oggetto raccolto o sparito
    MSG_DROP_REQUEST,     // client -> server: "butto questo oggetto" (tasto Q)
    MSG_ITEM_PICKUP       // client -> server: "voglio raccogliere questo oggetto"
};

#pragma pack(push, 1)

struct BlockChangeMessage
{
    uint8_t type;
    int32_t x, y, z;
    uint8_t block;
};

struct PlayerStateMessage
{
    uint8_t type;
    uint8_t playerId;
    float x, y, z;
    float yaw, pitch;
};

struct PlayerJoinedMessage
{
    uint8_t type;
    uint8_t playerId;
    float x, y, z;
};

struct PlayerLeftMessage
{
    uint8_t type;
    uint8_t playerId;
};

struct ChunkRequestMessage
{
    uint8_t type;
    int32_t cx, cz;
};

// Header di MSG_CHUNK_DATA, subito seguito dai dati RLE del chunk
struct ChunkDataHeader
{
    uint8_t type;
    int32_t cx, cz;
};

struct WelcomeMessage
{
    uint8_t type;
    uint8_t playerId;
    double worldTime;      // secondi dall'inizio del mondo
};

struct TimeSyncMessage
{
    uint8_t type;
    double worldTime;
};

struct ItemStateMessage
{
    uint8_t type;
    uint32_t id;
    uint8_t block;
    uint8_t count;
    float x, y, z;         // y = base dell'oggetto
};

struct ItemRemoveMessage
{
    uint8_t type;
    uint32_t id;
    uint8_t pickerId;      // 255 = sparito da solo
    uint8_t block;
    uint8_t count;
};

struct DropRequestMessage
{
    uint8_t type;
    uint8_t block;
    uint8_t count;
};

struct ItemPickupMessage
{
    uint8_t type;
    uint32_t id;
};

#pragma pack(pop)

constexpr uint32_t WORLD_FILE_MAGIC = 0x54494E59;
constexpr uint32_t WORLD_FILE_VERSION = 3;   // v3: mondo alto 96 blocchi (i vecchi chunk vengono rigenerati)

// ---------------------------------------------------------------------------
//  Helper per coordinate (corretti anche per coordinate negative)
// ---------------------------------------------------------------------------
inline int ChunkOf(int v) { return v >> CHUNK_SHIFT; }          // coordinata blocco -> chunk
inline int LocalOf(int v) { return v & (CHUNK_SIZE - 1); }      // coordinata blocco -> locale 0..15

inline int64_t ChunkKey(int cx, int cz)
{
    return ((int64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}

// ---------------------------------------------------------------------------
//  Compressione RLE (coppie: lunghezza, valore). Un chunk tipico passa da
//  8192 byte a ~2 KB, quindi il caricamento online e' molto piu' veloce.
// ---------------------------------------------------------------------------
inline void RleEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out)
{
    out.clear();
    size_t i = 0;
    while (i < n)
    {
        uint8_t v = src[i];
        size_t run = 1;
        while (i + run < n && src[i + run] == v && run < 255) run++;
        out.push_back((uint8_t)run);
        out.push_back(v);
        i += run;
    }
}

inline bool RleDecode(const uint8_t* src, size_t len, uint8_t* dst, size_t n)
{
    if (len % 2 != 0) return false;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 2)
    {
        size_t run = src[i];
        if (run == 0 || o + run > n) return false;
        std::memset(dst + o, src[i + 1], run);
        o += run;
    }
    return o == n;
}