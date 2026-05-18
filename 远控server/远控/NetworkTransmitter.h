#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <vector>
#include "Protocol.h" // 引入你的协议头文件

#pragma comment(lib, "ws2_32.lib")

class NetworkTransmitter {
public:
    NetworkTransmitter();
    ~NetworkTransmitter();
    void SendLargeFrame(const std::vector<uint8_t>& frameData, uint32_t frameId);
    bool StartServer(int port);
    bool IsConnected(std::atomic<std::chrono::steady_clock::time_point>&lastPingTime);
    // 发送视频包（带协议头）
    bool SendVideoPacket(const std::vector<unsigned char>& h264Data);

    // 检查并提取一条远端指令（非阻塞）
    bool GetNextEvent(ControlEvent& outEvent);
    void CloseConnection();
    bool SendAll(const char* data, int len);
private:
    bool HasData(); // 探测是否有数据可读
    bool ReadExactly(char* buf, int len); // 解决 TCP 粘包/半包的核心函数
    void CheckUdpHandshake(); // 【新增】检查 UDP 握手
    SOCKET m_listenSocket;
    SOCKET m_clientSocket;
    
    // 【新增】UDP 相关成员
    SOCKET m_udpSocket;
    sockaddr_in m_clientUdpAddr;
    bool m_udpReady = false;
    int timeoutCount = 0;
};

