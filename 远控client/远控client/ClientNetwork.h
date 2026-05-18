#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <map>
#include <queue>
#include <mutex>
#include "Protocol.h"

class ClientNetwork {
public:
    ClientNetwork();
    ~ClientNetwork();
    bool TCPConnectToServer(const char* ip, int port);
    bool UDPConnectToServer(const char* ip, int port);
    void ProcessNetworkData();
    // ���� TCP ����ָ��
    void SendCommand(uint32_t eventType, int x, int y, uint32_t key = 0, PacketType packetType = PacketType::PKT_MOUSE);
    bool ReadExactly(char* buf, int len);
    void HeartBeatPacket(std::atomic<bool>& running);
    // �����������ղ���װ UDP ��Ƶ֡�����������������̵߳��ã�
    bool ReceiveVideoFrameUDP(std::vector<unsigned char>& outFrame);
    bool ReceiveVideoFrameUDPGPU(std::vector<uint8_t>& outFrame);
    void CloseConnection();
    void SendBitrateRequest(int bitrate_bps);
private:
    std::mutex m_queueMtx;
    std::queue<std::vector<uint8_t>> m_videoQueue;
    // ֡ ID -> (����� -> ����)
    std::map<uint32_t, std::map<uint16_t, std::vector<uint8_t>>> m_frameAssembler;
    uint32_t m_lastFrameId = 0; // ���ڶ�����ʱ�ľ�֡

    bool SendAll(const char* data, int len);

    bool m_udpReady = false;
    SOCKET m_socket;      // TCP Socket
    SOCKET m_udpSocket;   // UDP Socket
    std::mutex m_sendMutex;
    uint32_t m_latestFrameId = 0;
    std::map<uint16_t, std::vector<uint8_t>> m_fragments;
    std::chrono::steady_clock::time_point m_lastFragTime;
    std::mutex m_netMutex; // �������
};