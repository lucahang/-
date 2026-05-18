#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <iostream>
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <string>

#include "ClientNetwork.h"
#include "Protocol.h"
#include "ClientConfig.h"
#include "CommandCollector.h"
#include "SafeQueue.h"
#include "VideoDecoderGPU.h"
#include "GpuRenderer.h"
#include "BandwidthEstimator.h"

#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "SDL3.lib")

// ============================================================
// 全局状态
// ============================================================
std::shared_ptr<ClientNetwork>  g_net;
std::unique_ptr<GpuDecoder>     g_decoder;
std::unique_ptr<GpuRenderer>    g_renderer;
std::atomic<bool>               g_running(true);
SafeQueue<std::vector<uint8_t>> g_videoQueue;

//static const int SERVER_W = 1920;
//static const int SERVER_H = 1080;
static const int WINDOW_W = 1600;
static const int WINDOW_H = 900;

// ============================================================
// 工具：SDL3 窗口 → HWND
// ============================================================
static HWND GetHWND(SDL_Window* window) {
    HWND hwnd = (HWND)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr);
    if (!hwnd)
        printf("[Init] 获取 HWND 失败: %s\n", SDL_GetError());
    return hwnd;
}

// ============================================================
// 工具：选 NVIDIA 显卡
// ============================================================
static IDXGIAdapter* FindNvidiaAdapter() {
    ComPtr<IDXGIFactory> factory;
    CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)factory.GetAddressOf());
    if (!factory) return nullptr;

    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        wprintf(L"[Init] 显卡: %s\n", desc.Description);
        if (std::wstring(desc.Description).find(L"NVIDIA") != std::wstring::npos) {
            printf("[Init] 使用 NVIDIA 独显\n");
            return adapter;
        }
        adapter->Release();
    }
    printf("[Init] 未找到 NVIDIA，使用默认设备\n");
    return nullptr;
}

static bool CreateD3D11Device(IDXGIAdapter* adapter,
    ID3D11Device** ppDev, ID3D11DeviceContext** ppCtx)
{
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        adapter,
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, lvls, 2, D3D11_SDK_VERSION,
        ppDev, nullptr, ppCtx);
    if (FAILED(hr)) { printf("[Init] D3D11CreateDevice 失败: 0x%08X\n", hr); return false; }
    printf("[Init] D3D11 设备创建成功\n");
    return true;
}

// ============================================================
// 网络接收线程
// ============================================================
static void NetworkReceiver(std::shared_ptr<ClientNetwork> net,
    BandwidthEstimator* estimator) {
    while (g_running.load()) {
        std::vector<uint8_t> buf;
        if (net->ReceiveVideoFrameUDP(buf)) {
            estimator->OnBytesReceived(static_cast<int>(buf.size()));
            g_videoQueue.push(std::move(buf));
        }
    }
}

