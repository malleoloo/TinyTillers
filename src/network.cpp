#include <enet/enet.h>
#include <cstring>
#include <iostream>
#include "network.h"

static ENetHost* client = nullptr;
static ENetPeer* peer = nullptr;
static bool connected = false;

bool InitClientNetwork(const char* ip, uint16_t port)
{
    if (enet_initialize() != 0) return false;

    client = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!client) {
        enet_deinitialize();
        return false;
    }

    ENetAddress address;
    enet_address_set_host(&address, ip);
    address.port = port;

    peer = enet_host_connect(client, &address, 2, 0);
    return peer != nullptr;
}

// Meta' del ping: compensa il ritardo con cui arriva l'ora del server
static double OneWayLatencySeconds()
{
    return peer ? (double)peer->roundTripTime * 0.0005 : 0.0;
}

bool IsClientConnected()
{
    return connected;
}

void PollClientNetwork(const NetCallbacks& cb)
{
    if (!client) return;

    static uint8_t chunkBuffer[CHUNK_VOLUME];

    ENetEvent event;
    while (enet_host_service(client, &event, 0) > 0)
    {
        if (event.type == ENET_EVENT_TYPE_CONNECT)
        {
            connected = true;
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            connected = false;
        }
        else if (event.type == ENET_EVENT_TYPE_RECEIVE)
        {
            uint8_t* data = event.packet->data;
            size_t len = event.packet->dataLength;

            if (len > 0)
            {
                uint8_t type = data[0];

                if (type == MSG_CHUNK_DATA && len > sizeof(ChunkDataHeader))
                {
                    ChunkDataHeader header;
                    std::memcpy(&header, data, sizeof(header));
                    if (RleDecode(data + sizeof(header), len - sizeof(header), chunkBuffer, CHUNK_VOLUME))
                    {
                        if (cb.onChunk) cb.onChunk(header.cx, header.cz, chunkBuffer);
                    }
                }
                else if (type == MSG_BLOCK_CHANGE && len == sizeof(BlockChangeMessage))
                {
                    BlockChangeMessage msg;
                    std::memcpy(&msg, data, sizeof(msg));
                    if (cb.onBlock) cb.onBlock(msg.x, msg.y, msg.z, msg.block);
                }
                else if (type == MSG_PLAYER_STATE && len == sizeof(PlayerStateMessage))
                {
                    PlayerStateMessage pmsg;
                    std::memcpy(&pmsg, data, sizeof(pmsg));
                    if (cb.onPlayerState) cb.onPlayerState(pmsg.playerId, pmsg.x, pmsg.y, pmsg.z, pmsg.yaw, pmsg.pitch);
                }
                else if (type == MSG_WELCOME && len == sizeof(WelcomeMessage))
                {
                    WelcomeMessage msg;
                    std::memcpy(&msg, data, sizeof(msg));
                    if (cb.onWelcome) cb.onWelcome(msg.playerId, msg.worldTime + OneWayLatencySeconds());
                }
                else if (type == MSG_TIME_SYNC && len == sizeof(TimeSyncMessage))
                {
                    TimeSyncMessage msg;
                    std::memcpy(&msg, data, sizeof(msg));
                    if (cb.onTimeSync) cb.onTimeSync(msg.worldTime + OneWayLatencySeconds());
                }
                else if (type == MSG_ITEM_STATE && len == sizeof(ItemStateMessage))
                {
                    ItemStateMessage msg;
                    std::memcpy(&msg, data, sizeof(msg));
                    if (cb.onItemState) cb.onItemState(msg.id, msg.block, msg.count, msg.x, msg.y, msg.z);
                }
                else if (type == MSG_ITEM_REMOVE && len == sizeof(ItemRemoveMessage))
                {
                    ItemRemoveMessage msg;
                    std::memcpy(&msg, data, sizeof(msg));
                    if (cb.onItemRemove) cb.onItemRemove(msg.id, msg.pickerId, msg.block, msg.count);
                }
            }
            enet_packet_destroy(event.packet);
        }
    }
}

void SendPlayerStatePacket(float x, float y, float z, float yaw, float pitch)
{
    if (!peer || !connected) return;
    PlayerStateMessage pmsg { MSG_PLAYER_STATE, 0, x, y, z, yaw, pitch };
    ENetPacket* pp = enet_packet_create(&pmsg, sizeof(pmsg), 0);
    if (pp && enet_peer_send(peer, 1, pp) != 0) enet_packet_destroy(pp);
}

void SendBlockChangePacket(int x, int y, int z, uint8_t block)
{
    if (!peer || !connected) return;
    BlockChangeMessage bmsg { MSG_BLOCK_CHANGE, x, y, z, block };
    ENetPacket* bp = enet_packet_create(&bmsg, sizeof(bmsg), ENET_PACKET_FLAG_RELIABLE);
    if (bp && enet_peer_send(peer, 0, bp) != 0) enet_packet_destroy(bp);
}

void SendChunkRequestPacket(int cx, int cz)
{
    if (!peer || !connected) return;
    ChunkRequestMessage rmsg { MSG_CHUNK_REQUEST, cx, cz };
    ENetPacket* rp = enet_packet_create(&rmsg, sizeof(rmsg), ENET_PACKET_FLAG_RELIABLE);
    if (rp && enet_peer_send(peer, 0, rp) != 0) enet_packet_destroy(rp);
}

void SendDropRequestPacket(uint8_t block, uint8_t count)
{
    if (!peer || !connected) return;
    DropRequestMessage msg { MSG_DROP_REQUEST, block, count };
    ENetPacket* p = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
    if (p && enet_peer_send(peer, 0, p) != 0) enet_packet_destroy(p);
}

void SendItemPickupPacket(uint32_t id)
{
    if (!peer || !connected) return;
    ItemPickupMessage msg { MSG_ITEM_PICKUP, id };
    ENetPacket* p = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
    if (p && enet_peer_send(peer, 0, p) != 0) enet_packet_destroy(p);
}

void CloseClientNetwork()
{
    if (peer) enet_peer_disconnect(peer, 0);
    if (client) enet_host_destroy(client);
    enet_deinitialize();
}