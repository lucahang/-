#pragma once
#include <cstdint>

const uint32_t PROTOCOL_MAGIC = 0x52445058;
const uint32_t MAX_PACKET_SIZE = 5 * 1024 * 1024;
const int UDP_MAX_PAYLOAD = 1200;

#pragma pack(push, 1)

// 1. TCP 基础包头（所有指令的领路人）
struct PacketHeader {
    uint32_t magic;
    uint32_t type;      // PacketType
    uint32_t length;    // 后续数据(如 ControlEvent)的长度
    uint32_t bitrate;   // 预留给视频流动态调速
    uint32_t hz;        // 预留给音频或帧率控制
};

// 2. 控制事件（鼠标键盘）
struct ControlEvent {
    uint32_t type;      // 冗余一份 PacketType，方便处理
    uint32_t eventType; // 具体动作：L_DOWN, MOVE 等
    int32_t x;
    int32_t y;
    uint32_t key;
};

// 3. UDP 视频分片包头（用于高速图像传输）
struct UdpFragmentHeader {
    uint32_t magic;
    uint32_t frameId;
    uint16_t fragIndex;
    uint16_t fragCount;
    uint8_t  isKeyFrame; // 改为 8 位，更标准
    uint16_t payloadLen;
};

// 4. 视频编码相关头（如果不用 UDP 也可以复用）
struct VideoPacketHeader {
    uint32_t frameId;
    uint16_t packetIdx;
    uint16_t totalPackets;
    uint32_t dataSize;
    uint8_t  isLast;     // 改为 8 位，更安全
};

#pragma pack(pop)

enum class PacketType : uint32_t {
    PKT_VIDEO = 1,
    PKT_MOUSE = 2,
    PKT_KEYBOARD = 3,
    PKT_PING = 4,
    PKT_PONG = 5,
    PKT_BITRATE = 6
};