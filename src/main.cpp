#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <iostream>
#include <string>

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include "protocol.h"
#include "network.h"

// Dichiariamo qui la funzione del menu esterno senza bisogno di un file .h
std::string RunMainMenu(Font customFont);

constexpr float PLAYER_RADIUS = 0.30f;
constexpr float PLAYER_HEIGHT = 1.80f;
constexpr float EYE_HEIGHT = 1.62f;

constexpr float WALK_SPEED = 5.0f;
constexpr float JUMP_SPEED = 7.0f;
constexpr float GRAVITY = 20.0f;
constexpr float REACH = 6.0f;

constexpr int MATERIAL_COUNT = 9;

constexpr int LOAD_RADIUS = RENDER_DISTANCE + 2;      
constexpr int UNLOAD_RADIUS = RENDER_DISTANCE + 4;    
constexpr int MAX_PENDING_CHUNKS = 8;                 
constexpr double MESH_BUDGET_SECONDS = 0.003;         

constexpr float CLOUD_CELL = 12.0f;
constexpr float CLOUD_THICKNESS = 4.0f;
constexpr float CLOUD_HEIGHT = 112.0f;
constexpr float CLOUD_SPEED = 0.6f;                   
constexpr int CLOUD_RADIUS_CELLS = 20;                

constexpr float NIGHT_BRIGHTNESS = 0.10f;

struct FallingSandEntity
{
    int x, z;
    float y;
    float velocityY;
};
std::vector<FallingSandEntity> fallingSands;

struct RemotePlayer {
    bool active = false;
    float x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0;
    float bodyYaw = 0;
};
RemotePlayer remotePlayers[MAX_PLAYERS];

struct Chunk
{
    int cx = 0, cz = 0;
    uint8_t blocks[CHUNK_SIZE][CHUNK_SIZE][WORLD_Y] = {};   
    Mesh meshes[MATERIAL_COUNT] = {};
    bool hasMesh[MATERIAL_COUNT] = {};
    bool dirty = true;
};

std::unordered_map<int64_t, Chunk*> chunks;
std::unordered_set<int64_t> pendingChunks;

const Block hotbar[9] = {
    GRASS, DIRT, STONE, WOOD, SAND, LEAVES, STONE, DIRT, GRASS
};

Texture2D crosshairTexture = {};
Texture2D grassSideTexture = {};
Texture2D grassTopTexture = {};
Texture2D grassBottomTexture = {};
Texture2D dirtTexture = {};
Texture2D oakLeavesTexture = {};
Texture2D oakWoodSideTexture = {};
Texture2D oakWoodTopTexture = {};
Texture2D stoneTexture = {};
Texture2D sandTexture = {};

Font customFont = {};

Model bodyModel = {};
Model headModel = {};
Texture2D playerSkinTexture = {};

double g_timeBase = START_TIME_SECONDS;
double g_timeStamp = 0.0;
double WorldTimeNow() { return g_timeBase + (GetTime() - g_timeStamp); }
void SetWorldTime(double t) { g_timeBase = t; g_timeStamp = GetTime(); }

Color g_worldTint = WHITE;     
int myPlayerId = -1;

struct Slot { Block block = AIR; int count = 0; };
Slot inventory[9];
constexpr bool CREATIVE_INFINITE_BLOCKS = false;    

struct DroppedItem
{
    uint32_t id = 0;
    Block block = AIR;
    int count = 1;
    Vector3 pos = {};          
    Vector3 shown = {};        
    float phase = 0.0f;
    double lastClaim = 0.0;
};
std::vector<DroppedItem> droppedItems;

struct PickupAnim { Block block; Vector3 from; int pickerId; float t; };
std::vector<PickupAnim> pickupAnims;

Material materials[MATERIAL_COUNT] = {};
Shader leavesCutoutShader = {};

constexpr float CROSSHAIR_SCALE = 1.75f;

const char* LEAVES_FRAGMENT_SHADER = R"(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main()
{
    vec4 texel = texture(texture0, fragTexCoord);
    if (texel.a < 0.5) discard;
    vec3 color = texel.rgb * fragColor.rgb * colDiffuse.rgb;
    finalColor = vec4(color, 1.0);
}
)";

const float FACE_VERTICES[6][6][3] = {
    {{1,0,0},{1,1,0},{1,1,1},{1,0,0},{1,1,1},{1,0,1}},
    {{0,0,1},{0,1,1},{0,1,0},{0,0,1},{0,1,0},{0,0,0}},
    {{0,1,1},{1,1,1},{1,1,0},{0,1,1},{1,1,0},{0,1,0}},
    {{0,0,0},{1,0,0},{1,0,1},{0,0,0},{1,0,1},{0,0,1}},
    {{1,0,1},{1,1,1},{0,1,1},{1,0,1},{0,1,1},{0,0,1}},
    {{0,0,0},{0,1,0},{1,1,0},{0,0,0},{1,1,0},{1,0,0}}
};

