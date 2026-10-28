#include <enet/enet.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "protocol.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
//  Configurazione
// ---------------------------------------------------------------------------
constexpr uint32_t WORLD_SEED = 1337u;
constexpr int SAND_LEVEL = 30;                       // sotto questa quota la superficie e' sabbia
constexpr int KEEP_DISTANCE = RENDER_DISTANCE + 5;  // chunk tenuti in RAM attorno ai giocatori
constexpr int MAX_CHUNK_COORD = 1 << 24;
constexpr int MAX_BLOCK_COORD = 1 << 28;
static const char* WORLD_DIR = "server_world";

static volatile std::sig_atomic_t running = 1;
static void OnSignal(int) { running = 0; }

// ---------------------------------------------------------------------------
//  Chunk
// ---------------------------------------------------------------------------
struct ServerChunk
{
    int cx = 0, cz = 0;
    uint8_t blocks[CHUNK_SIZE][CHUNK_SIZE][WORLD_Y];
    bool unsaved = false;
};

static std::unordered_map<int64_t, ServerChunk*> chunks;

struct Player
{
    bool active = false;
    float x = SPAWN_X;
    float y = 12.0f;
    float z = SPAWN_Z;
    float yaw = 0.0f;
    float pitch = 0.0f;
};

static std::array<Player, MAX_PLAYERS> players;

// ---------------------------------------------------------------------------
//  Generazione del terreno (infinita e deterministica)
// ---------------------------------------------------------------------------
static float Hash01(int x, int z)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + WORLD_SEED * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFFu) / 16777216.0f;
}

static float ValueNoise(float x, float z)
{
    int xi = (int)floorf(x), zi = (int)floorf(z);
    float fx = x - (float)xi, fz = z - (float)zi;
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sz = fz * fz * (3.0f - 2.0f * fz);

    float a = Hash01(xi, zi),     b = Hash01(xi + 1, zi);
    float c = Hash01(xi, zi + 1), d = Hash01(xi + 1, zi + 1);

    float top = a + (b - a) * sx;
    float bot = c + (d - c) * sx;
    return top + (bot - top) * sz;
}

static uint32_t HashU32(int x, int z, uint32_t salt);

static float SmoothStep(float a, float b, float v)
{
    float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Somma di ottave di rumore, normalizzata 0..1
static float Fbm(float x, float z, int octaves, float offset)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i)
    {
        sum += ValueNoise(x + offset + i * 37.1f, z + offset - i * 19.7f) * amp;
        norm += amp;
        amp *= 0.5f;
        x *= 2.0f;
        z *= 2.0f;
    }
    return sum / norm;
}

// Terreno stile Minecraft Beta: grandi regioni alte/basse, pianure quasi piatte,
// zone collinari e, ogni tanto, montagne con creste alte fino a ~85 blocchi.
int TerrainHeight(int xi, int zi)
{
    const float x = (float)xi, z = (float)zi;

    float continent = Fbm(x / 320.0f, z / 320.0f, 2, 11.0f);
    float base = 34.0f + (continent - 0.5f) * 22.0f;

    float hillMask = SmoothStep(0.42f, 0.62f, ValueNoise(x / 170.0f + 300.0f, z / 170.0f + 300.0f));
    float detail = Fbm(x / 40.0f, z / 40.0f, 4, 100.0f) - 0.5f;
    float hills = detail * 2.0f * (2.5f + 13.0f * hillMask);          // pianura: +-1, colline: +-6

    float mountMask = SmoothStep(0.52f, 0.70f, ValueNoise(x / 230.0f + 700.0f, z / 230.0f + 700.0f));
    float r = fabsf(2.0f * Fbm(x / 90.0f, z / 90.0f, 3, 900.0f) - 1.0f);
    float ridge = std::clamp(1.0f - r * 2.2f, 0.0f, 1.0f);
    float mountains = mountMask * (8.0f + 34.0f * ridge * ridge);

    float h = base + hills + mountains;
    if (h < 29.0f) h = 29.0f + (h - 29.0f) * 0.35f;                    // vallate poco profonde
    return std::clamp((int)lroundf(h), 3, WORLD_Y - 10);
}

