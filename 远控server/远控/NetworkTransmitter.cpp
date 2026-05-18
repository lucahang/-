#define NOMINMAX
#include <Windows.h>
#include <thread>
#include <chrono>
#include "NetworkTransmitter.h"
#include <iostream>
#include <algorithm>

NetworkTransmitter::NetworkTransmitter()
    : m_listenSocket(INVALID_SOCKET), m_clientSocket(INVALID_SOCKET), m_udpSocket(INVALID_SOCKET) {
    
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

NetworkTransmitter::~NetworkTransmitter() {
    CloseConnection();
    if (m_listenSocket != INVALID_SOCKET) closesocket(m_listenSocket);
    WSACleanup();
}

bool NetworkTransmitter::StartServer(int port) {
    CloseConnection();

    // 1. 初始化 TCP 监听
    if (m_listenSocket == INVALID_SOCKET) {
        m_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
        if (listen(m_listenSocket, 1) == SOCKET_ERROR) return false;
    }

    // 2. 【新增】初始化 UDP 监听 (端口为 TCP端口 + 1)
    if (m_udpSocket == INVALID_SOCKET) {
        m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        

        sockaddr_in udpAddr{};
        udpAddr.sin_family = AF_INET;
        udpAddr.sin_port = htons(port + 1); // 例如 TCP 8808，UDP 就是 8809
        udpAddr.sin_addr.s_addr = INADDR_ANY;
        bind(m_udpSocket, (sockaddr*)&udpAddr, sizeof(udpAddr));
    }

    std::cout << "[Server] WAITING TCP CONNECTION..." << std::endl;
    m_clientSocket = accept(m_listenSocket, NULL, NULL);
    std::cout << "[Server] TCP CONNECTION FINISHED" << std::endl;
    m_udpReady = false; // 重置 UDP 状态
    // 设置 UDP 为阻塞模式，方便稍后探测握手包
    u_long mode = 0;
    ioctlsocket(m_udpSocket, FIONBIO, &mode);
    std::cout << "[Server] WAITING UDP CONNECTION..." << std::endl;
    CheckUdpHandshake();
    std::cout << "[Server] UDP CONNECTION FINISHED" << std::endl;
    // 设置 UDP 为非阻塞模式，方便稍后探测握手包
    mode = 1;
    ioctlsocket(m_udpSocket, FIONBIO, &mode);
    return m_clientSocket != INVALID_SOCKET&& m_udpReady;
}

// 【新增】检测客户端的 UDP 握手包，获取对方 UDP 的真实 IP 和端口
void NetworkTransmitter::CheckUdpHandshake() {
    if (m_udpReady) return;

    char buf[16];
    sockaddr_in senderAddr{};
    int senderLen = sizeof(senderAddr);

    // 因为设置了非阻塞，没有数据会立刻返回
    int r = recvfrom(m_udpSocket, buf, sizeof(buf), 0, (sockaddr*)&senderAddr, &senderLen);
    if (r == SOCKET_ERROR) {
        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK) {
            // 没数据，正常情况
            std::cout << "no data from m_udpSocket" << std::endl;
            return;
        }
        else {
            printf("recvfrom error: %d\n", err);
            if (err == WSAECONNRESET) {
                printf("[Server] client disconnected (UDP)\n");

                m_udpReady = false;   // ⭐关键
                return;
            }
        }
    }

    if (r >= 4) {
        uint32_t magic = *(uint32_t*)buf;
        if (magic == PROTOCOL_MAGIC) {
            m_clientUdpAddr = senderAddr;
            m_udpReady = true;
            std::cout << "[Server] 接收到客户端 UDP 握手，视频推流通道建立！" << std::endl;
        }
    }
}