// ============================================================
// SDL3 输入事件 → CommandCollector
// SDL3 key 字段路径变化：
//   keysym.sym  → e.key.key
//   keysym.scancode → e.key.scancode
// ============================================================
static void HandleSDLInput(const SDL_Event& e,
    CommandCollector* collector,
    SDL_Window* window)
{
    if (!collector) return;

    int cw, ch;
    SDL_GetWindowSize(window, &cw, &ch);

    switch (e.type) {

        // ---- 鼠标移动 ----
    case SDL_EVENT_MOUSE_MOTION:
        collector->OnMouseMsg(WM_MOUSEMOVE,
            (int)e.motion.x, (int)e.motion.y,
            cw, ch, SERVER_W, SERVER_H);
        break;

        // ---- 鼠标按键 ----
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        UINT msg = 0;
        switch (e.button.button) {
        case SDL_BUTTON_LEFT:
            msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
        case SDL_BUTTON_RIGHT:
            msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
        case SDL_BUTTON_MIDDLE:
            msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
        default: break;
        }
        if (msg)
            collector->OnMouseMsg(msg,
                (int)e.button.x, (int)e.button.y,
                cw, ch, SERVER_W, SERVER_H);
        break;
    }

                                  // ---- 滚轮 ----
                                  // SDL3 wheel.y 是 float，正=向上，与 Windows 一致
    case SDL_EVENT_MOUSE_WHEEL: {
        SHORT delta = (SHORT)((int)e.wheel.y * WHEEL_DELTA);
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        collector->OnMouseMsg(WM_MOUSEWHEEL,
            (int)mx, (int)my,
            cw, ch, SERVER_W, SERVER_H, delta);
        break;
    }

                              // ---- 键盘 ----
                              // SDL3: e.key.key (SDL_Keycode), e.key.scancode (SDL_Scancode)
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        SDL_Keycode sym = e.key.key;        // SDL3 新字段
        SDL_Scancode scan = e.key.scancode;   // SDL3 新字段

        // SDL3 repeat 字段：true 表示长按自动重复，按需过滤
        // if (e.key.repeat) break;

        WORD vk = 0;

        // 字母键
        if (sym >= SDLK_A && sym <= SDLK_Z) {
            vk = (WORD)sym;  // SDL3 字母 keycode 直接就是大写 ASCII
        }
        // 主键盘数字
        else if (sym >= SDLK_0 && sym <= SDLK_9) {
            vk = (WORD)sym;
        }
        else {
            // 用 scancode 转 Windows VK（SDL3 scancode 与 HID 一致）
            vk = (WORD)MapVirtualKey((UINT)scan, MAPVK_VSC_TO_VK);

            // 特殊键精确映射
            switch (sym) {
            case SDLK_RETURN:       vk = VK_RETURN;    break;
            case SDLK_ESCAPE:       vk = VK_ESCAPE;    break;
            case SDLK_BACKSPACE:    vk = VK_BACK;      break;
            case SDLK_TAB:          vk = VK_TAB;       break;
            case SDLK_SPACE:        vk = VK_SPACE;     break;
            case SDLK_LSHIFT:       vk = VK_LSHIFT;    break;
            case SDLK_RSHIFT:       vk = VK_RSHIFT;    break;
            case SDLK_LCTRL:        vk = VK_LCONTROL;  break;
            case SDLK_RCTRL:        vk = VK_RCONTROL;  break;
            case SDLK_LALT:         vk = VK_LMENU;     break;
            case SDLK_RALT:         vk = VK_RMENU;     break;
            case SDLK_LGUI:         vk = VK_LWIN;      break;
            case SDLK_RGUI:         vk = VK_RWIN;      break;
            case SDLK_DELETE:       vk = VK_DELETE;    break;
            case SDLK_INSERT:       vk = VK_INSERT;    break;
            case SDLK_HOME:         vk = VK_HOME;      break;
            case SDLK_END:          vk = VK_END;       break;
            case SDLK_PAGEUP:       vk = VK_PRIOR;     break;
            case SDLK_PAGEDOWN:     vk = VK_NEXT;      break;
            case SDLK_UP:           vk = VK_UP;        break;
            case SDLK_DOWN:         vk = VK_DOWN;      break;
            case SDLK_LEFT:         vk = VK_LEFT;      break;
            case SDLK_RIGHT:        vk = VK_RIGHT;     break;
            case SDLK_F1:           vk = VK_F1;        break;
            case SDLK_F2:           vk = VK_F2;        break;
            case SDLK_F3:           vk = VK_F3;        break;
            case SDLK_F4:           vk = VK_F4;        break;
            case SDLK_F5:           vk = VK_F5;        break;
            case SDLK_F6:           vk = VK_F6;        break;
            case SDLK_F7:           vk = VK_F7;        break;
            case SDLK_F8:           vk = VK_F8;        break;
            case SDLK_F9:           vk = VK_F9;        break;
            case SDLK_F10:          vk = VK_F10;       break;
            case SDLK_F11:          vk = VK_F11;       break;
            case SDLK_F12:          vk = VK_F12;       break;
            case SDLK_CAPSLOCK:     vk = VK_CAPITAL;   break;
            case SDLK_NUMLOCKCLEAR: vk = VK_NUMLOCK;   break;
            case SDLK_PRINTSCREEN:  vk = VK_SNAPSHOT;  break;
            case SDLK_PAUSE:        vk = VK_PAUSE;     break;
            case SDLK_KP_0:         vk = VK_NUMPAD0;   break;
            case SDLK_KP_1:         vk = VK_NUMPAD1;   break;
            case SDLK_KP_2:         vk = VK_NUMPAD2;   break;
            case SDLK_KP_3:         vk = VK_NUMPAD3;   break;
            case SDLK_KP_4:         vk = VK_NUMPAD4;   break;
            case SDLK_KP_5:         vk = VK_NUMPAD5;   break;
            case SDLK_KP_6:         vk = VK_NUMPAD6;   break;
            case SDLK_KP_7:         vk = VK_NUMPAD7;   break;
            case SDLK_KP_8:         vk = VK_NUMPAD8;   break;
            case SDLK_KP_9:         vk = VK_NUMPAD9;   break;
            case SDLK_KP_ENTER:     vk = VK_RETURN;    break;
            case SDLK_KP_PLUS:      vk = VK_ADD;       break;
            case SDLK_KP_MINUS:     vk = VK_SUBTRACT;  break;
            case SDLK_KP_MULTIPLY:  vk = VK_MULTIPLY;  break;
            case SDLK_KP_DIVIDE:    vk = VK_DIVIDE;    break;
            case SDLK_KP_PERIOD:    vk = VK_DECIMAL;   break;
                // 符号键
            case SDLK_MINUS:        vk = VK_OEM_MINUS; break;
            case SDLK_EQUALS:       vk = VK_OEM_PLUS;  break;
            case SDLK_LEFTBRACKET:  vk = VK_OEM_4;     break;
            case SDLK_RIGHTBRACKET: vk = VK_OEM_6;     break;
            case SDLK_BACKSLASH:    vk = VK_OEM_5;     break;
            case SDLK_SEMICOLON:    vk = VK_OEM_1;     break;
            case SDLK_APOSTROPHE:   vk = VK_OEM_7;     break;
            case SDLK_GRAVE:        vk = VK_OEM_3;     break;
            case SDLK_COMMA:        vk = VK_OEM_COMMA; break;
            case SDLK_PERIOD:       vk = VK_OEM_PERIOD;break;
            case SDLK_SLASH:        vk = VK_OEM_2;     break;
            default: break;
            }
        }

        if (vk != 0) {
            bool isDown = (e.type == SDL_EVENT_KEY_DOWN);
            LPARAM lp = (LPARAM)((UINT)scan << 16);
            if (!isDown) lp |= (1 << 30) | (1 << 31);
            collector->OnKeyMsg(
                isDown ? WM_KEYDOWN : WM_KEYUP,
                (WPARAM)vk, lp);
        }
        break;
    }

    default: break;
    }
}

