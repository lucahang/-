#include "ClientNetwork.h"
#include <iostream>
#include <map>

ClientNetwork::ClientNetwork() : m_socket(INVALID_SOCKET), m_udpSocket(INVALID_SOCKET) {
    u_long mode = 1; // 1 为非阻塞
    ioctlsocket(m_udpSocket, FIONBIO, &mode);
    int recvBufSize = 10 * 1024 * 1024; // 10MB
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVBUF, (const char*)&recvBufSize, sizeof(recvBufSize));
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

ClientNetwork::~ClientNetwork() {
    CloseConnection();
    WSACleanup();
}

void ClientNetwork::ProcessNetworkData() {
    char recvBuf[2048]; // 足够装下 Header + 1400 数据
    int len = recvfrom(m_udpSocket, recvBuf, sizeof(recvBuf), 0, nullptr, nullptr);

    if (len < sizeof(VideoPacketHeader)) return;

    VideoPacketHeader* header = (VideoPacketHeader*)recvBuf;
    uint8_t* payload = (uint8_t*)(recvBuf + sizeof(VideoPacketHeader));

    // 1. 丢弃由于延迟产生的过时旧帧
    if (header->frameId < m_lastFrameId) return;

    // 2. 将零件放入对应的帧桶里
    auto& currentFrameMap = m_frameAssembler[header->frameId];
    currentFrameMap[header->packetIdx] = std::vector<uint8_t>(payload, payload + header->dataSize);

    // 3. 检查这帧的所有零件是否到齐
    if (currentFrameMap.size() == header->totalPackets) {
        // 到齐了！开始组装
        std::vector<uint8_t> completeFrame;
        for (uint16_t i = 0; i < header->totalPackets; i++) {
            completeFrame.insert(completeFrame.end(),
                currentFrameMap[i].begin(),
                currentFrameMap[i].end());
        }

        // 推入解码队列
        m_videoQueue.push(completeFrame);

        // 更新 ID 并清理旧缓存
        m_lastFrameId = header->frameId;
        m_frameAssembler.erase(header->frameId);

        // 可选：清理比当前 frameId 小的所有残留记录（防止内存泄漏）
        for (auto it = m_frameAssembler.begin(); it != m_frameAssembler.end(); ) {
            if (it->first < m_lastFrameId) it = m_frameAssembler.erase(it);
            else ++it;
        }
    }
}

//server的IP和port
bool ClientNetwork::TCPConnectToServer(const char* ip, int port) {
    // 1. 连接 TCP (控制通道)
    CloseConnection();
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
        == SOCKET_ERROR) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }
    return true;
}
bool ClientNetwork::UDPConnectToServer(const char* ip, int port){
    // 2. 【新增】初始化 UDP Socket 并发送握手包打洞
    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    // 增大 UDP 接收缓冲区，防止高清画面瞬间发包过多导致丢包
    int recvBuf = 1024 * 1024 * 4; // 4MB
    int timeout = 100; // ms
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVTIMEO, 
        (const char*)&timeout, sizeof(timeout));
    setsockopt(m_udpSocket, SOL_SOCKET, SO_RCVBUF,
        (const char*)&recvBuf, sizeof(recvBuf));
    sockaddr_in udpAddr{};
    udpAddr.sin_family = AF_INET;
    udpAddr.sin_port = htons(port + 1); // 服务端 UDP 端口
    inet_pton(AF_INET, ip, &udpAddr.sin_addr);

    // 发送魔数作为握手包
    uint32_t magic = PROTOCOL_MAGIC;
    for (int i = 1;i <=5 ;i++) {
        std::cout << "i:" << i << "st send :";
        Sleep(1000);
        int ret = sendto(m_udpSocket,
            (const char*)&magic,
            sizeof(magic),
            0,
            (sockaddr*)&udpAddr,
            sizeof(udpAddr));

        if (ret == SOCKET_ERROR) {
            printf("sendto failed: %d\n", WSAGetLastError());
        }
        else {
            printf("sendto success, bytes = %d\n", ret);
        }
        
    }
    return true;
}