bool NetworkTransmitter::SendVideoPacket(const std::vector<unsigned char>& h264Data) {
    if (m_clientSocket == INVALID_SOCKET || h264Data.empty() || !m_udpReady) return false;

    static uint32_t s_frameId = 0;
    s_frameId++;

    const int totalLen = (int)h264Data.size();
    const int fragCount = (totalLen + UDP_MAX_PAYLOAD - 1) / UDP_MAX_PAYLOAD;

    // 1. 使用固定大小的安全缓冲区，避免越界
    uint8_t sendBuf[1500];
    // 强制获取固定的 Header 大小，不要依赖 sizeof(header) 变量名，防止编译器差异
    const size_t headSize = sizeof(UdpFragmentHeader);

    for (int i = 0; i < fragCount; ++i) {
        int offset = i * UDP_MAX_PAYLOAD;
        int payloadLen = std::min(UDP_MAX_PAYLOAD, totalLen - offset);

        UdpFragmentHeader header;
        memset(&header, 0, headSize); // 必须清零，防止随机值干扰
        header.magic = PROTOCOL_MAGIC;
        header.frameId = s_frameId;
        header.fragIndex = (uint16_t)i;
        header.fragCount = (uint16_t)fragCount;
        header.payloadLen = (uint16_t)payloadLen;
        // 如果是 I 帧（67或65开头），标记一下，方便 Client 统计
        header.isKeyFrame = (h264Data[0] == 0 && h264Data[1] == 0 && h264Data[2] == 0 && h264Data[3] == 1 && (h264Data[4] & 0x1F) == 7) ? 1 : 0;

        // 2. 严格搬运数据
        memcpy(sendBuf, &header, headSize);
        memcpy(sendBuf + headSize, h264Data.data() + offset, payloadLen);

        int sent = sendto(m_udpSocket, (const char*)sendBuf, (int)(headSize + payloadLen), 0,
            (sockaddr*)&m_clientUdpAddr, sizeof(m_clientUdpAddr));

        if (sent <= 0) {
            // UDP 缓冲区可能满了
            std::this_thread::yield();
        }

        // 3. Pacing 策略：根据包大小动态调整
        // 177KB 的包如果 200us 发一个，对某些网络还是太快，尝试 300us
       // std::this_thread::sleep_for(std::chrono::microseconds(300));
    }

    return true;
}

void NetworkTransmitter::CloseConnection() {
    if (m_clientSocket != INVALID_SOCKET) {
        closesocket(m_clientSocket);
        m_clientSocket = INVALID_SOCKET;
    }
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }
    memset(&m_clientUdpAddr, 0, sizeof(m_clientUdpAddr));
    m_udpReady = false;
}

// ... GetNextEvent, IsConnected, ReadExactly, SendAll 保持你原本的 TCP 逻辑不变 ...
// (只需要确保 TCP 专门负责鼠标和键盘指令)

bool NetworkTransmitter::ReadExactly(char* buf, int len) {
    int received = 0;
    while (received < len) {
        int r = recv(m_clientSocket, buf + received, len - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}

bool NetworkTransmitter::HasData() {
    if (m_clientSocket == INVALID_SOCKET) return false;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_clientSocket, &fds);

    timeval tv = { 0, 0 };

    return select(0, &fds, NULL, NULL, &tv) > 0;
}
bool NetworkTransmitter::GetNextEvent(ControlEvent& outEvent) {
    if (!HasData()) return false;

    PacketHeader header{};
    if (!ReadExactly((char*)&header, sizeof(header))) return false;
    if (header.magic != PROTOCOL_MAGIC) return false;
    //std::cout << header.type << std::endl;
    if (header.type == (uint32_t)PacketType::PKT_PING) {
        outEvent.type = header.type;
        return true;
    }
    if (header.type == (uint32_t)PacketType::PKT_BITRATE) {
        outEvent.type = header.type;
        outEvent.key = header.bitrate;
        return true;
    }
    if (header.length <= 0 || header.length > MAX_PACKET_SIZE) {
        return false;
    }
    // 🚀 保存类型
    outEvent.type = header.type;

    // 🚀 统一处理：鼠标 + 键盘都读取
    if (header.type == (uint32_t)PacketType::PKT_MOUSE ||
        header.type == (uint32_t)PacketType::PKT_KEYBOARD) {

        if (header.length != sizeof(ControlEvent)) {
            // 长度异常，丢弃
            std::vector<char> tmp(header.length);
            ReadExactly(tmp.data(), header.length);
            return false;
        }

        ControlEvent temp;
        if (!ReadExactly((char*)&temp, sizeof(ControlEvent))) return false;

        temp.type = header.type;
        outEvent = temp;
        return true;
    }

    // 🚀 其他未知数据 → 丢弃
    if (header.length > 0 && header.length < MAX_PACKET_SIZE) {
        std::vector<char> tmp(header.length);
        ReadExactly(tmp.data(), header.length);
    }

    return false;
}

bool NetworkTransmitter::IsConnected(
    std::atomic<std::chrono::steady_clock::time_point>& lastPingTime)
{
    if (m_clientSocket == INVALID_SOCKET) return false;


    auto now = std::chrono::steady_clock::now();
    auto last = lastPingTime.load();

    if (now - last > std::chrono::seconds(6)) {
       /* timeoutCount++;

        if (timeoutCount == 1) {
            std::cout << "heartbeat delay..." << std::endl;
        }

        if (timeoutCount >= 3) {
            std::cout << "client timeout" << std::endl;
            return false;
        }*/
        std::cout << "client timeout" << std::endl;
        return false;
    }
   /* else {
        timeoutCount = 0;
    }*/

    return true;
}

bool NetworkTransmitter::SendAll(const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(m_clientSocket, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

