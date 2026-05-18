#pragma once
#include "NetHeader.h"
#include <thread>
#include <atomic>
#include <memory>
#include <cstdint>

class ClientNetwork;

// 服务端分辨率（鼠标 Hook 坐标映射用）
static const int SERVER_W = 1920;
static const int SERVER_H = 1080;

class CommandCollector {
public:
    explicit CommandCollector(std::shared_ptr<ClientNetwork> net);
    ~CommandCollector();

    void Start(HWND hwnd);

    void OnMouseMsg(UINT msg,
        int clientX, int clientY,
        int clientW, int clientH,
        int serverW, int serverH,
        SHORT wheelDelta = 0);

    void OnKeyMsg(UINT msg, WPARAM wParam, LPARAM lParam);

    CommandCollector(const CommandCollector&) = delete;
    CommandCollector& operator=(const CommandCollector&) = delete;

private:
    void HookLoop();
    void OnKeyAction(uint32_t vk, uint32_t scan, bool isDown, bool isExt);

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam); // 新增

    std::shared_ptr<ClientNetwork> g_net;
    static CommandCollector* s_instance;

    HHOOK             hKeyHook = nullptr;
    HHOOK             hMouseHook = nullptr;  // 新增
    HWND              hTargetWnd = nullptr;
    std::atomic<bool> running{ false };
    std::thread       hookThread;
    DWORD             hookThreadId = 0;
};