void ClientNetwork::CloseConnection() {
    if (m_socket != INVALID_SOCKET) closesocket(m_socket);
    if (m_udpSocket != INVALID_SOCKET) closesocket(m_udpSocket);
}

void ClientNetwork::HeartBeatPacket(std::atomic<bool>& running) {
    while (running) {
        PacketHeader header{};
        header.magic = PROTOCOL_MAGIC;
        header.type = static_cast<uint32_t>(PacketType::PKT_PING);
        int r = send(m_socket, (char*)&header, sizeof(header), 0);
        std::cout << "m_socket:"<<m_socket << std::endl;
        if (r <= 0) {
            std::cout << "send fail, error=" << WSAGetLastError() << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// 1. 将 ID3D11Texture2D 换成 uint8_t，因为网络传输的是字节
bool ClientNetwork::ReceiveVideoFrameUDPGPU(std::vector<uint8_t>& outFrame) {
    // 1. 网络接收不需要加锁，recvfrom 本身是线程安全的系统调用
    char buf[2048]; // 够用就行，UDP MTU 通常 1500
    sockaddr_in fromAddr;
    int addrLen = sizeof(fromAddr);
    int r = recvfrom(m_udpSocket, buf, sizeof(buf), 0, (sockaddr*)&fromAddr, &addrLen);

    if (r <= (int)sizeof(UdpFragmentHeader)) return false;

    UdpFragmentHeader* hdr = (UdpFragmentHeader*)buf;
    if (hdr->magic != PROTOCOL_MAGIC) return false;

    // 2. 进入临界区，保护共享的数据结构
    std::lock_guard<std::mutex> lock(m_netMutex);

    auto now = std::chrono::steady_clock::now();

    // 丢弃过旧的帧数据
    if (hdr->frameId < m_latestFrameId) return false;

    // 如果是新的一帧，清理旧的残留分片
    if (hdr->frameId > m_latestFrameId) {
        m_fragments.clear();
        m_latestFrameId = hdr->frameId;
    }

    // 超时检测：如果上一片和这一片隔了太久，说明网络断层，清空
    if (!m_fragments.empty()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastFragTime).count() > 500) {
            m_fragments.clear();
        }
    }
    m_lastFragTime = now;

    // 存储分片
    const uint8_t* payloadStart = (const uint8_t*)buf + sizeof(UdpFragmentHeader);
    // 确保 payloadLen 合法，防止越界
    if (hdr->payloadLen > (r - sizeof(UdpFragmentHeader))) return false;

    m_fragments[hdr->fragIndex] = std::vector<uint8_t>(payloadStart, payloadStart + hdr->payloadLen);

    // 3. 严格拼包检查
    if (m_fragments.size() == hdr->fragCount) {
        // 关键：检查是否存在索引断层 (必须包含从 0 到 count-1 的所有分片)
        outFrame.clear();
        // 预分配内存提高 insert 效率
        outFrame.reserve(hdr->fragCount * UDP_MAX_PAYLOAD);

        for (int i = 0; i < hdr->fragCount; ++i) {
            auto it = m_fragments.find(i);
            if (it == m_fragments.end()) {
                // 只要缺了一片，这一帧就是坏的，不能返回 true
                // 注意：不要 clear，因为可能只是乱序，等剩下的片到齐
                return false;
            }
            outFrame.insert(outFrame.end(), it->second.begin(), it->second.end());
        }

        // 执行到这里说明整帧凑齐且连续
        m_fragments.clear(); // 拼完必须清空，迎接下一帧

        // 调试打印
        if (outFrame.size() >= 5) {
            printf("[Receive] Frame OK. ID: %u, Size: %zu, Head: %02x %02x %02x %02x %02x\n",
                hdr->frameId, outFrame.size(),
                outFrame[0], outFrame[1], outFrame[2], outFrame[3], outFrame[4]);
        }
        return true;
    }

    return false;
}


bool ClientNetwork::ReceiveVideoFrameUDP(std::vector<unsigned char>& outFrame) {
    static uint32_t latestFrameId = 0;
    //static std::map<uint16_t, std::vector<unsigned char>> fragments;
    static std::vector<uint8_t> frameBuffer(2 * 1024 * 1024);
    static std::vector<bool> receivedMap(2000, false);
    static int receivedCount = 0;
    static int totalExpected = 0;

    static auto lastTime = std::chrono::steady_clock::now();

    char buf[2000];
    int r = recvfrom(m_udpSocket, buf, sizeof(buf), 0, NULL, NULL);
    if (r <= 0) return false;

    UdpFragmentHeader* hdr = (UdpFragmentHeader*)buf;
    if (hdr->magic != PROTOCOL_MAGIC) return false;

    if (hdr->frameId > latestFrameId) {
        latestFrameId = hdr->frameId;
        receivedCount = 0;
        totalExpected = hdr->fragCount;
        std::fill(receivedMap.begin(), receivedMap.begin() + totalExpected + 1, false);
    }
    else if (hdr->frameId < latestFrameId) {
        return false; // 丢弃过时包
    }
    if (hdr->fragIndex >= 2000 || receivedMap[hdr->fragIndex]) return false;

    int offset = hdr->fragIndex * UDP_MAX_PAYLOAD;
    memcpy(frameBuffer.data() + offset, buf + sizeof(UdpFragmentHeader), hdr->payloadLen);
    receivedMap[hdr->fragIndex] = true;
    receivedCount++;
    if (receivedCount == hdr->fragCount) {
        outFrame.assign(frameBuffer.begin(), frameBuffer.begin() + (offset + hdr->payloadLen));
        return true;
    }
    auto now = std::chrono::steady_clock::now();

    lastTime = now;

    return false;
}
// ... SendCommand, ReadExactly 保持原样 ...

bool ClientNetwork::SendAll(const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(m_socket, data + sent, len - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}
//SendCommand(1, sx, sy, 0, PacketType::PKT_MOUSE)
void ClientNetwork::SendCommand(uint32_t eventType, int x, int y, 
        uint32_t key, PacketType packetType) {
    if (m_socket == INVALID_SOCKET) return;

    std::lock_guard<std::mutex> lock(m_sendMutex);

    PacketHeader header{};
    header.magic = PROTOCOL_MAGIC;
    header.type = static_cast<uint32_t>(packetType);
    header.length = sizeof(ControlEvent);

    ControlEvent ev{};
    ev.eventType = eventType;
    ev.x = x;
    ev.y = y;
    ev.key = key;
    ev.type = (uint32_t)packetType;
    if (!SendAll(reinterpret_cast<const char*>(&header), sizeof(header))) return;
    SendAll(reinterpret_cast<const char*>(&ev), sizeof(ev));
}

void ClientNetwork::SendBitrateRequest(int bitrate_bps) {
    if (m_socket == INVALID_SOCKET) return;
    std::lock_guard<std::mutex> lock(m_sendMutex);
    PacketHeader hdr{};
    hdr.magic   = PROTOCOL_MAGIC;
    hdr.type    = static_cast<uint32_t>(PacketType::PKT_BITRATE);
    hdr.length  = 0;
    hdr.bitrate = static_cast<uint32_t>(bitrate_bps);
    hdr.hz      = 0;
    SendAll(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
}

bool ClientNetwork::ReadExactly(char* buf, int len) {
    if (len <= 0 || len > static_cast<int>(MAX_PACKET_SIZE)) return false;
    if (m_udpSocket == INVALID_SOCKET) return false;

    int received = 0;
    while (received < len) {
        int r = recv(m_udpSocket, buf + received, len - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
}