const int FACE_NORMALS[6][3] = {
    {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
};

const float FACE_UV[6][6][2] = {
    {{0,1},{0,0},{1,0},{0,1},{1,0},{1,1}},
    {{0,1},{0,0},{1,0},{0,1},{1,0},{1,1}},
    {{0,1},{1,1},{1,0},{0,1},{1,0},{0,0}},
    {{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}},
    {{1,1},{1,0},{0,0},{1,1},{0,0},{0,1}},
    {{1,1},{1,0},{0,0},{1,1},{0,0},{0,1}}
};

Chunk* GetChunk(int cx, int cz)
{
    auto it = chunks.find(ChunkKey(cx, cz));
    return it == chunks.end() ? nullptr : it->second;
}

bool InWorld(int x, int y, int z)
{
    return y >= 0 && y < WORLD_Y && GetChunk(ChunkOf(x), ChunkOf(z)) != nullptr;
}

bool IsSolid(int x, int y, int z)
{
    if (y < 0) return true;
    if (y >= WORLD_Y) return false;
    Chunk* c = GetChunk(ChunkOf(x), ChunkOf(z));
    if (!c) return true;
    return c->blocks[LocalOf(x)][LocalOf(z)][y] != AIR;
}

Block GetBlock(int x, int y, int z)
{
    if (y < 0 || y >= WORLD_Y) return AIR;
    Chunk* c = GetChunk(ChunkOf(x), ChunkOf(z));
    if (!c) return AIR;
    return (Block)c->blocks[LocalOf(x)][LocalOf(z)][y];
}

void SetBlock(int x, int y, int z, Block block)
{
    if (y < 0 || y >= WORLD_Y) return;
    Chunk* c = GetChunk(ChunkOf(x), ChunkOf(z));
    if (!c) return;
    c->blocks[LocalOf(x)][LocalOf(z)][y] = block;
}

Color BlockColor(Block block)
{
    switch (block)
    {
        case GRASS:  return {95, 175, 65, 255};
        case DIRT:   return {125, 82, 48, 255};
        case STONE:  return {130, 135, 145, 255};
        case WOOD:   
        case WOOD_HORIZ_X:
        case WOOD_HORIZ_Z: return WHITE;
        case SAND:   return {220, 205, 135, 255};
        case LEAVES: return WHITE;
        default:     return WHITE;
    }
}

const char* BlockName(Block block)
{
    switch (block)
    {
        case GRASS:  return "Erba";
        case DIRT:   return "Terra";
        case STONE:  return "Pietra";
        case WOOD:   
        case WOOD_HORIZ_X:
        case WOOD_HORIZ_Z: return "Legno";
        case SAND:   return "Sabbia";
        case LEAVES: return "Foglie";
        default:     return "";
    }
}

Texture2D FaceTexture(Block b, int face)
{
    switch (b)
    {
        case GRASS:  return face == 2 ? grassTopTexture : (face == 3 ? grassBottomTexture : grassSideTexture);
        case DIRT:   return dirtTexture;
        case STONE:  return stoneTexture;
        case WOOD:
        case WOOD_HORIZ_X:
        case WOOD_HORIZ_Z: return (face == 2 || face == 3) ? oakWoodTopTexture : oakWoodSideTexture;
        case SAND:   return sandTexture;
        case LEAVES: return oakLeavesTexture;
        default:     return Texture2D{};
    }
}

bool CanPickup(Block b)
{
    for (const Slot& sl : inventory) if (sl.count > 0 && sl.block == b && sl.count < MAX_STACK) return true;
    for (const Slot& sl : inventory) if (sl.count <= 0) return true;
    return false;
}

void AddToInventory(Block b, int n)
{
    for (Slot& sl : inventory)
        if (n > 0 && sl.count > 0 && sl.block == b && sl.count < MAX_STACK)
        {
            int add = std::min(n, MAX_STACK - sl.count);
            sl.count += add; n -= add;
        }
    for (Slot& sl : inventory)
        if (n > 0 && sl.count <= 0)
        {
            int add = std::min(n, MAX_STACK);
            sl.block = b; sl.count = add; n -= add;
        }
}

void MarkChunkDirty(int x, int z)
{
    int cx = ChunkOf(x), cz = ChunkOf(z);
    int lx = LocalOf(x), lz = LocalOf(z);

    auto mark = [](int a, int b) {
        if (Chunk* c = GetChunk(a, b)) c->dirty = true;
    };

    mark(cx, cz);
    if (lx == 0) mark(cx - 1, cz);
    if (lx == CHUNK_SIZE - 1) mark(cx + 1, cz);
    if (lz == 0) mark(cx, cz - 1);
    if (lz == CHUNK_SIZE - 1) mark(cx, cz + 1);
    if (lx == 0 && lz == 0) mark(cx - 1, cz - 1);
    if (lx == 0 && lz == CHUNK_SIZE - 1) mark(cx - 1, cz + 1);
    if (lx == CHUNK_SIZE - 1 && lz == 0) mark(cx + 1, cz - 1);
    if (lx == CHUNK_SIZE - 1 && lz == CHUNK_SIZE - 1) mark(cx + 1, cz + 1);
}

void FreeChunkMeshes(Chunk& chunk)
{
    for (int g = 0; g < MATERIAL_COUNT; g++)
    {
        if (chunk.hasMesh[g])
        {
            UnloadMesh(chunk.meshes[g]);
            chunk.meshes[g] = {};
            chunk.hasMesh[g] = false;
        }
    }
}

void BuildChunk(Chunk& chunk)
{
    FreeChunkMeshes(chunk);

    const Chunk* nb[3][3];
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++)
            nb[dx + 1][dz + 1] = GetChunk(chunk.cx + dx, chunk.cz + dz);

    auto blockAt = [&](int lx, int y, int lz) -> Block {
        if (y < 0) return STONE;          
        if (y >= WORLD_Y) return AIR;
        int ox = lx < 0 ? 0 : (lx >= CHUNK_SIZE ? 2 : 1);
        int oz = lz < 0 ? 0 : (lz >= CHUNK_SIZE ? 2 : 1);
        const Chunk* c = nb[ox][oz];
        if (!c) return AIR;
        return (Block)c->blocks[lx & (CHUNK_SIZE - 1)][lz & (CHUNK_SIZE - 1)][y];
    };

    static const float AO_BRIGHTNESS[4] = { 0.45f, 0.62f, 0.80f, 1.0f };

    auto isOccluder = [&](int lx, int y, int lz) -> bool {
        Block b = blockAt(lx, y, lz);
        return b != AIR && b != LEAVES;
    };

    auto vertexAO = [&](int x, int y, int z, int face, int vi) -> float {
        const int* n = FACE_NORMALS[face];
        const float* p = FACE_VERTICES[face][vi];

        int t[2] = { 0, 0 }, d[2] = { 0, 0 }, ti = 0;
        for (int a = 0; a < 3; a++)
        {
            if (n[a] == 0)
            {
                t[ti] = a;
                d[ti] = p[a] > 0.5f ? 1 : -1;
                ti++;
            }
        }

        int base[3] = { x + n[0], y + n[1], z + n[2] };
        int s1[3] = { base[0], base[1], base[2] };  s1[t[0]] += d[0];
        int s2[3] = { base[0], base[1], base[2] };  s2[t[1]] += d[1];
        int cr[3] = { base[0], base[1], base[2] };  cr[t[0]] += d[0];  cr[t[1]] += d[1];

        bool side1 = isOccluder(s1[0], s1[1], s1[2]);
        bool side2 = isOccluder(s2[0], s2[1], s2[2]);
        bool corner = isOccluder(cr[0], cr[1], cr[2]);

        int level = (side1 && side2) ? 0 : 3 - ((int)side1 + (int)side2 + (int)corner);
        return AO_BRIGHTNESS[level];
    };

    std::array<std::vector<float>, MATERIAL_COUNT> vertices;
    std::array<std::vector<float>, MATERIAL_COUNT> normals;
    std::array<std::vector<float>, MATERIAL_COUNT> texcoords;
    std::array<std::vector<unsigned char>, MATERIAL_COUNT> colors;

    for (int x = 0; x < CHUNK_SIZE; x++)
    {
        for (int z = 0; z < CHUNK_SIZE; z++)
        {
            for (int y = 0; y < WORLD_Y; y++)
            {
                Block block = (Block)chunk.blocks[x][z][y];
                if (block == AIR) continue;

                for (int face = 0; face < 6; face++)
                {
                    int nx = x + FACE_NORMALS[face][0];
                    int ny = y + FACE_NORMALS[face][1];
                    int nz = z + FACE_NORMALS[face][2];

                    Block neighbor = blockAt(nx, ny, nz);
                    bool neighborOpaque = neighbor != AIR && neighbor != LEAVES;
                    if (neighborOpaque) continue;

                    int group = 0;
                    if (block == GRASS) group = (face == 2 ? 2 : (face == 3 ? 3 : 1));
                    else if (block == DIRT) group = 0;
                    else if (block == LEAVES) group = 4;
                    else if (block == WOOD) group = (face == 2 || face == 3) ? 6 : 5;
                    else if (block == WOOD_HORIZ_X) group = (face == 0 || face == 1) ? 6 : 5;
                    else if (block == WOOD_HORIZ_Z) group = (face == 4 || face == 5) ? 6 : 5;
                    else if (block == STONE) group = 7;
                    else if (block == SAND) group = 8;

                    static const int CORNER_VERTEX[4] = { 0, 1, 2, 5 };
                    float ao[4];
                    for (int k = 0; k < 4; k++) ao[k] = vertexAO(x, y, z, face, CORNER_VERTEX[k]);

                    static const int ORDER_NORMAL[6]  = { 0, 1, 2, 0, 2, 3 };
                    static const int ORDER_FLIPPED[6] = { 0, 1, 3, 1, 2, 3 };
                    const int* order = (ao[0] + ao[2] >= ao[1] + ao[3]) ? ORDER_NORMAL : ORDER_FLIPPED;

                    for (int i = 0; i < 6; i++)
                    {
                        int corner = order[i];
                        int v = CORNER_VERTEX[corner];
                        unsigned char shade = (unsigned char)(255.0f * ao[corner]);

                        vertices[group].push_back((float)x + FACE_VERTICES[face][v][0]);
                        vertices[group].push_back((float)y + FACE_VERTICES[face][v][1]);
                        vertices[group].push_back((float)z + FACE_VERTICES[face][v][2]);
                        normals[group].push_back((float)FACE_NORMALS[face][0]);
                        normals[group].push_back((float)FACE_NORMALS[face][1]);
                        normals[group].push_back((float)FACE_NORMALS[face][2]);
                        texcoords[group].push_back(FACE_UV[face][v][0]);
                        texcoords[group].push_back(FACE_UV[face][v][1]);
                        colors[group].push_back(shade);
                        colors[group].push_back(shade);
                        colors[group].push_back(shade);
                        colors[group].push_back(255);
                    }
                }
            }
        }
    }

    chunk.dirty = false;
    for (int group = 0; group < MATERIAL_COUNT; group++)
    {
        if (vertices[group].empty()) continue;
        Mesh mesh = {};
        mesh.vertexCount = (int)(vertices[group].size() / 3);
        mesh.triangleCount = mesh.vertexCount / 3;
        mesh.vertices = (float*)MemAlloc((unsigned int)(vertices[group].size() * sizeof(float)));
        mesh.normals = (float*)MemAlloc((unsigned int)(normals[group].size() * sizeof(float)));
        mesh.texcoords = (float*)MemAlloc((unsigned int)(texcoords[group].size() * sizeof(float)));
        mesh.colors = (unsigned char*)MemAlloc((unsigned int)(colors[group].size() * sizeof(unsigned char)));

        std::memcpy(mesh.vertices, vertices[group].data(), vertices[group].size() * sizeof(float));
        std::memcpy(mesh.normals, normals[group].data(), normals[group].size() * sizeof(float));
        std::memcpy(mesh.texcoords, texcoords[group].data(), texcoords[group].size() * sizeof(float));
        std::memcpy(mesh.colors, colors[group].data(), colors[group].size() * sizeof(unsigned char));

        UploadMesh(&mesh, false);
        chunk.meshes[group] = mesh;
        chunk.hasMesh[group] = true;
    }
}

