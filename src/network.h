#pragma once
#include "protocol.h"

// Callback chiamate da PollClientNetwork (tutte opzionali)
struct NetCallbacks
{
    void (*onChunk)(int cx, int cz, const uint8_t* blocks) = nullptr;
    void (*onBlock)(int x, int y, int z, uint8_t block) = nullptr;
    void (*onPlayerState)(uint8_t playerId, float x, float y, float z, float yaw, float pitch) = nullptr;
    void (*onWelcome)(uint8_t playerId, double worldTime) = nullptr;
    void (*onTimeSync)(double worldTime) = nullptr;
    void (*onItemState)(uint32_t id, uint8_t block, uint8_t count, float x, float y, float z) = nullptr;
    void (*onItemRemove)(uint32_t id, uint8_t pickerId, uint8_t block, uint8_t count) = nullptr;
};

bool InitClientNetwork(const char* ip, uint16_t port);
bool IsClientConnected();
void PollClientNetwork(const NetCallbacks& cb);
void SendPlayerStatePacket(float x, float y, float z, float yaw, float pitch);
void SendBlockChangePacket(int x, int y, int z, uint8_t block);
void SendChunkRequestPacket(int cx, int cz);
void SendDropRequestPacket(uint8_t block, uint8_t count);
void SendItemPickupPacket(uint32_t id);
void CloseClientNetwork();