// ============================================================
// 主函数（SDL3 要求签名为 int main(int, char**)）
// ============================================================
int main(int argc, char* argv[]) {
    ClientConfig::SERVER_IP = argv[1];
    std::cout << "SERVER_IP:" << ClientConfig::SERVER_PORT << std::endl;
    ClientConfig::SERVER_PORT = std::stoi(argv[2]);
    std::cout << "SERVER PORT:" << ClientConfig::SERVER_PORT << std::endl;

    // 必须在 SDL_Init 之前调用，告诉 Windows 本程序自己处理 DPI
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ----------------------------------------------------------
    // 1. 初始化 SDL3
    // ----------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) {   // SDL3 返回 bool
        printf("[Init] SDL_Init 失败: %s\n", SDL_GetError());
        return -1;
    }

    // SDL3 CreateWindow 新签名：title, w, h, flags
    SDL_Window* window = SDL_CreateWindow(
        "GPU 远控客户端",
        WINDOW_W, WINDOW_H,
        SDL_WINDOW_RESIZABLE);
    if (!window) {
        printf("[Init] SDL_CreateWindow 失败: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }

    HWND hwnd = GetHWND(window);
    if (!hwnd) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // ----------------------------------------------------------
    // 2. 创建 D3D11 设备
    // ----------------------------------------------------------
    IDXGIAdapter* pAdapter = FindNvidiaAdapter();
    ID3D11Device* d3dDev = nullptr;
    ID3D11DeviceContext* d3dCtx = nullptr;
    if (!CreateD3D11Device(pAdapter, &d3dDev, &d3dCtx)) {
        if (pAdapter) pAdapter->Release();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    if (pAdapter) pAdapter->Release();

    // ----------------------------------------------------------
    // 3. 初始化 GPU 解码器 & 渲染器
    // ----------------------------------------------------------
    g_decoder = std::make_unique<GpuDecoder>();
    g_renderer = std::make_unique<GpuRenderer>();

    // 获取实际渲染像素尺寸（非逻辑尺寸）
    int renderW, renderH;
    SDL_GetWindowSizeInPixels(window, &renderW, &renderH);
    printf("[Init] 逻辑尺寸: %dx%d  实际像素: %dx%d\n",
        WINDOW_W, WINDOW_H, renderW, renderH);


    if (!g_decoder->Init(d3dDev, WINDOW_W, WINDOW_H)) {
        printf("GPU 解码器初始化失败\n"); return -1;
    }
    if (!g_renderer->Init(hwnd, WINDOW_W, WINDOW_H, d3dDev)) {
        printf("GPU 渲染器初始化失败\n"); return -1;
    }
    d3dCtx->Release();
    d3dDev->Release();

    // ----------------------------------------------------------
    // 4. 主循环（含断线重连）
    // ----------------------------------------------------------
    int retryDelay = 3;

    while (g_running.load()) {
        printf("\n[Client] ===== 连接 %s:%d =====\n",
            ClientConfig::SERVER_IP, ClientConfig::SERVER_PORT);

        g_net = std::make_shared<ClientNetwork>();

        if (!g_net->TCPConnectToServer(ClientConfig::SERVER_IP.c_str(),
            ClientConfig::SERVER_PORT)) {
            printf("[Client] TCP 失败，%d 秒后重试\n", retryDelay);
            Uint64 waitUntil = SDL_GetTicks() + retryDelay * 1000;
            while (SDL_GetTicks() < waitUntil && g_running.load()) {
                SDL_Event e;
                while (SDL_PollEvent(&e))
                    if (e.type == SDL_EVENT_QUIT) g_running = false;
                SDL_Delay(50);
            }
            retryDelay = (std::min)(retryDelay * 2, 30);
            if (!g_running.load()) break;
            continue;
        }
        printf("[Client] TCP 成功\n");

        std::thread hb(&ClientNetwork::HeartBeatPacket, g_net, std::ref(g_running));
        hb.detach();

        if (!g_net->UDPConnectToServer(ClientConfig::SERVER_IP.c_str(),
            ClientConfig::SERVER_PORT)) {
            printf("[Client] UDP 失败，%d 秒后重试\n", retryDelay);
            g_running = false;
            SDL_Delay(retryDelay * 1000);
            retryDelay = (std::min)(retryDelay * 2, 30);
            g_running = true;
            g_net.reset();
            continue;
        }
        printf("[Client] UDP 成功，开始接收视频\n");
        retryDelay = 3;

        auto collector = std::make_unique<CommandCollector>(g_net);
        collector->Start(hwnd);

        BandwidthEstimator estimator([&](int bitrate) {
            g_net->SendBitrateRequest(bitrate);
            printf("[Bitrate] set to %d bps\n", bitrate);
        });
        estimator.Start();

        std::thread netThr(NetworkReceiver, g_net, &estimator);

        // -------------------------------------------------------
        // 5. 渲染 + 事件循环
        // -------------------------------------------------------
        int  pktCnt = 0, frameCnt = 0;
        auto lastLog = std::chrono::steady_clock::now();

        while (g_running.load()) {

            // SDL3 事件泵
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) {
                    g_running = false;
                    break;
                }
                HandleSDLInput(e, collector.get(), window);
            }
            if (!g_running.load()) break;

            // FPS 日志
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(
                now - lastLog).count() >= 1) {
                printf("[FPS] 收包 %d/s  渲染 %d/s  队列 %zu\n",
                    pktCnt, frameCnt, g_videoQueue.size());
                pktCnt = frameCnt = 0;
                lastLog = now;
            }

            // 取最新帧
            std::vector<uint8_t> h264;
            while (g_videoQueue.size() > 1) g_videoQueue.pop(h264);
            if (!g_videoQueue.pop(h264)) {
                SDL_Delay(1);
                continue;
            }
            pktCnt++;

            // GPU 解码
            ID3D11Texture2D* nv12 = nullptr;
            UINT             slice = 0;
            if (!g_decoder->Decode(h264.data(), h264.size(), &nv12, &slice)) {
                printf("[Decoder] 解码错误，包大小 %zu\n", h264.size());
                continue;
            }
            if (!nv12) continue;

            // GPU 渲染
            g_renderer->Render(nv12, slice);
            nv12->Release();
            frameCnt++;
        }

        // -------------------------------------------------------
        // 6. 断线清理
        // -------------------------------------------------------
        printf("[Client] 断线，清理...\n");
        g_running = false;

        if (netThr.joinable()) netThr.join();
        estimator.Stop();
        g_videoQueue.clear();
        collector.reset();
        g_net.reset();

        if (!g_running.load()) break;

        g_running = true;
        printf("[Client] %d 秒后重连\n", retryDelay);
        Uint64 waitUntil = SDL_GetTicks() + retryDelay * 1000;
        while (SDL_GetTicks() < waitUntil && g_running.load()) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_EVENT_QUIT) g_running = false;
            SDL_Delay(50);
        }
        retryDelay = (std::min)(retryDelay * 2, 30);
    }

    // ----------------------------------------------------------
    // 7. 最终清理
    // ----------------------------------------------------------
    g_renderer->Cleanup();
    g_decoder->Cleanup();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}