void BuildDirtyChunks(int pcx, int pcz)
{
    struct Candidate { int d2; Chunk* chunk; };
    static std::vector<Candidate> candidates;
    candidates.clear();

    for (auto& kv : chunks)
    {
        Chunk* c = kv.second;
        if (!c->dirty) continue;
        bool complete = true;
        for (int dx = -1; dx <= 1 && complete; dx++)
            for (int dz = -1; dz <= 1; dz++)
                if ((dx != 0 || dz != 0) && !GetChunk(c->cx + dx, c->cz + dz)) { complete = false; break; }
        if (!complete) continue;

        int dx = c->cx - pcx, dz = c->cz - pcz;
        candidates.push_back({ dx * dx + dz * dz, c });
    }

    if (candidates.empty()) return;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });

    double start = GetTime();
    for (const Candidate& cand : candidates)
    {
        BuildChunk(*cand.chunk);
        if (GetTime() - start > MESH_BUDGET_SECONDS) break;
    }
}

void DrawWorld(const Camera3D& cam)
{
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    constexpr float CHUNK_RADIUS = 52.0f;   

    auto isVisible = [&](const Chunk& c) -> bool {
        Vector3 center = {
            c.cx * (float)CHUNK_SIZE + CHUNK_SIZE * 0.5f,
            WORLD_Y * 0.5f,
            c.cz * (float)CHUNK_SIZE + CHUNK_SIZE * 0.5f
        };
        return Vector3DotProduct(Vector3Subtract(center, cam.position), fwd) > -CHUNK_RADIUS;
    };

    for (const auto& kv : chunks)
    {
        const Chunk& chunk = *kv.second;
        if (!isVisible(chunk)) continue;
        Matrix transform = MatrixTranslate((float)(chunk.cx * CHUNK_SIZE), 0.0f, (float)(chunk.cz * CHUNK_SIZE));
        for (int g = 0; g < MATERIAL_COUNT; g++)
        {
            if (g == 4) continue;
            if (chunk.hasMesh[g]) DrawMesh(chunk.meshes[g], materials[g], transform);
        }
    }

    rlEnableDepthMask();
    BeginBlendMode(BLEND_ALPHA);
    for (const auto& kv : chunks)
    {
        const Chunk& chunk = *kv.second;
        if (!chunk.hasMesh[4]) continue;
        if (!isVisible(chunk)) continue;
        Matrix transform = MatrixTranslate((float)(chunk.cx * CHUNK_SIZE), 0.0f, (float)(chunk.cz * CHUNK_SIZE));
        DrawMesh(chunk.meshes[4], materials[4], transform);
    }
    EndBlendMode();
}

void RequestNeededChunks(int pcx, int pcz)
{
    if ((int)pendingChunks.size() >= MAX_PENDING_CHUNKS) return;

    struct Candidate { int d2, cx, cz; };
    static std::vector<Candidate> candidates;
    candidates.clear();

    for (int dx = -LOAD_RADIUS; dx <= LOAD_RADIUS; dx++)
    {
        for (int dz = -LOAD_RADIUS; dz <= LOAD_RADIUS; dz++)
        {
            int d2 = dx * dx + dz * dz;
            if (d2 > LOAD_RADIUS * LOAD_RADIUS) continue;

            int cx = pcx + dx, cz = pcz + dz;
            int64_t key = ChunkKey(cx, cz);
            if (chunks.count(key) || pendingChunks.count(key)) continue;
            candidates.push_back({ d2, cx, cz });
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.d2 < b.d2; });

    for (const Candidate& c : candidates)
    {
        if ((int)pendingChunks.size() >= MAX_PENDING_CHUNKS) break;
        pendingChunks.insert(ChunkKey(c.cx, c.cz));
        SendChunkRequestPacket(c.cx, c.cz);
    }
}

