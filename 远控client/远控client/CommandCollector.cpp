#include "NetHeader.h"
#include "CommandCollector.h"
#include "ClientNetwork.h"
#include "Protocol.h"
#include <iostream>

CommandCollector* CommandCollector::s_instance = nullptr;

CommandCollector::CommandCollector(std::shared_ptr<ClientNetwork> net)
    : g_net(std::move(net)) {
    s_instance = this;
}

CommandCollector::~CommandCollector() {
    running = false;

    // 先卸载 Hook（必须在线程消息泵还活着时卸载）
    if (hKeyHook) {
        UnhookWindowsHookEx(hKeyHook);
        hKeyHook = nullptr;
    }
    if (hMouseHook) {
        UnhookWindowsHookEx(hMouseHook);
        hMouseHook = nullptr;
    }

    // 发 WM_QUIT 让 HookLoop 的 GetMessage 退出
    if (hookThreadId != 0)
        PostThreadMessage(hookThreadId, WM_QUIT, 0, 0);
    if (hookThread.joinable())
        hookThread.join();

    if (s_instance == this)
        s_instance = nullptr;
}

void CommandCollector::Start(HWND hwnd) {
    hTargetWnd = hwnd;
    running = true;
    hookThread = std::thread(&CommandCollector::HookLoop, this);
}

// ---------------------------------------------------------------
// Hook 线程：同时装键盘 + 鼠标低级 Hook
// ---------------------------------------------------------------
void CommandCollector::HookLoop() {
    hookThreadId = GetCurrentThreadId();

    // 初始化线程消息队列
    MSG dummy{};
    PeekMessage(&dummy, NULL, WM_USER, WM_USER, PM_NOREMOVE);

    // 装键盘 Hook
    hKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
        GetModuleHandle(NULL), 0);
    if (!hKeyHook)
        std::cerr << "[CommandCollector] 键盘 Hook 失败，错误码: "
        << GetLastError() << "\n";
    else
        std::cout << "[CommandCollector] 键盘 Hook 安装成功\n";

    // 装鼠标 Hook
    hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc,
        GetModuleHandle(NULL), 0);
    if (!hMouseHook)
        std::cerr << "[CommandCollector] 鼠标 Hook 失败，错误码: "
        << GetLastError() << "\n";
    else
        std::cout << "[CommandCollector] 鼠标 Hook 安装成功\n";

    // 消息泵：Hook 依赖这个循环驱动
    MSG msg;
    while (running && GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 退出时卸载（double-free 安全，析构里已置 nullptr）
    if (hKeyHook) { UnhookWindowsHookEx(hKeyHook);   hKeyHook = nullptr; }
    if (hMouseHook) { UnhookWindowsHookEx(hMouseHook); hMouseHook = nullptr; }
}