// In alta quota la montagna e' roccia nuda (con un bordo irregolare verso l'erba)
static bool BareRock(int x, int z, int h)
{
    return h >= 60 + (int)(HashU32(x, z, 31) % 9u);
}

// ---------------------------------------------------------------------------
//  Alberi: deterministici, quindi identici qualunque chunk venga generato per primo.
//  Il mondo e' diviso in celle da 7x7 blocchi; ogni cella puo' contenere al massimo
//  un albero. Cosi' un albero a cavallo tra due chunk viene disegnato correttamente
//  da entrambi, senza dipendere dall'ordine di generazione.
// ---------------------------------------------------------------------------
constexpr int TREE_CELL = 7;

static uint32_t HashU32(int x, int z, uint32_t salt)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (WORLD_SEED + salt * 2654435761u) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return h;
}

static int FloorDiv(int a, int b)
{
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

struct Tree
{
    int x, z;       // posizione del tronco
    int ground;     // quota della superficie sotto l'albero
    int trunk;      // altezza del tronco
};

static bool TreeInCell(int cellX, int cellZ, Tree& t)
{
    uint32_t h = HashU32(cellX, cellZ, 11);
    int ox = cellX * TREE_CELL + 1 + (int)(h % 5u);
    int oz = cellZ * TREE_CELL + 1 + (int)((h >> 8) % 5u);

    if (std::abs(ox) < 6 && std::abs(oz) < 6) return false;   // zona di spawn libera

    // Zone di foresta (fitte) alternate a pianure (con pochi alberi)
    float forest = ValueNoise(ox / 96.0f + 500.0f, oz / 96.0f + 500.0f);
    float chance = forest > 0.55f ? 0.75f : 0.10f;
    if (((h >> 12) & 0x3FFu) / 1024.0f >= chance) return false;

    int ground = TerrainHeight(ox, oz);
    if (ground <= SAND_LEVEL || BareRock(ox, oz, ground)) return false;                    // niente alberi sulla sabbia

    t = { ox, oz, ground, 4 + (int)((h >> 24) % 3u) };         // tronco alto 4-6
    return true;
}

static void PlaceTrees(ServerChunk* c)
{
    const int baseX = c->cx * CHUNK_SIZE;
    const int baseZ = c->cz * CHUNK_SIZE;
    const int minX = baseX - 2, maxX = baseX + CHUNK_SIZE + 1;   // le chiome sporgono di 2 blocchi
    const int minZ = baseZ - 2, maxZ = baseZ + CHUNK_SIZE + 1;

    auto put = [&](int wx, int y, int wz, uint8_t block, bool overwrite) {
        int lx = wx - baseX, lz = wz - baseZ;
        if (lx < 0 || lx >= CHUNK_SIZE || lz < 0 || lz >= CHUNK_SIZE || y < 0 || y >= WORLD_Y) return;
        uint8_t& b = c->blocks[lx][lz][y];
        if (overwrite || b == AIR) b = block;
    };

    for (int cellX = FloorDiv(minX, TREE_CELL); cellX <= FloorDiv(maxX, TREE_CELL); ++cellX)
    {
        for (int cellZ = FloorDiv(minZ, TREE_CELL); cellZ <= FloorDiv(maxZ, TREE_CELL); ++cellZ)
        {
            Tree t;
            if (!TreeInCell(cellX, cellZ, t)) continue;
            if (t.x < minX || t.x > maxX || t.z < minZ || t.z > maxZ) continue;

            const int top = t.ground + t.trunk;

            // Chioma stile Minecraft: due strati 5x5, uno 3x3 e una croce in cima (angoli casuali)
            for (int y = top - 2; y <= top + 1; ++y)
            {
                int r = (y >= top) ? 1 : 2;
                for (int dx = -r; dx <= r; ++dx)
                {
                    for (int dz = -r; dz <= r; ++dz)
                    {
                        bool corner = (std::abs(dx) == r && std::abs(dz) == r);
                        if (corner)
                        {
                            if (y == top + 1) continue;
                            if (HashU32(t.x + dx, t.z + dz + y * 131, 23) & 1u) continue;
                        }
                        put(t.x + dx, y, t.z + dz, LEAVES, false);
                    }
                }
            }

            // Tronco (sovrascrive le foglie)
            for (int y = t.ground + 1; y <= top; ++y)
                put(t.x, y, t.z, WOOD, true);
        }
    }
}

static void GenerateChunk(ServerChunk* c)
{
    std::memset(c->blocks, 0, sizeof(c->blocks));

    for (int lx = 0; lx < CHUNK_SIZE; ++lx)
    {
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        {
            int wx = c->cx * CHUNK_SIZE + lx;
            int wz = c->cz * CHUNK_SIZE + lz;
            int h = TerrainHeight(wx, wz);
            bool beach = h <= SAND_LEVEL;
            bool rock = !beach && BareRock(wx, wz, h);

            for (int y = 0; y <= h; ++y)
            {
                if (y == h) c->blocks[lx][lz][y] = beach ? SAND : (rock ? STONE : GRASS);
                else if (y >= h - 3) c->blocks[lx][lz][y] = beach ? SAND : (rock ? STONE : DIRT);
                else c->blocks[lx][lz][y] = STONE;
            }
        }
    }

    PlaceTrees(c);
}

// ---------------------------------------------------------------------------
//  Salvataggio / caricamento: un file per chunk modificato
// ---------------------------------------------------------------------------
static std::string ChunkPath(int cx, int cz)
{
    return std::string(WORLD_DIR) + "/c_" + std::to_string(cx) + "_" + std::to_string(cz) + ".bin";
}

static bool SaveChunk(const ServerChunk& c)
{
    std::string path = ChunkPath(c.cx, c.cz);
    std::string tmp = path + ".tmp";

    std::vector<uint8_t> rle;
    RleEncode(&c.blocks[0][0][0], CHUNK_VOLUME, rle);

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        uint32_t hdr[2] = { WORLD_FILE_MAGIC, WORLD_FILE_VERSION };
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        f.write(reinterpret_cast<const char*>(rle.data()), (std::streamsize)rle.size());
        f.close();
        if (!f) return false;
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

static bool LoadChunkFromDisk(ServerChunk* c)
{
    std::ifstream f(ChunkPath(c->cx, c->cz), std::ios::binary);
    if (!f) return false;

    uint32_t hdr[2] = {};
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    if (!f || hdr[0] != WORLD_FILE_MAGIC || hdr[1] != WORLD_FILE_VERSION) return false;

    std::vector<uint8_t> rle((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return RleDecode(rle.data(), rle.size(), &c->blocks[0][0][0], CHUNK_VOLUME);
}

static ServerChunk* GetChunk(int cx, int cz)
{
    int64_t key = ChunkKey(cx, cz);
    auto it = chunks.find(key);
    if (it != chunks.end()) return it->second;

    auto* c = new ServerChunk();
    c->cx = cx;
    c->cz = cz;
    if (!LoadChunkFromDisk(c)) GenerateChunk(c);
    chunks[key] = c;
    return c;
}

static void SaveDirtyChunks()
{
    for (auto& kv : chunks)
    {
        ServerChunk* c = kv.second;
        if (c->unsaved && SaveChunk(*c)) c->unsaved = false;
    }
}

// ---------------------------------------------------------------------------
//  Accesso ai blocchi (coordinate mondo)
// ---------------------------------------------------------------------------
static uint8_t GetBlock(int x, int y, int z)
{
    ServerChunk* c = GetChunk(ChunkOf(x), ChunkOf(z));
    return c->blocks[LocalOf(x)][LocalOf(z)][y];
}

static void SetBlock(int x, int y, int z, uint8_t block)
{
    ServerChunk* c = GetChunk(ChunkOf(x), ChunkOf(z));
    c->blocks[LocalOf(x)][LocalOf(z)][y] = block;
    c->unsaved = true;
}

static bool IsSolidAt(int x, int y, int z)
{
    if (y < 0) return true;
    if (y >= WORLD_Y) return false;
    return GetBlock(x, y, z) != AIR;
}

// ---------------------------------------------------------------------------
//  Rete
// ---------------------------------------------------------------------------
void SendPacket(ENetPeer* peer, const void* data, size_t size, enet_uint8 channel = 0)
{
    ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (packet && enet_peer_send(peer, channel, packet) != 0)
        enet_packet_destroy(packet);
}

void Broadcast(ENetHost* server, const void* data, size_t size, enet_uint8 channel = 0)
{
    ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (packet) enet_host_broadcast(server, channel, packet);
}

void BroadcastBlock(ENetHost* server, int x, int y, int z, uint8_t block)
{
    BlockChangeMessage m { MSG_BLOCK_CHANGE, x, y, z, block };
    Broadcast(server, &m, sizeof(m));
}

void SendChunk(ENetPeer* peer, int cx, int cz)
{
    ServerChunk* c = GetChunk(cx, cz);

    static std::vector<uint8_t> rle;
    RleEncode(&c->blocks[0][0][0], CHUNK_VOLUME, rle);

    std::vector<uint8_t> packet(sizeof(ChunkDataHeader) + rle.size());
    ChunkDataHeader header { MSG_CHUNK_DATA, cx, cz };
    std::memcpy(packet.data(), &header, sizeof(header));
    std::memcpy(packet.data() + sizeof(header), rle.data(), rle.size());
    SendPacket(peer, packet.data(), packet.size());
}

// ---------------------------------------------------------------------------
//  Sabbia che cade
// ---------------------------------------------------------------------------
struct FallingSand
{
    int x, z;
    float y;
    float vy;
};
static std::vector<FallingSand> fallingSands;

void CheckSandColumn(ENetHost* server, int x, int z)
{
    ServerChunk* c = GetChunk(ChunkOf(x), ChunkOf(z));
    int lx = LocalOf(x), lz = LocalOf(z);

    for (int y = 1; y < WORLD_Y; ++y)
    {
        if (c->blocks[lx][lz][y] == SAND && c->blocks[lx][lz][y - 1] == AIR)
        {
            c->blocks[lx][lz][y] = AIR;
            c->unsaved = true;
            BroadcastBlock(server, x, y, z, AIR);
            BroadcastBlock(server, x, y, z, FALLING_SAND);
            fallingSands.push_back({ x, z, (float)y, 0.0f });
        }
    }
}

void TickFallingSands(ENetHost* server)
{
    static uint32_t last = enet_time_get();
    uint32_t now = enet_time_get();
    float dt = std::min((now - last) / 1000.0f, 0.05f);
    last = now;

    if (fallingSands.empty() || dt <= 0.0f) return;

    for (size_t i = 0; i < fallingSands.size(); )
    {
        FallingSand& s = fallingSands[i];
        float oldY = s.y;
        s.vy += 30.0f * dt;
        s.y  -= s.vy * dt;

        int landing = -1;
        for (int cy = (int)floorf(oldY) - 1; cy >= (int)floorf(s.y); --cy)
        {
            if (IsSolidAt(s.x, cy, s.z)) { landing = cy + 1; break; }
        }

        if (landing < 0) { ++i; continue; }
        while (landing < WORLD_Y && GetBlock(s.x, landing, s.z) != AIR) ++landing;

        if (landing < WORLD_Y)
        {
            SetBlock(s.x, landing, s.z, SAND);
            BroadcastBlock(server, s.x, landing, s.z, SAND);
        }
        fallingSands.erase(fallingSands.begin() + i);
    }
}

// ---------------------------------------------------------------------------
//  Ora del mondo (ciclo giorno/notte): decisa dal server e mandata a tutti,
//  cosi' cielo, sole, luna e nuvole sono identici per ogni giocatore.
// ---------------------------------------------------------------------------
static double worldTime = START_TIME_SECONDS;

static std::string TimePath() { return std::string(WORLD_DIR) + "/time.bin"; }

static void LoadWorldTime()
{
    std::ifstream f(TimePath(), std::ios::binary);
    double t = 0.0;
    if (f.read(reinterpret_cast<char*>(&t), sizeof(t)) && std::isfinite(t) && t >= 0.0) worldTime = t;
}

static void SaveWorldTime()
{
    std::ofstream f(TimePath(), std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(&worldTime), sizeof(worldTime));
}

// ---------------------------------------------------------------------------
//  Oggetti a terra (blocchi buttati con Q o lasciati da un blocco rotto).
//  Il server simula la fisica, decide chi li raccoglie e manda lo stato a tutti.
//  Tutti i messaggi degli oggetti viaggiano sul canale 1, quindi arrivano in ordine.
// ---------------------------------------------------------------------------
struct DropItem
{
    uint32_t id;
    uint8_t block, count;
    float x, y, z;          // y = base dell'oggetto
    float vx, vy, vz;
    float age;
    float pickupDelay;
    bool dirty;
};
static std::vector<DropItem> items;
static uint32_t nextItemId = 1;
constexpr float ITEM_LIFETIME = 300.0f;      // 5 minuti, come Minecraft
constexpr size_t MAX_ITEMS = 256;

static uint32_t rngState = 2463534242u;
static float Rand01()
{
    rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
    return (rngState & 0xFFFFFFu) / 16777216.0f;
}

static void SendItemState(ENetHost* server, const DropItem& it, ENetPeer* only = nullptr)
{
    ItemStateMessage m { MSG_ITEM_STATE, it.id, it.block, it.count, it.x, it.y, it.z };
    if (only) SendPacket(only, &m, sizeof(m), 1);
    else Broadcast(server, &m, sizeof(m), 1);
}

static void RemoveItem(ENetHost* server, size_t index, uint8_t pickerId)
{
    const DropItem& it = items[index];
    ItemRemoveMessage m { MSG_ITEM_REMOVE, it.id, pickerId, it.block, it.count };
    Broadcast(server, &m, sizeof(m), 1);
    items.erase(items.begin() + (std::ptrdiff_t)index);
}

static void SpawnItem(ENetHost* server, uint8_t block, uint8_t count, float x, float y, float z,
                      float vx, float vy, float vz, float pickupDelay)
{
    if (items.size() >= MAX_ITEMS) RemoveItem(server, 0, 255);
    DropItem it { nextItemId++, block, count, x, y, z, vx, vy, vz, 0.0f, pickupDelay, false };
    items.push_back(it);
    SendItemState(server, it);
}

// Cosa lascia un blocco quando viene rotto
static uint8_t DropOf(uint8_t b)
{
    switch (b)
    {
        case AIR: case FALLING_SAND: return AIR;
        case GRASS: return DIRT;
        case WOOD_HORIZ_X: case WOOD_HORIZ_Z: return WOOD;
        default: return b;
    }
}

static void SpawnBlockDrop(ENetHost* server, int x, int y, int z, uint8_t oldBlock)
{
    uint8_t d = DropOf(oldBlock);
    if (d == AIR) return;
    SpawnItem(server, d, 1, x + 0.5f, y + 0.1f, z + 0.5f,
              (Rand01() - 0.5f) * 2.0f, 2.5f, (Rand01() - 0.5f) * 2.0f, 0.5f);
}

static void SpawnPlayerDrop(ENetHost* server, int playerId, uint8_t block, uint8_t count)
{
    const Player& p = players[playerId];
    float cp = cosf(p.pitch), sp = sinf(p.pitch);
    float dx = sinf(p.yaw) * cp, dy = sp, dz = cosf(p.yaw) * cp;
    SpawnItem(server, block, count, p.x + dx * 0.4f, p.y + 1.3f, p.z + dz * 0.4f,
              dx * 6.5f, dy * 6.5f + 1.5f, dz * 6.5f, 2.0f);
}

static void StepItem(DropItem& it, float dt)
{
    const float ox = it.x, oy = it.y, oz = it.z;

    // Se un blocco e' finito addosso all'oggetto, lo spinge in superficie
    for (int g = 0; g < 4 && IsSolidAt((int)floorf(it.x), (int)floorf(it.y + 0.05f), (int)floorf(it.z)); ++g)
        it.y = floorf(it.y + 0.05f) + 1.0f;

    it.vy = std::max(it.vy - 16.0f * dt, -30.0f);
    float ny = it.y + it.vy * dt;
    bool ground = false;
    const int fx = (int)floorf(it.x), fz = (int)floorf(it.z);
    if (it.vy <= 0.0f && IsSolidAt(fx, (int)floorf(ny), fz)) { ny = floorf(ny) + 1.0f; it.vy = 0.0f; ground = true; }
    else if (it.vy > 0.0f && IsSolidAt(fx, (int)floorf(ny + 0.25f), fz)) { it.vy = 0.0f; ny = it.y; }
    it.y = ny;

    const int ty = (int)floorf(it.y + 0.1f);
    float nx = it.x + it.vx * dt;
    if (IsSolidAt((int)floorf(nx), ty, (int)floorf(it.z))) it.vx = -it.vx * 0.3f; else it.x = nx;
    float nz = it.z + it.vz * dt;
    if (IsSolidAt((int)floorf(it.x), ty, (int)floorf(nz))) it.vz = -it.vz * 0.3f; else it.z = nz;

    float damp = expf((ground ? -12.0f : -0.3f) * dt);
    it.vx *= damp; it.vz *= damp;
    if (ground && fabsf(it.vx) < 0.05f && fabsf(it.vz) < 0.05f) { it.vx = 0.0f; it.vz = 0.0f; }

    if (fabsf(it.x - ox) + fabsf(it.y - oy) + fabsf(it.z - oz) > 1.0e-4f) it.dirty = true;
}

static void TickItems(ENetHost* server)
{
    static uint32_t last = enet_time_get();
    static float sendTimer = 0.0f;
    uint32_t now = enet_time_get();
    float dt = std::min((now - last) / 1000.0f, 0.05f);
    last = now;

    if (items.empty() || dt <= 0.0f) return;

    sendTimer += dt;
    bool send = sendTimer >= 0.05f;      // massimo 20 aggiornamenti al secondo
    if (send) sendTimer = 0.0f;

    for (size_t i = 0; i < items.size(); )
    {
        DropItem& it = items[i];
        it.age += dt;
        if (it.age > ITEM_LIFETIME) { RemoveItem(server, i, 255); continue; }

        StepItem(it, dt);
        if (send && it.dirty) { SendItemState(server, it); it.dirty = false; }
        ++i;
    }
}

static void HandleItemPickup(ENetHost* server, int playerId, uint32_t itemId)
{
    const Player& p = players[playerId];
    for (size_t i = 0; i < items.size(); ++i)
    {
        const DropItem& it = items[i];
        if (it.id != itemId) continue;
        if (it.age < it.pickupDelay) return;

        float dx = it.x - p.x, dy = (it.y + 0.15f) - (p.y + 0.9f), dz = it.z - p.z;
        if (dx * dx + dy * dy + dz * dz > 3.0f * 3.0f) return;

        RemoveItem(server, i, (uint8_t)playerId);
        return;
    }
}

static void TickWorld(ENetHost* server)
{
    static uint32_t last = enet_time_get();
    static double syncTimer = 0.0;
    uint32_t now = enet_time_get();
    double dt = (now - last) / 1000.0;
    last = now;

    worldTime += dt;
    syncTimer += dt;
    if (syncTimer >= 2.0)
    {
        syncTimer = 0.0;
        TimeSyncMessage m { MSG_TIME_SYNC, worldTime };
        Broadcast(server, &m, sizeof(m), 1);
    }

    TickFallingSands(server);
    TickItems(server);
}

// ---------------------------------------------------------------------------
//  Scaricamento dei chunk lontani da tutti i giocatori
// ---------------------------------------------------------------------------
static void UnloadFarChunks()
{
    for (auto it = chunks.begin(); it != chunks.end(); )
    {
        ServerChunk* c = it->second;
        bool keep = false;

        for (const Player& p : players)
        {
            if (!p.active) continue;
            int pcx = ChunkOf((int)floorf(p.x));
            int pcz = ChunkOf((int)floorf(p.z));
            if (std::abs(c->cx - pcx) <= KEEP_DISTANCE && std::abs(c->cz - pcz) <= KEEP_DISTANCE)
            {
                keep = true;
                break;
            }
        }

        if (!keep)
        {
            for (const FallingSand& s : fallingSands)
                if (ChunkOf(s.x) == c->cx && ChunkOf(s.z) == c->cz) { keep = true; break; }

            if (!keep)
                for (const DropItem& it : items)
                    if (ChunkOf((int)floorf(it.x)) == c->cx && ChunkOf((int)floorf(it.z)) == c->cz) { keep = true; break; }
        }

        if (keep || (c->unsaved && !SaveChunk(*c)))
        {
            ++it;
            continue;
        }

        delete c;
        it = chunks.erase(it);
    }
}

// ---------------------------------------------------------------------------
//  Log in console: [ora] messaggio
// ---------------------------------------------------------------------------
static void Log(const std::string& msg)
{
    std::time_t t = std::time(nullptr);
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    std::cout << "[" << buf << "] " << msg << std::endl;
}

static std::string PeerAddress(const ENetPeer* peer)
{
    char ip[64] = "?";
    enet_address_get_host_ip(&peer->address, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(peer->address.port);
}

static int ActivePlayers()
{
    int n = 0;
    for (const Player& p : players) if (p.active) ++n;
    return n;
}

int GetPlayerId(ENetPeer* peer)
{
    if (!peer || !peer->data) return -1;
    return *static_cast<int*>(peer->data);
}

void SendPlayerState(ENetHost* server, int id)
{
    const Player& p = players[id];
    PlayerStateMessage msg { MSG_PLAYER_STATE, static_cast<uint8_t>(id), p.x, p.y, p.z, p.yaw, p.pitch };
    Broadcast(server, &msg, sizeof(msg), 1);
}

int main(int argc, char** argv)
{
    if (enet_initialize() != 0) return 1;

    std::error_code ec;
    fs::create_directories(WORLD_DIR, ec);

    LoadWorldTime();
    // Facoltativo: "server.exe 13000" avvia il mondo a quel "tick" (0 = alba, 6000 = mezzogiorno, 18000 = mezzanotte)
    if (argc > 1) worldTime = std::atof(argv[1]) / 24000.0 * DAY_LENGTH_SECONDS;

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    ENetAddress address {};
    address.host = ENET_HOST_ANY;
    address.port = SERVER_PORT;

    ENetHost* server = enet_host_create(&address, MAX_PLAYERS, 2, 0, 0);
    if (!server)
    {
        enet_deinitialize();
        return 1;
    }

    std::cout << "TinyTillers Server avviato sulla porta UDP " << SERVER_PORT
              << " (mondo infinito, chunk generati su richiesta).\n";

    uint32_t lastSave = enet_time_get();
    uint32_t lastUnload = lastSave;

    while (running)
    {
        TickWorld(server);

        uint32_t now = enet_time_get();
        if (now - lastSave >= 5000) { SaveDirtyChunks(); SaveWorldTime(); lastSave = now; }
        if (now - lastUnload >= 10000) { UnloadFarChunks(); lastUnload = now; }

        ENetEvent event {};

        while (enet_host_service(server, &event, 10) > 0)
        {
            TickWorld(server);
            switch (event.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                {
                    int id = -1;
                    for (int i = 0; i < MAX_PLAYERS; ++i)
                        if (!players[i].active) { id = i; break; }

                    Log("Tentativo di connessione da " + PeerAddress(event.peer));

                    if (id == -1)
                    {
                        Log("Connessione RIFIUTATA (server pieno): " + PeerAddress(event.peer));
                        enet_peer_disconnect_now(event.peer, 0);
                        break;
                    }

                    players[id] = {};
                    players[id].active = true;
                    players[id].x = SPAWN_X;
                    players[id].z = SPAWN_Z;
                    players[id].y = (float)TerrainHeight(0, 0) + 2.0f;

                    event.peer->data = new int(id);

                    Log("Giocatore " + std::to_string(id + 1) + " CONNESSO da " + PeerAddress(event.peer) +
                        "  (online: " + std::to_string(ActivePlayers()) + "/" + std::to_string(MAX_PLAYERS) + ")");

                    WelcomeMessage welcome { MSG_WELCOME, static_cast<uint8_t>(id), worldTime };
                    SendPacket(event.peer, &welcome, sizeof(welcome), 1);
                    for (const DropItem& it : items) SendItemState(server, it, event.peer);

                    PlayerJoinedMessage joined { MSG_PLAYER_JOINED, static_cast<uint8_t>(id), players[id].x, players[id].y, players[id].z };
                    Broadcast(server, &joined, sizeof(joined));

                    for (int other = 0; other < MAX_PLAYERS; ++other)
                        if (other != id && players[other].active) SendPlayerState(server, other);
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE:
                {
                    int id = GetPlayerId(event.peer);
                    if (id >= 0 && event.packet->dataLength > 0)
                    {
                        uint8_t type = event.packet->data[0];
                        if (type == MSG_CHUNK_REQUEST && event.packet->dataLength == sizeof(ChunkRequestMessage))
                        {
                            ChunkRequestMessage msg {};
                            std::memcpy(&msg, event.packet->data, sizeof(msg));
                            if (msg.cx > -MAX_CHUNK_COORD && msg.cx < MAX_CHUNK_COORD &&
                                msg.cz > -MAX_CHUNK_COORD && msg.cz < MAX_CHUNK_COORD)
                            {
                                SendChunk(event.peer, msg.cx, msg.cz);
                            }
                        }
                        else if (type == MSG_BLOCK_CHANGE && event.packet->dataLength == sizeof(BlockChangeMessage))
                        {
                            BlockChangeMessage msg {};
                            std::memcpy(&msg, event.packet->data, sizeof(msg));
                            if (msg.y >= 0 && msg.y < WORLD_Y &&
                                msg.x > -MAX_BLOCK_COORD && msg.x < MAX_BLOCK_COORD &&
                                msg.z > -MAX_BLOCK_COORD && msg.z < MAX_BLOCK_COORD &&
                                msg.block <= WOOD_HORIZ_Z)
                            {
                                uint8_t oldBlock = GetBlock(msg.x, msg.y, msg.z);
                                SetBlock(msg.x, msg.y, msg.z, msg.block);
                                if (msg.block == AIR && oldBlock != AIR) SpawnBlockDrop(server, msg.x, msg.y, msg.z, oldBlock);
                                Broadcast(server, &msg, sizeof(msg));
                                CheckSandColumn(server, msg.x, msg.z);
                            }
                        }
                        else if (type == MSG_DROP_REQUEST && event.packet->dataLength == sizeof(DropRequestMessage))
                        {
                            DropRequestMessage msg {};
                            std::memcpy(&msg, event.packet->data, sizeof(msg));
                            if (msg.block >= GRASS && msg.block <= WOOD_HORIZ_Z && msg.count >= 1 && msg.count <= MAX_STACK)
                                SpawnPlayerDrop(server, id, msg.block, msg.count);
                        }
                        else if (type == MSG_ITEM_PICKUP && event.packet->dataLength == sizeof(ItemPickupMessage))
                        {
                            ItemPickupMessage msg {};
                            std::memcpy(&msg, event.packet->data, sizeof(msg));
                            HandleItemPickup(server, id, msg.id);
                        }
                        else if (type == MSG_PLAYER_STATE && event.packet->dataLength == sizeof(PlayerStateMessage))
                        {
                            PlayerStateMessage msg {};
                            std::memcpy(&msg, event.packet->data, sizeof(msg));
                            if (std::isfinite(msg.x) && std::isfinite(msg.y) && std::isfinite(msg.z) &&
                                std::fabs(msg.x) < 1.0e7f && std::fabs(msg.z) < 1.0e7f)
                            {
                                players[id].x = msg.x; players[id].y = msg.y; players[id].z = msg.z;
                                players[id].yaw = msg.yaw; players[id].pitch = msg.pitch;
                                SendPlayerState(server, id);
                            }
                        }
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                {
                    int id = GetPlayerId(event.peer);
                    if (id >= 0)
                    {
                        players[id].active = false;
                        Log("Giocatore " + std::to_string(id + 1) + " DISCONNESSO (" + PeerAddress(event.peer) +
                            ")  (online: " + std::to_string(ActivePlayers()) + "/" + std::to_string(MAX_PLAYERS) + ")");
                        delete static_cast<int*>(event.peer->data);
                        event.peer->data = nullptr;
                        PlayerLeftMessage msg { MSG_PLAYER_LEFT, static_cast<uint8_t>(id) };
                        Broadcast(server, &msg, sizeof(msg));
                    }
                    break;
                }
                default: break;
            }
        }
    }

    std::cout << "Chiusura server, salvataggio del mondo...\n";
    SaveDirtyChunks();
    SaveWorldTime();
    for (auto& kv : chunks) delete kv.second;
    chunks.clear();

    enet_host_destroy(server);
    enet_deinitialize();
    return 0;
}