void UnloadFarChunks(int pcx, int pcz)
{
    for (auto it = chunks.begin(); it != chunks.end(); )
    {
        Chunk* c = it->second;
        int dx = c->cx - pcx, dz = c->cz - pcz;
        if (dx * dx + dz * dz > UNLOAD_RADIUS * UNLOAD_RADIUS)
        {
            FreeChunkMeshes(*c);
            delete c;
            it = chunks.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

static uint32_t CloudHash(int x, int z)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + 1013904223u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return h;
}

static float CloudValueNoise(float x, float z)
{
    int xi = (int)floorf(x), zi = (int)floorf(z);
    float fx = x - (float)xi, fz = z - (float)zi;
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sz = fz * fz * (3.0f - 2.0f * fz);

    auto h = [](int a, int b) { return (CloudHash(a, b) & 0xFFFFFFu) / 16777216.0f; };
    float a = h(xi, zi),     b = h(xi + 1, zi);
    float c = h(xi, zi + 1), d = h(xi + 1, zi + 1);

    float top = a + (b - a) * sx;
    float bot = c + (d - c) * sx;
    return top + (bot - top) * sz;
}

static bool CloudAt(int cx, int cz)
{
    float n = CloudValueNoise(cx / 5.0f, cz / 5.0f) * 0.6f
            + CloudValueNoise(cx / 2.3f + 71.0f, cz / 2.3f + 33.0f) * 0.4f;
    return n > 0.55f;
}

void DrawClouds(Vector3 camPos, double offset)
{
    constexpr int R = CLOUD_RADIUS_CELLS;
    constexpr int N = 2 * R + 3;              
    static bool mask[N][N];

    double shiftD = floor(offset / CLOUD_CELL);
    int shiftCells = (int)shiftD;
    float frac = (float)(offset - shiftD * CLOUD_CELL);
    int baseX = (int)floorf((camPos.x - frac) / CLOUD_CELL);
    int baseZ = (int)floorf(camPos.z / CLOUD_CELL);

    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            mask[i][j] = CloudAt(baseX + i - R - 1 - shiftCells, baseZ + j - R - 1);

    const float y0 = CLOUD_HEIGHT;
    const float y1 = CLOUD_HEIGHT + CLOUD_THICKNESS;

    auto shade = [](float s) {
        unsigned char vr = (unsigned char)(g_worldTint.r * s), vg = (unsigned char)(g_worldTint.g * s), vb = (unsigned char)(g_worldTint.b * s);
        rlColor4ub(vr, vg, vb, 204);              
    };

    auto emit = [&]() {
        rlBegin(RL_QUADS);
        for (int i = 1; i < N - 1; i++)
        {
            for (int j = 1; j < N - 1; j++)
            {
                if (!mask[i][j]) continue;

                int di = i - R - 1, dj = j - R - 1;
                if (di * di + dj * dj > R * R) continue;

                float x0 = (float)(baseX + di) * CLOUD_CELL + frac, x1 = x0 + CLOUD_CELL;
                float z0 = (float)(baseZ + dj) * CLOUD_CELL,          z1 = z0 + CLOUD_CELL;

                shade(1.0f);   
                rlVertex3f(x0, y1, z0); rlVertex3f(x0, y1, z1); rlVertex3f(x1, y1, z1); rlVertex3f(x1, y1, z0);

                shade(0.7f);   
                rlVertex3f(x0, y0, z0); rlVertex3f(x1, y0, z0); rlVertex3f(x1, y0, z1); rlVertex3f(x0, y0, z1);

                if (!mask[i - 1][j])
                {
                    shade(0.9f);
                    rlVertex3f(x0, y0, z0); rlVertex3f(x0, y0, z1); rlVertex3f(x0, y1, z1); rlVertex3f(x0, y1, z0);
                }
                if (!mask[i + 1][j])
                {
                    shade(0.9f);
                    rlVertex3f(x1, y0, z0); rlVertex3f(x1, y1, z0); rlVertex3f(x1, y1, z1); rlVertex3f(x1, y0, z1);
                }
                if (!mask[i][j - 1])
                {
                    shade(0.8f);
                    rlVertex3f(x0, y0, z0); rlVertex3f(x0, y1, z0); rlVertex3f(x1, y1, z0); rlVertex3f(x1, y0, z0);
                }
                if (!mask[i][j + 1])
                {
                    shade(0.8f);
                    rlVertex3f(x0, y0, z1); rlVertex3f(x1, y0, z1); rlVertex3f(x1, y1, z1); rlVertex3f(x0, y1, z1);
                }
            }
        }
        rlEnd();
    };

    rlEnableDepthMask();
    rlDisableBackfaceCulling();

    rlSetBlendFactors(RL_ZERO, RL_ONE, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
    emit();
    EndBlendMode();

    BeginBlendMode(BLEND_ALPHA);
    emit();
    EndBlendMode();

    rlEnableBackfaceCulling();
}

void DrawPlayerModel(Vector3 pos, float headYaw, float bodyYaw, float pitch)
{
    float correctedBodyYaw = (bodyYaw * RAD2DEG) + 180.0f;
    float correctedHeadYaw = (headYaw * RAD2DEG) + 180.0f;

    DrawModelEx(bodyModel, pos, {0.0f, 1.0f, 0.0f}, correctedBodyYaw, {1.0f, 1.0f, 1.0f}, g_worldTint);

    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(correctedHeadYaw, 0.0f, 1.0f, 0.0f);
    rlTranslatef(0.0f, 1.45f, 0.0f);
    rlRotatef(pitch * RAD2DEG, 1.0f, 0.0f, 0.0f);
    rlTranslatef(0.0f, -1.45f, 0.0f);

    DrawModel(headModel, {0.0f, 0.0f, 0.0f}, 1.0f, g_worldTint);
    rlPopMatrix();
}

BoundingBox PlayerBox(Vector3 feet)
{
    return {
        {feet.x - PLAYER_RADIUS, feet.y, feet.z - PLAYER_RADIUS},
        {feet.x + PLAYER_RADIUS, feet.y + PLAYER_HEIGHT, feet.z + PLAYER_RADIUS}
    };
}

bool CollidesAt(Vector3 feet)
{
    BoundingBox player = PlayerBox(feet);
    int minX = (int)floorf(player.min.x), maxX = (int)floorf(player.max.x);
    int minY = (int)floorf(player.min.y), maxY = (int)floorf(player.max.y);
    int minZ = (int)floorf(player.min.z), maxZ = (int)floorf(player.max.z);

    for (int x = minX; x <= maxX; x++)
    {
        for (int y = minY; y <= maxY; y++)
        {
            for (int z = minZ; z <= maxZ; z++)
            {
                if (!IsSolid(x, y, z)) continue;
                BoundingBox block = {{(float)x, (float)y, (float)z}, {(float)x + 1, (float)y + 1, (float)z + 1}};
                if (CheckCollisionBoxes(player, block)) return true;
            }
        }
    }
    return false;
}

bool IsBlockInsidePlayer(int x, int y, int z, Vector3 feet)
{
    BoundingBox block = {{(float)x, (float)y, (float)z}, {(float)x + 1, (float)y + 1, (float)z + 1}};
    return CheckCollisionBoxes(block, PlayerBox(feet));
}

struct BlockHit { bool hit = false; int x = 0, y = 0, z = 0; int nx = 0, ny = 0, nz = 0; };

BlockHit RaycastWorld(Ray ray)
{
    BlockHit result;
    Vector3 dir = Vector3Normalize(ray.direction);
    int x = (int)floorf(ray.position.x), y = (int)floorf(ray.position.y), z = (int)floorf(ray.position.z);
    int stepX = dir.x >= 0 ? 1 : -1, stepY = dir.y >= 0 ? 1 : -1, stepZ = dir.z >= 0 ? 1 : -1;
    float deltaX = dir.x == 0 ? INFINITY : fabsf(1.0f / dir.x);
    float deltaY = dir.y == 0 ? INFINITY : fabsf(1.0f / dir.y);
    float deltaZ = dir.z == 0 ? INFINITY : fabsf(1.0f / dir.z);
    float maxX = dir.x == 0 ? INFINITY : ((dir.x >= 0 ? x + 1.0f - ray.position.x : ray.position.x - x) / fabsf(dir.x));
    float maxY = dir.y == 0 ? INFINITY : ((dir.y >= 0 ? y + 1.0f - ray.position.y : ray.position.y - y) / fabsf(dir.y));
    float maxZ = dir.z == 0 ? INFINITY : ((dir.z >= 0 ? z + 1.0f - ray.position.z : ray.position.z - z) / fabsf(dir.z));
    int normalX = 0, normalY = 0, normalZ = 0;
    float distance = 0.0f;

    while (distance <= REACH)
    {
        if (IsSolid(x, y, z))
        {
            if (!InWorld(x, y, z)) return result;
            result.hit = true; result.x = x; result.y = y; result.z = z;
            result.nx = normalX; result.ny = normalY; result.nz = normalZ;
            return result;
        }
        if (maxX < maxY && maxX < maxZ) { x += stepX; distance = maxX; maxX += deltaX; normalX = -stepX; normalY = 0; normalZ = 0; }
        else if (maxY < maxZ) { y += stepY; distance = maxY; maxY += deltaY; normalX = 0; normalY = -stepY; normalZ = 0; }
        else { z += stepZ; distance = maxZ; maxZ += deltaZ; normalX = 0; normalY = 0; normalZ = -stepZ; }
    }
    return result;
}

struct SkyState
{
    float sunAngle;     
    float starAlpha;    
    float glow;         
    float glowSide;     
    Color clear;        
    Color tint;         
};

SkyState ComputeSky(double t)
{
    SkyState s = {};
    double frac = fmod(t, DAY_LENGTH_SECONDS) / DAY_LENGTH_SECONDS;
    if (frac < 0.0) frac += 1.0;

    s.sunAngle = (float)(frac * 2.0 * PI);
    float elev = sinf(s.sunAngle);                                    
    float d = Clamp(elev * 2.0f + 0.5f, 0.0f, 1.0f);                   
    d = d * d * (3.0f - 2.0f * d);

    s.starAlpha = Clamp((-elev - 0.02f) / 0.25f, 0.0f, 1.0f);
    s.glow = Clamp(1.0f - fabsf(elev) / 0.30f, 0.0f, 1.0f);
    s.glowSide = cosf(s.sunAngle) >= 0.0f ? 1.0f : -1.0f;

    auto mix = [&](float a, float b) { return (unsigned char)Lerp(a, b, d); };
    s.clear = { mix(5, 135), mix(6, 190), mix(15, 235), 255 };

    float br = Lerp(NIGHT_BRIGHTNESS, 1.0f, d);
    s.tint = { (unsigned char)(255.0f * br * Lerp(0.82f, 1.0f, d)),
               (unsigned char)(255.0f * br * Lerp(0.88f, 1.0f, d)),
               (unsigned char)(255.0f * br), 255 };
    return s;
}

constexpr int STAR_COUNT = 1500;
Vector3 starCorners[STAR_COUNT][4];

void InitStars()
{
    uint32_t seed = 987654321u;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };

    for (int i = 0; i < STAR_COUNT; i++)
    {
        Vector3 n;
        float len2;
        do {
            n = { rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f };
            len2 = n.x * n.x + n.y * n.y + n.z * n.z;
        } while (len2 > 1.0f || len2 < 0.01f);
        n = Vector3Scale(n, 1.0f / sqrtf(len2));

        Vector3 ref = fabsf(n.y) > 0.9f ? Vector3{1, 0, 0} : Vector3{0, 1, 0};
        Vector3 t1 = Vector3Normalize(Vector3CrossProduct(ref, n));
        Vector3 t2 = Vector3CrossProduct(n, t1);
        float size = 0.25f + rnd() * 0.40f;

        Vector3 c = Vector3Scale(n, 90.0f);
        starCorners[i][0] = Vector3Add(c, Vector3Add(Vector3Scale(t1, -size), Vector3Scale(t2, -size)));
        starCorners[i][1] = Vector3Add(c, Vector3Add(Vector3Scale(t1,  size), Vector3Scale(t2, -size)));
        starCorners[i][2] = Vector3Add(c, Vector3Add(Vector3Scale(t1,  size), Vector3Scale(t2,  size)));
        starCorners[i][3] = Vector3Add(c, Vector3Add(Vector3Scale(t1, -size), Vector3Scale(t2,  size)));
    }
}

void DrawSky(const SkyState& sky, Vector3 camPos)
{
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    rlDisableDepthMask();
    rlDisableBackfaceCulling();

    rlPushMatrix();
    rlTranslatef(camPos.x, camPos.y, camPos.z);

    if (sky.glow > 0.0f)
    {
        const float x = sky.glowSide * 100.0f;
        const int SEG = 24;
        unsigned char a = (unsigned char)(210.0f * sky.glow * sky.glow);
        rlBegin(RL_TRIANGLES);
        for (int k = 0; k < SEG; k++)
        {
            float a0 = 2.0f * PI * k / SEG, a1 = 2.0f * PI * (k + 1) / SEG;
            rlColor4ub(255, 150, 60, a);  rlVertex3f(x, 0.0f, 0.0f);
            rlColor4ub(255, 100, 40, 0);  rlVertex3f(x, 75.0f * sinf(a0), 75.0f * cosf(a0));
            rlColor4ub(255, 100, 40, 0);  rlVertex3f(x, 75.0f * sinf(a1), 75.0f * cosf(a1));
        }
        rlEnd();
    }

    rlRotatef(sky.sunAngle * RAD2DEG, 0.0f, 0.0f, 1.0f);

    if (sky.starAlpha > 0.0f)
    {
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, (unsigned char)(230.0f * sky.starAlpha));
        for (int i = 0; i < STAR_COUNT; i++)
            for (int k = 0; k < 4; k++)
                rlVertex3f(starCorners[i][k].x, starCorners[i][k].y, starCorners[i][k].z);
        rlEnd();
    }

    auto square = [](float x, float cy, float cz, float h, Color c) {
        rlColor4ub(c.r, c.g, c.b, c.a);
        rlVertex3f(x, cy - h, cz - h); rlVertex3f(x, cy - h, cz + h);
        rlVertex3f(x, cy + h, cz + h); rlVertex3f(x, cy + h, cz - h);
    };

    rlBegin(RL_QUADS);
    square(100.0f, 0, 0, 24.0f, { 255, 225, 140, 55 });
    square(100.0f, 0, 0, 15.0f, { 255, 250, 215, 255 });
    square(-100.0f, 0, 0, 10.0f, { 222, 228, 240, 255 });
    square(-99.9f, -3.0f,  2.0f, 2.6f, { 190, 197, 214, 255 });
    square(-99.9f,  4.0f, -4.0f, 3.0f, { 195, 202, 218, 255 });
    square(-99.9f, -5.0f, -5.0f, 1.8f, { 188, 195, 212, 255 });
    square(-99.9f,  5.0f,  5.0f, 1.6f, { 192, 199, 215, 255 });
    rlEnd();

    rlPopMatrix();

    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
    rlEnableDepthTest();
}

void DrawBlockCube(Block b, Vector3 center, float size, float yawDeg, Color tint)
{
    static const float FACE_SHADE[6] = { 0.8f, 0.8f, 1.0f, 0.5f, 0.6f, 0.6f };

    rlPushMatrix();
    rlTranslatef(center.x, center.y, center.z);
    rlRotatef(yawDeg, 0.0f, 1.0f, 0.0f);
    rlScalef(size, size, size);

    for (int f = 0; f < 6; f++)
    {
        Texture2D tex = FaceTexture(b, f);
        if (tex.id == 0) continue;

        rlSetTexture(tex.id);
        rlBegin(RL_TRIANGLES);
        rlColor4ub((unsigned char)(tint.r * FACE_SHADE[f]), (unsigned char)(tint.g * FACE_SHADE[f]), (unsigned char)(tint.b * FACE_SHADE[f]), 255);
        for (int v = 0; v < 6; v++)
        {
            rlTexCoord2f(FACE_UV[f][v][0], FACE_UV[f][v][1]);
            rlVertex3f(FACE_VERTICES[f][v][0] - 0.5f, FACE_VERTICES[f][v][1] - 0.5f, FACE_VERTICES[f][v][2] - 0.5f);
        }
        rlEnd();
        rlSetTexture(0);
    }

    rlPopMatrix();
}

void UpdateDroppedItems(float dt, Vector3 player, bool spawned)
{
    const double now = GetTime();
    const float k = std::min(1.0f, dt * 20.0f);

    for (DroppedItem& it : droppedItems)
    {
        it.shown = Vector3Lerp(it.shown, it.pos, k);

        if (!spawned || !IsClientConnected() || now - it.lastClaim < 0.3 || !CanPickup(it.block)) continue;

        float dx = it.pos.x - player.x, dz = it.pos.z - player.z, dy = it.pos.y - player.y;
        if (dx * dx + dz * dz < 1.4f * 1.4f && dy > -0.7f && dy < 2.0f)
        {
            SendItemPickupPacket(it.id);
            it.lastClaim = now;
        }
    }
}

void DrawDroppedItems()
{
    const float t = (float)GetTime();

    for (const DroppedItem& it : droppedItems)
    {
        if (!GetChunk(ChunkOf((int)floorf(it.shown.x)), ChunkOf((int)floorf(it.shown.z)))) continue;

        float bob = 0.20f + sinf(t * 2.5f + it.phase) * 0.05f;
        float yaw = fmodf(t * 60.0f + it.phase * 57.0f, 360.0f);
        int copies = it.count > 16 ? 3 : (it.count > 1 ? 2 : 1);

        for (int c = 0; c < copies; c++)
        {
            Vector3 p = { it.shown.x + (c - (copies - 1) * 0.5f) * 0.08f,
                          it.shown.y + bob,
                          it.shown.z + (c - (copies - 1) * 0.5f) * 0.06f };
            DrawBlockCube(it.block, p, 0.30f, yaw, g_worldTint);
        }
    }
}

void DrawPickupAnims(float dt, Vector3 player)
{
    for (size_t i = 0; i < pickupAnims.size(); )
    {
        PickupAnim& a = pickupAnims[i];
        a.t += dt;
        float k = a.t / 0.18f;
        if (k >= 1.0f) { pickupAnims.erase(pickupAnims.begin() + i); continue; }

        Vector3 target = player;
        if (a.pickerId != myPlayerId && a.pickerId >= 0 && a.pickerId < MAX_PLAYERS)
            target = { remotePlayers[a.pickerId].x, remotePlayers[a.pickerId].y, remotePlayers[a.pickerId].z };
        target.y += 0.9f;

        Vector3 p = Vector3Lerp(a.from, target, k * k);
        DrawBlockCube(a.block, p, 0.30f * (1.0f - 0.5f * k), a.t * 300.0f, g_worldTint);
        ++i;
    }
}

void DrawBlockIcon(Block b, float cx, float cy, float width)
{
    float hw = width * 0.5f;
    float R = hw / 0.866f;
    float hr = R * 0.5f;

    Vector2 T = {cx, cy - R}, UR = {cx + hw, cy - hr}, LR = {cx + hw, cy + hr};
    Vector2 B = {cx, cy + R}, LL = {cx - hw, cy + hr}, UL = {cx - hw, cy - hr}, C = {cx, cy};

    auto quad = [](Texture2D tex, Vector2 p0, Vector2 p1, Vector2 p2, Vector2 p3, float shade) {
        if (tex.id == 0) return;
        unsigned char v = (unsigned char)(255.0f * shade);
        rlSetTexture(tex.id);
        rlBegin(RL_QUADS);
        rlColor4ub(v, v, v, 255);
        rlTexCoord2f(0, 0); rlVertex2f(p0.x, p0.y);
        rlTexCoord2f(1, 0); rlVertex2f(p1.x, p1.y);
        rlTexCoord2f(1, 1); rlVertex2f(p2.x, p2.y);
        rlTexCoord2f(0, 1); rlVertex2f(p3.x, p3.y);
        rlEnd();
        rlSetTexture(0);
    };

    quad(FaceTexture(b, 2), T, UR, C, UL, 1.0f);     
    quad(FaceTexture(b, 4), UL, C, B, LL, 0.80f);    
    quad(FaceTexture(b, 0), C, UR, LR, B, 0.60f);    
}

void DrawHotbar(int selected, Font font)
{
    float scale = (float)GetScreenHeight() / 720.0f;

    constexpr int SLOTS = 9;
    int size = (int)(56.0f * scale);
    int total = SLOTS * size;
    int startX = (GetScreenWidth() - total) / 2;
    int y = GetScreenHeight() - (int)(160.0f * scale);

    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();

    for (int i = 0; i < SLOTS; i++)
    {
        int x = startX + i * size;
        DrawRectangle(x, y, size, size, Fade(BLACK, 0.70f));
        DrawRectangleLinesEx({(float)x, (float)y, (float)size, (float)size}, i == selected ? 3.0f * scale : 1.0f * scale, i == selected ? DARKGRAY : GRAY);

        const Slot& sl = inventory[i];
        if (sl.count <= 0) continue;

        DrawBlockIcon(sl.block, x + size * 0.5f, y + size * 0.5f, 36.0f * scale);

        if (sl.count > 1)
        {
            const char* num = TextFormat("%d", sl.count);
            float fs = 16.0f * scale;
            Vector2 ts = MeasureTextEx(font, num, fs, 1.0f);
            Vector2 pos = { x + size - ts.x - 4.0f * scale, y + size - ts.y - 2.0f * scale };
            DrawTextEx(font, num, { pos.x + 1.5f * scale, pos.y + 1.5f * scale }, fs, 1.0f, BLACK);
            DrawTextEx(font, num, pos, fs, 1.0f, WHITE);
        }
    }

    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();

    const char* itemName = inventory[selected].count > 0 ? BlockName(inventory[selected].block) : "";
    float fontSize = 20.0f * scale;
    Vector2 textSize = MeasureTextEx(font, itemName, fontSize, 1.0f);
    float nameX = startX + (total - textSize.x) / 2.0f;
    float nameY = y - (int)(75.0f * scale);
    DrawTextEx(font, itemName, {nameX, nameY}, fontSize, 1.0f, WHITE);
}

int main()
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_FULLSCREEN_MODE);
    InitWindow(1280, 720, "TinyTillers");
    SetTargetFPS(144);

    InitStars();
    SetWorldTime(START_TIME_SECONDS);

    customFont = LoadFont("assets/UI/font.ttf");

    std::string chosenIp = RunMainMenu(customFont);
    if (chosenIp.empty() || WindowShouldClose())
    {
        UnloadFont(customFont);
        CloseWindow();
        return 0;
    }

    DisableCursor();

    const double connectStart = GetTime();
    bool everConnected = false;

    if (!InitClientNetwork(chosenIp.c_str(), SERVER_PORT))
    {
        std::cerr << "Impossibile connettersi al server.\n";
    }

    crosshairTexture = LoadTexture("assets/UI/Crosshair.png");
    grassSideTexture = LoadTexture("assets/Blocks/Grass_L.png");
    grassTopTexture = LoadTexture("assets/Blocks/Grass_U.png");
    grassBottomTexture = LoadTexture("assets/Blocks/Grass_B.png");
    dirtTexture = LoadTexture("assets/Blocks/Grass_B.png");
    oakLeavesTexture = LoadTexture("assets/Blocks/Oak_Leaves.png");
    oakWoodSideTexture = LoadTexture("assets/Blocks/Oak_Wood_L.png");
    oakWoodTopTexture = LoadTexture("assets/Blocks/Oak_Wood_U.png");
    stoneTexture = LoadTexture("assets/Blocks/Stone.png");
    sandTexture = LoadTexture("assets/Blocks/Sand.png");

    bodyModel = LoadModel("assets/Entities/body.obj");
    headModel = LoadModel("assets/Entities/head.obj");
    playerSkinTexture = LoadTexture("assets/Entities/skin.png");

    if (bodyModel.materialCount > 0) bodyModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = playerSkinTexture;
    if (headModel.materialCount > 0) headModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = playerSkinTexture;

    leavesCutoutShader = LoadShaderFromMemory(nullptr, LEAVES_FRAGMENT_SHADER);

    for (int i = 0; i < MATERIAL_COUNT; i++)
    {
        materials[i] = LoadMaterialDefault();
        materials[i].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    }

    materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = dirtTexture;
    materials[1].maps[MATERIAL_MAP_DIFFUSE].texture = grassSideTexture;
    materials[2].maps[MATERIAL_MAP_DIFFUSE].texture = grassTopTexture;
    materials[3].maps[MATERIAL_MAP_DIFFUSE].texture = grassBottomTexture;
    materials[4].maps[MATERIAL_MAP_DIFFUSE].texture = oakLeavesTexture;
    materials[5].maps[MATERIAL_MAP_DIFFUSE].texture = oakWoodSideTexture;
    materials[6].maps[MATERIAL_MAP_DIFFUSE].texture = oakWoodTopTexture;
    materials[7].maps[MATERIAL_MAP_DIFFUSE].texture = stoneTexture;
    materials[8].maps[MATERIAL_MAP_DIFFUSE].texture = sandTexture;

    if (IsShaderValid(leavesCutoutShader))
        materials[4].shader = leavesCutoutShader;

    Mesh sandCubeMesh = GenMeshCube(1.0f, 1.0f, 1.0f);

    Vector3 player = { SPAWN_X, 40.0f, SPAWN_Z };
    Vector3 velocity = {0, 0, 0};
    bool onGround = false;
    bool spawned = false;
    float yaw = 0.0f, pitch = 0.0f;
    float bodyYaw = 0.0f;
    int selected = 0;
    int cameraMode = 0;
    bool isBorderless = false;

    for (int i = 0; i < 9; i++) { inventory[i].block = hotbar[i]; inventory[i].count = MAX_STACK; }

    Camera3D camera = {};
    camera.position = {player.x, player.y + EYE_HEIGHT, player.z};
    camera.target = {player.x, player.y + EYE_HEIGHT, player.z + 1.0f};
    camera.up = {0, 1, 0};
    camera.fovy = 75.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    while (!WindowShouldClose())
    {
        if (IsKeyPressed(KEY_F11))
        {
            isBorderless = !isBorderless;
            int monitor = GetCurrentMonitor();
            if (isBorderless)
            {
                SetWindowState(FLAG_WINDOW_UNDECORATED);
                SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
                SetWindowPosition(0, 0);
            }
            else
            {
                ClearWindowState(FLAG_WINDOW_UNDECORATED);
                SetWindowSize(1280, 720);
                SetWindowPosition((GetMonitorWidth(monitor) - 1280) / 2, (GetMonitorHeight(monitor) - 720) / 2);
            }
        }

        if (IsKeyPressed(KEY_F5)) cameraMode = (cameraMode + 1) % 3;

        NetCallbacks cb;
        cb.onChunk = [](int cx, int cz, const uint8_t* blocks) {
            int64_t key = ChunkKey(cx, cz);
            pendingChunks.erase(key);

            Chunk* c = GetChunk(cx, cz);
            if (!c)
            {
                c = new Chunk();
                c->cx = cx;
                c->cz = cz;
                chunks[key] = c;
            }
            std::memcpy(c->blocks, blocks, CHUNK_VOLUME);
            c->dirty = true;
        };
        cb.onBlock = [](int x, int y, int z, uint8_t block) {
            if (block == FALLING_SAND)
            {
                fallingSands.push_back({x, z, (float)y, 0.0f});
                return;
            }

            if (!InWorld(x, y, z)) return;

            SetBlock(x, y, z, (Block)block);
            MarkChunkDirty(x, z);
        };
        cb.onPlayerState = [](uint8_t playerId, float x, float y, float z, float yaw, float pitch) {
            if (playerId < MAX_PLAYERS)
            {
                remotePlayers[playerId].active = true;
                remotePlayers[playerId].x = x;
                remotePlayers[playerId].y = y;
                remotePlayers[playerId].z = z;
                remotePlayers[playerId].yaw = yaw;
                remotePlayers[playerId].pitch = pitch;

                float maxHeadAngle = 50.0f * DEG2RAD;
                float rDiff = remotePlayers[playerId].yaw - remotePlayers[playerId].bodyYaw;
                while (rDiff > PI) rDiff -= 2.0f * PI;
                while (rDiff < -PI) rDiff += 2.0f * PI;
                if (rDiff > maxHeadAngle) remotePlayers[playerId].bodyYaw = remotePlayers[playerId].yaw - maxHeadAngle;
                else if (rDiff < -maxHeadAngle) remotePlayers[playerId].bodyYaw = remotePlayers[playerId].yaw + maxHeadAngle;
            }
        };
        cb.onWelcome = [](uint8_t playerId, double worldTime) {
            myPlayerId = playerId;
            SetWorldTime(worldTime);
        };
        cb.onTimeSync = [](double worldTime) {
            SetWorldTime(worldTime);
        };
        cb.onItemState = [](uint32_t id, uint8_t block, uint8_t count, float x, float y, float z) {
            for (DroppedItem& it : droppedItems)
            {
                if (it.id != id) continue;
                it.pos = { x, y, z };
                it.block = (Block)block;
                it.count = count;
                return;
            }
            DroppedItem d;
            d.id = id;
            d.block = (Block)block;
            d.count = count;
            d.pos = d.shown = { x, y, z };
            d.phase = (float)(id % 628) * 0.01f;
            droppedItems.push_back(d);
        };
        cb.onItemRemove = [](uint32_t id, uint8_t pickerId, uint8_t block, uint8_t count) {
            for (size_t i = 0; i < droppedItems.size(); i++)
            {
                if (droppedItems[i].id != id) continue;
                if (pickerId != 255)
                {
                    Vector3 from = droppedItems[i].shown;
                    from.y += 0.2f;
                    pickupAnims.push_back({ (Block)block, from, (int)pickerId, 0.0f });
                }
                droppedItems.erase(droppedItems.begin() + i);
                break;
            }
            if (pickerId != 255 && pickerId == myPlayerId) AddToInventory((Block)block, count);
        };
        PollClientNetwork(cb);

        int playerChunkX = ChunkOf((int)floorf(player.x));
        int playerChunkZ = ChunkOf((int)floorf(player.z));
        if (IsClientConnected()) RequestNeededChunks(playerChunkX, playerChunkZ);
        UnloadFarChunks(playerChunkX, playerChunkZ);

        if (!spawned)
        {
            int sx = (int)floorf(player.x), sz = (int)floorf(player.z);
            Chunk* spawnChunk = GetChunk(ChunkOf(sx), ChunkOf(sz));
            if (spawnChunk && !spawnChunk->dirty)
            {
                int gy = WORLD_Y - 1;
                while (gy > 0 && !IsSolid(sx, gy, sz)) gy--;
                player.y = (float)gy + 1.0f;
                spawned = true;
            }
        }

        float dt = std::min(GetFrameTime(), 0.05f);

        const double worldNow = WorldTimeNow();
        const SkyState sky = ComputeSky(worldNow);
        g_worldTint = sky.tint;
        for (int i = 0; i < MATERIAL_COUNT; i++) materials[i].maps[MATERIAL_MAP_DIFFUSE].color = sky.tint;

        UpdateDroppedItems(dt, player, spawned);

        for (size_t i = 0; i < fallingSands.size(); )
        {
            FallingSandEntity& fs = fallingSands[i];
            float oldY = fs.y;

            fs.velocityY += 30.0f * dt;
            fs.y -= fs.velocityY * dt;

            int landing = -1;
            for (int cy = (int)floorf(oldY) - 1; cy >= (int)floorf(fs.y); --cy)
            {
                if (IsSolid(fs.x, cy, fs.z)) { landing = cy + 1; break; }
            }

            if (landing < 0) { ++i; continue; }

            if (GetChunk(ChunkOf(fs.x), ChunkOf(fs.z)) != nullptr)
            {
                while (landing < WORLD_Y && GetBlock(fs.x, landing, fs.z) != AIR)
                    landing++;

                if (landing < WORLD_Y)
                {
                    SetBlock(fs.x, landing, fs.z, SAND);
                    MarkChunkDirty(fs.x, fs.z);
                }
            }

            fallingSands.erase(fallingSands.begin() + i);
        }

        Vector2 mouse = GetMouseDelta();
        yaw -= mouse.x * 0.0025f;
        pitch -= mouse.y * 0.0025f;
        pitch = std::clamp(pitch, -1.50f, 1.50f);

        float maxHeadAngle = 50.0f * DEG2RAD;
        float angleDiff = yaw - bodyYaw;
        while (angleDiff > PI) angleDiff -= 2.0f * PI;
        while (angleDiff < -PI) angleDiff += 2.0f * PI;

        if (angleDiff > maxHeadAngle) {
            bodyYaw = yaw - maxHeadAngle;
        } else if (angleDiff < -maxHeadAngle) {
            bodyYaw = yaw + maxHeadAngle;
        }

        Vector3 forward = {sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch)};
        Vector3 forwardFlat = {sinf(yaw), 0.0f, cosf(yaw)};
        Vector3 right = {cosf(yaw), 0.0f, -sinf(yaw)};

        if (spawned)
        {
            Vector3 move = {0, 0, 0};
            if (IsKeyDown(KEY_W)) move = Vector3Add(move, forwardFlat);
            if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, forwardFlat);
            if (IsKeyDown(KEY_A)) move = Vector3Add(move, right);
            if (IsKeyDown(KEY_D)) move = Vector3Subtract(move, right);
            if (Vector3Length(move) > 0.0f) {
                move = Vector3Normalize(move);
            }

            Vector3 step = Vector3Scale(move, WALK_SPEED * dt);
            Vector3 next = player;
            next.x += step.x; if (!CollidesAt(next)) player.x = next.x;
            next = player;
            next.z += step.z; if (!CollidesAt(next)) player.z = next.z;

            if (IsKeyPressed(KEY_SPACE) && onGround) { velocity.y = JUMP_SPEED; onGround = false; }
            velocity.y -= GRAVITY * dt;
            velocity.y = std::max(velocity.y, -30.0f);

            float verticalStep = velocity.y * dt;
            next = player;
            next.y += verticalStep;
            if (!CollidesAt(next)) { player.y = next.y; onGround = false; }
            else { if (verticalStep < 0.0f) onGround = true; velocity.y = 0.0f; }
        }

        Vector3 eyePos = {player.x, player.y + EYE_HEIGHT, player.z};
        if (cameraMode == 0)
        {
            camera.position = eyePos;
            camera.target = Vector3Add(camera.position, forward);
        }
        else if (cameraMode == 1)
        {
            Vector3 backOffset = Vector3Scale(forward, 3.5f);
            camera.position = Vector3Subtract(eyePos, backOffset);
            camera.position.y += 0.5f;
            camera.target = eyePos;
        }
        else if (cameraMode == 2)
        {
            Vector3 frontOffset = Vector3Scale(forward, 3.5f);
            camera.position = Vector3Add(eyePos, frontOffset);
            camera.position.y += 0.5f;
            camera.target = eyePos;
        }

        SendPlayerStatePacket(player.x, player.y, player.z, yaw, pitch);

        for (int i = 0; i < 9; i++)
            if (IsKeyPressed((KeyboardKey)(KEY_ONE + i))) selected = i;

        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) selected = (selected + (wheel > 0 ? 8 : 1)) % 9;

        if (IsKeyPressed(KEY_Q) && spawned && IsClientConnected() && inventory[selected].count > 0)
        {
            bool all = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            int n = all ? inventory[selected].count : 1;
            SendDropRequestPacket((uint8_t)inventory[selected].block, (uint8_t)n);
            inventory[selected].count -= n;
            if (inventory[selected].count <= 0) inventory[selected] = Slot{};
        }

        Ray ray = GetMouseRay({(float)GetScreenWidth() / 2.0f, (float)GetScreenHeight() / 2.0f}, camera);
        BlockHit hit = RaycastWorld(ray);

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hit.hit && InWorld(hit.x, hit.y, hit.z))
        {
            SetBlock(hit.x, hit.y, hit.z, AIR);
            MarkChunkDirty(hit.x, hit.z);
            SendBlockChangePacket(hit.x, hit.y, hit.z, AIR);
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && hit.hit)
        {
            int bx = hit.x + hit.nx;
            int by = hit.y + hit.ny;
            int bz = hit.z + hit.nz;

            if (InWorld(bx, by, bz) && GetBlock(bx, by, bz) == AIR && !IsBlockInsidePlayer(bx, by, bz, player) && inventory[selected].count > 0)
            {
                Block blockToPlace = inventory[selected].block;

                if (blockToPlace == WOOD)
                {
                    if (hit.ny != 0) blockToPlace = WOOD;
                    else if (hit.nx != 0) blockToPlace = WOOD_HORIZ_X;
                    else if (hit.nz != 0) blockToPlace = WOOD_HORIZ_Z;
                }

                SetBlock(bx, by, bz, blockToPlace);
                MarkChunkDirty(bx, bz);
                SendBlockChangePacket(bx, by, bz, blockToPlace);

                if (!CREATIVE_INFINITE_BLOCKS && --inventory[selected].count <= 0) inventory[selected] = Slot{};
            }
        }

        BuildDirtyChunks(playerChunkX, playerChunkZ);

        BeginDrawing();
        ClearBackground(sky.clear);
        BeginMode3D(camera);
        DrawSky(sky, camera.position);
        DrawWorld(camera);

        DrawClouds(camera.position, CLOUD_SPEED * worldNow);

        if (cameraMode != 0)
        {
            DrawPlayerModel(player, yaw, bodyYaw, pitch);
        }

        for (int i = 0; i < MAX_PLAYERS; i++)
        {
            if (remotePlayers[i].active)
            {
                Vector3 remotePos = { remotePlayers[i].x, remotePlayers[i].y, remotePlayers[i].z };
                
                float distX = fabsf(remotePos.x - player.x);
                float distZ = fabsf(remotePos.z - player.z);
                if (distX < 0.3f && distZ < 0.3f)
                    continue;

                DrawPlayerModel(remotePos, remotePlayers[i].yaw, remotePlayers[i].bodyYaw, remotePlayers[i].pitch);
            }
        }

        DrawDroppedItems();
        DrawPickupAnims(dt, player);

        for (const auto& fs : fallingSands)
        {
            DrawMesh(sandCubeMesh, materials[8],
                     MatrixTranslate(fs.x + 0.5f, fs.y + 0.5f, fs.z + 0.5f));
        }

        if (hit.hit && InWorld(hit.x, hit.y, hit.z))
        {
            DrawCubeWires({hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f}, 1.015f, 1.015f, 1.015f, BLACK);
        }
        EndMode3D();

        if (crosshairTexture.id != 0)
        {
            float width = crosshairTexture.width * CROSSHAIR_SCALE;
            float height = crosshairTexture.height * CROSSHAIR_SCALE;
            DrawTexturePro(crosshairTexture, {0, 0, (float)crosshairTexture.width, (float)crosshairTexture.height}, 
                           {GetScreenWidth() / 2.0f - width / 2.0f, GetScreenHeight() / 2.0f - height / 2.0f, width, height}, {0, 0}, 0, WHITE);
        }

        DrawHotbar(selected, customFont);

        if (IsClientConnected()) everConnected = true;
        if (!IsClientConnected())
        {
            double waited = GetTime() - connectStart;
            std::string l1, l2;
            Color c = YELLOW;
            if (everConnected)
            {
                l1 = "Connessione al server persa";
                c = RED;
            }
            else if (waited < 6.0)
            {
                l1 = "Connessione a " + chosenIp + ":" + std::to_string(SERVER_PORT) + "...";
            }
            else
            {
                l1 = "Impossibile connettersi a " + chosenIp + ":" + std::to_string(SERVER_PORT);
                l2 = "Controlla IP, port forwarding UDP " + std::to_string(SERVER_PORT) + " e firewall del server";
                c = RED;
            }
            Vector2 s1 = MeasureTextEx(customFont, l1.c_str(), 22.0f, 1.0f);
            DrawTextEx(customFont, l1.c_str(), {GetScreenWidth() / 2.0f - s1.x / 2.0f, GetScreenHeight() / 2.0f + 100.0f}, 22.0f, 1.0f, c);
            if (!l2.empty())
            {
                Vector2 s2 = MeasureTextEx(customFont, l2.c_str(), 16.0f, 1.0f);
                DrawTextEx(customFont, l2.c_str(), {GetScreenWidth() / 2.0f - s2.x / 2.0f, GetScreenHeight() / 2.0f + 130.0f}, 16.0f, 1.0f, WHITE);
            }
        }

        if (!spawned)
        {
            const char* loadingText = "Caricamento del mondo...";
            Vector2 ts = MeasureTextEx(customFont, loadingText, 24.0f, 1.0f);
            DrawTextEx(customFont, loadingText,
                       {GetScreenWidth() / 2.0f - ts.x / 2.0f, GetScreenHeight() / 2.0f + 60.0f},
                       24.0f, 1.0f, WHITE);
        }

        EndDrawing();
    }

    for (auto& kv : chunks)
    {
        FreeChunkMeshes(*kv.second);
        delete kv.second;
    }
    chunks.clear();

    UnloadFont(customFont);
    UnloadModel(bodyModel);
    UnloadModel(headModel);
    UnloadTexture(playerSkinTexture);
    UnloadMesh(sandCubeMesh);
    CloseClientNetwork();
    CloseWindow();
    return 0;
}