// ---------------------------------------------------------------
// 键盘低级 Hook 回调
// ---------------------------------------------------------------
LRESULT CALLBACK CommandCollector::LowLevelKeyboardProc(int nCode,
    WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance) {
        auto* pKey = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // ✅ 过滤掉 SendInput / keybd_event 注入的合成事件
        if (pKey->flags & LLKHF_INJECTED)
            return CallNextHookEx(NULL, nCode, wParam, lParam);

        if (GetForegroundWindow() == s_instance->hTargetWnd) {
            bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            if (isDown || isUp) {
                bool isExt = (pKey->flags & LLKHF_EXTENDED) != 0;
                s_instance->OnKeyAction(pKey->vkCode, pKey->scanCode, isDown, isExt);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------------------------------------------------------
// 鼠标低级 Hook 回调
// ---------------------------------------------------------------
LRESULT CALLBACK CommandCollector::LowLevelMouseProc(int nCode,
    WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_instance) {
        if (GetForegroundWindow() == s_instance->hTargetWnd) {
            auto* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

            // 鼠标坐标是屏幕绝对坐标，转为客户区坐标
            POINT pt = pMouse->pt;
            ScreenToClient(s_instance->hTargetWnd, &pt);

            // 获取客户区尺寸
            RECT rc;
            GetClientRect(s_instance->hTargetWnd, &rc);
            int cw = rc.right - rc.left;
            int ch = rc.bottom - rc.top;

            // 只处理客户区内的事件（排除标题栏/边框）
            if (pt.x >= 0 && pt.x < cw && pt.y >= 0 && pt.y < ch) {
                SHORT wheelDelta = 0;
                if (wParam == WM_MOUSEWHEEL)
                    wheelDelta = static_cast<SHORT>(HIWORD(pMouse->mouseData));

                s_instance->OnMouseMsg(
                    static_cast<UINT>(wParam),
                    pt.x, pt.y, cw, ch,
                    SERVER_W, SERVER_H,
                    wheelDelta
                );
            }
        }
    }
    // 鼠标同样不拦截，让系统正常处理
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ---------------------------------------------------------------
// WndProc 兜底（Hook 失效时使用）
// ---------------------------------------------------------------
void CommandCollector::OnKeyMsg(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg != WM_KEYDOWN && msg != WM_KEYUP &&
        msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP)
        return;

    // Hook 正常工作时不重复发包
    if (hKeyHook) return;

    uint32_t vk = static_cast<uint32_t>(wParam);
    uint32_t scan = (static_cast<uint32_t>(lParam) >> 16) & 0xFF;
    bool     isExt = (lParam & (1 << 24)) != 0;
    bool     isDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);

    OnKeyAction(vk, scan, isDown, isExt);
}

void CommandCollector::OnKeyAction(uint32_t vk, uint32_t scan,
    bool isDown, bool isExt) {
    if (!g_net) return;

    printf("[Keyboard] VK: 0x%02X | Status: %s | Scan: %d | Ext: %s\n",
        vk,
        isDown ? "DOWN" : "UP  ",
        scan,
        isExt ? "Yes" : "No");

    try {
        g_net->SendCommand(isDown ? 1u : 0u, 0, 0, vk, PacketType::PKT_KEYBOARD);
    }
    catch (...) {}
}

// ---------------------------------------------------------------
// 鼠标消息处理（坐标映射 + 发包）
// ---------------------------------------------------------------
void CommandCollector::OnMouseMsg(UINT msg,
    int clientX, int clientY,
    int clientW, int clientH,
    int serverW, int serverH,
    SHORT wheelDelta) {
    if (!g_net) return;

    int sx = (clientW > 0) ? (clientX * serverW / clientW) : clientX;
    int sy = (clientH > 0) ? (clientY * serverH / clientH) : clientY;
    sx = max(0, min(sx, serverW - 1));
    sy = max(0, min(sy, serverH - 1));

    // ---- 添加日志输出 ----
    const char* msgName = "UNKNOWN";
    bool isMove = (msg == WM_MOUSEMOVE);

    switch (msg) {
    case WM_MOUSEMOVE:    msgName = "MOVE"; break;
    case WM_LBUTTONDOWN:  msgName = "L_DOWN"; break;
    case WM_LBUTTONUP:    msgName = "L_UP"; break;
    case WM_RBUTTONDOWN:  msgName = "R_DOWN"; break;
    case WM_RBUTTONUP:    msgName = "R_UP"; break;
    case WM_MBUTTONDOWN:  msgName = "M_DOWN"; break;
    case WM_MBUTTONUP:    msgName = "M_UP"; break;
    case WM_MOUSEWHEEL:   msgName = "WHEEL"; break;
    }

    if (!isMove) {
        printf("[Mouse] Event: %-7s | Local: (%4d, %4d) -> Server: (%4d, %4d) | Wheel: %d\n",
            msgName, clientX, clientY, sx, sy, wheelDelta);
    }
    else {
        // 如果需要调试移动轨迹，可以取消下面这行的注释
        // printf("[Mouse] Move: (%4d, %4d)\r", sx, sy); 
    }

    try {
        switch (msg) {
        case WM_MOUSEMOVE:    g_net->SendCommand(1, sx, sy, 0, PacketType::PKT_MOUSE); break;
        case WM_LBUTTONDOWN:  g_net->SendCommand(2, sx, sy, 0, PacketType::PKT_MOUSE); break;
        case WM_LBUTTONUP:    g_net->SendCommand(3, sx, sy, 0, PacketType::PKT_MOUSE); break;
        case WM_RBUTTONDOWN:  g_net->SendCommand(4, sx, sy, 0, PacketType::PKT_MOUSE); break;
        case WM_RBUTTONUP:    g_net->SendCommand(5, sx, sy, 0, PacketType::PKT_MOUSE); break;
        case WM_MBUTTONDOWN:  g_net->SendCommand(6, sx, sy, 0, PacketType::PKT_MOUSE); break;
        case WM_MBUTTONUP:    g_net->SendCommand(7, sx, sy, 0, PacketType::PKT_MOUSE); break;
        case WM_MOUSEWHEEL:
            g_net->SendCommand(8, sx, sy,
                static_cast<uint32_t>(wheelDelta), PacketType::PKT_MOUSE); break;
        default: break;
        }
    }
    catch (...) {}
}