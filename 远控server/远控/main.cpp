#include <iostream>
#include <thread>
#include <chrono>


// 各模块组件的模块
#include "Config.h"
#include "ScreenCapturer.h"
#include "VideoEncoderGPU.h"
#include "NetworkTransmitter.h"
#include "InputController.h"
#include "Protocol.h"

ScreenCapturer capturer;
GpuEncoder encoder;
NetworkTransmitter network;
InputController input;
ControlEvent cmd;
int width = -1;
int height = -1;
std::atomic<bool> connected{ true };
std::atomic<std::chrono::steady_clock::time_point> lastPingTime{
    std::chrono::steady_clock::now()
};
void NetworkCMDRecv() {
    while (connected.load()) {
        while (network.GetNextEvent(cmd)) {
            std::cout << "size of cmd:" << sizeof(cmd) << std::endl;
            if (cmd.type == (uint32_t)PacketType::PKT_MOUSE) {
                std::cout << "Mouse event: x=" << cmd.x << " y=" << cmd.y << " eventType=" << cmd.eventType << " key=" << cmd.key << std::endl;
                switch (cmd.eventType) {
                case 1: // 移动
                    input.MoveMouseTo(cmd.x, cmd.y, width, height);
                    break;
                case 2: // 左键按下
                    input.LeftClickDown();
                    break;
                case 3: // 左键抬起
                    input.LeftClickUp();
                    break;
                case 4: // 右键按下
                    input.RightClickDown();
                    break;
                case 5: // 右键抬起
                    input.RightClickUp();
                    break;
                case 6: // 中键按下
                    input.MiddleClickDown();
                    break;
                case 7: // 中键抬起
                    input.MiddleClickUp();
                    break;
                case 8: // 滚轮
                    // key 字段复用传 delta，客户端在 SendCommand 时将 SHORT 转 uint32_t
                    input.ScrollWheel((SHORT)(uint16_t)cmd.key);
                    break;
                default:
                    break;
                }
            }
            else if (cmd.type == (uint32_t)PacketType::PKT_KEYBOARD) {
                std::cout << "Keyboard event: key=" << cmd.key << " eventType=" << cmd.eventType << std::endl;
                // eventType: 1=按下, 0=抬起
                input.PressKey((WORD)cmd.key, cmd.eventType == 1);
            }
            else if (cmd.type == (uint32_t)PacketType::PKT_PING) {
                lastPingTime.store(std::chrono::steady_clock::now());
            }
            else if (cmd.type == (uint32_t)PacketType::PKT_BITRATE) {
                int newBitrate = (int)cmd.key;
                if (newBitrate > 0) {
                    std::cout << "[Server] Client requested bitrate: " << newBitrate << " bps" << std::endl;
                    encoder.SetBitrate(newBitrate);
                }
            }
        }

        // 避免空转占满 CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
//using cpu for video encode
//int main() {
//    if (!capturer.Init()) {
//        std::cout << "屏幕采集初始化失败！" << std::endl;
//        return -1;
//    }
//
//    // 获取屏幕信息
//    CaptureInfo screenInfo;
//    unsigned char* testPixels = nullptr;
//    while (!testPixels) {
//        testPixels = capturer.CaptureNextFrame(screenInfo);
//        if (testPixels) {
//            capturer.ReleaseFrame();
//            break;
//        }
//    }
//    if (width == -1)
//        width = screenInfo.width;
//    if (height == -1)
//        height = screenInfo.height;
//    encoder.Init(screenInfo.width, screenInfo.height, Config::TARGET_FPS);
//
//    std::cout << "[Server] 初始化完成，等待连接..." << std::endl;
//    
//    // ========= 外层循环，支持断线重连 =========
//    while (true) {
//
//        if (!network.StartServer(Config::SERVER_PORT)) {
//            std::cout << "[Server] 启动失败，3秒后重试..." << std::endl;
//            std::this_thread::sleep_for(std::chrono::seconds(3));
//            continue;
//        }
//        connected = true;
//        std::cout << "[Server] 客户端已连接" << std::endl;
//        // 启动网络接收线程
//        std::thread netThread(NetworkCMDRecv);
//        // ========= 内层循环，处理当前连接 =========
//
//        const int target_interval = 1000 / Config::TARGET_FPS;
//        while (connected.load()) {
//            //auto t1 = std::chrono::steady_clock::now();
//
//            static int frameCount = 0;
//            frameCount++;
//            static auto lastLog = std::chrono::steady_clock::now();
//            auto start = std::chrono::steady_clock::now();
//            if (std::chrono::duration_cast<std::chrono::seconds>(start - lastLog).count() >= 1) {
//                std::cout << "Real Server FPS: " << frameCount << std::endl;
//                frameCount = 0;
//                lastLog = start;
//            }
//            // ====== 1. 采集 + 编码 + 发送 ======
//            CaptureInfo frameInfo;
//            unsigned char* pixels = capturer.CaptureNextFrame(frameInfo);
//            //auto t2 = std::chrono::steady_clock::now();
//            if (pixels) {
//                std::vector<unsigned char> packet =
//                    encoder.EncodeFrame(pixels, frameInfo.rowPitch);
//                //auto t3 = std::chrono::steady_clock::now();
//
//                capturer.ReleaseFrame();
//                if (!packet.empty()) {
//                    if (!network.SendVideoPacket(packet)) {
//                        std::cout << "[Server] 发送失败，客户端断开" << std::endl;
//                        connected = false;
//                    }
//                }
//                /*auto t4 = std::chrono::steady_clock::now();
//                int ms_capture = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
//                int ms_encode = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
//                int ms_send = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3).count();*/
//
//                //std::cout << ms_capture << ' ' << ms_encode << ' ' << ms_send << std::endl;
//            }
//
//
//
//            // ====== 2. 网络连接状态超时检测 ======
//            if (!network.IsConnected(lastPingTime)) {
//                std::cout << "[Server] 检测到连接断开" << std::endl;
//                connected = false;
//            }
//            // 控制帧率，避免100% CPU占用
//            //auto end = std::chrono::steady_clock::now();
//            //int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
//
//            /*int sleepTime = Config::WAIT_TIMEOUT_MS - elapsed;
//            if (sleepTime > 0) {
//                std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
//            }*/
//
//            // ====== 4. 频率控制 (精确应对 Sleep) ======
//            auto work_done = std::chrono::steady_clock::now();
//            int work_time = std::chrono::duration_cast<std::chrono::milliseconds>(work_done - start).count();
//
//            // 如果处理一帧的时间已经超过目标间隔，就不要 sleep 了，继续处理下一帧
//            if (work_time < target_interval) {
//                std::this_thread::sleep_for(std::chrono::milliseconds(target_interval - work_time));
//            }
//
//            // ====== 5. 网络及频率检测 (优化版本) ======
//            // 每隔 1 秒检测一次网络状态即可，不需要每帧都检测
//            static auto last_check_time = std::chrono::steady_clock::now();
//            auto now = std::chrono::steady_clock::now();
//            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_check_time).count() >= 1) {
//                if (!network.IsConnected(lastPingTime)) {
//                    std::cout << "[Server] 检测到连接超时，关闭连接" << std::endl;
//                    connected = false;
//                }
//                last_check_time = now;
//            }
//
//        }
//
//        // ========= 连接断开后重新等待 =========
//        std::cout << "[Server] 客户端已断开，等待重连..." << std::endl;
//        connected = false;
//        if (netThread.joinable()) {
//            netThread.join();
//            std::cout << "netThread join" << std::endl;
//        }
//
//        std::this_thread::sleep_for(std::chrono::seconds(2));
//    }
//    
//
//    return 0;
//}

//using gpu for video encode
int main(int argc, char* argv[]) {

    Config::SERVER_PORT = std::stoi(argv[1]);
    std::cout << "SERVER PORT:" << Config::SERVER_PORT << std::endl;

    SetProcessDPIAware();
    if (!capturer.Init()) {
        std::cout << "屏幕采集初始化失败！" << std::endl;
        return -1;
    }

    // 获取屏幕信息
    CaptureInfo screenInfo;
    unsigned char* frameOutput = nullptr;
    while (!frameOutput) {
        frameOutput = capturer.CaptureNextFrame(screenInfo);
        if (frameOutput) {
            capturer.ReleaseFrame();
            break;
        }
    }
    if (width == -1)
        width = screenInfo.width;
    if (height == -1)
        height = screenInfo.height;
    if (!encoder.Init(screenInfo.width, screenInfo.height, Config::TARGET_FPS)) {
        std::cout << "encoder.Init error" << std::endl;
    }

    std::cout << "[Server] 初始化完成，等待连接..." << std::endl;

    // ========= 外层循环，支持断线重连 =========
    while (true) {

        if (!network.StartServer(Config::SERVER_PORT)) {
            std::cout << "[Server] 启动失败，3秒后重试..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        connected = true;
        std::cout << "[Server] 客户端已连接" << std::endl;
        // 启动网络接收线程
        std::thread netThread(NetworkCMDRecv);
        // ========= 内层循环，处理当前连接 =========
        const int target_time = 1000 / Config::TARGET_FPS;
        // 1. 使用高精度等待定时器
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 是 Win10 以后支持的，优先级检查
        HANDLE hTimer = CreateWaitableTimerEx(NULL, NULL,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        // 设置 16.6ms 周期触发 (单位为 100 纳秒，负数表示相对时间)
        LARGE_INTEGER liDueTime;
        liDueTime.QuadPart = -10000LL * target_time; // 16.6666 ms
        if (!SetWaitableTimer(hTimer, &liDueTime, target_time, NULL, NULL, 0)) {
            // 错误处理
            std::cerr << "SetWaitableTimer failed: " << GetLastError() << std::endl;
        }

        while (connected.load()) {
            WaitForSingleObject(hTimer, INFINITE);

            static int frameCount = 0;
            frameCount++;
            static auto lastLog = std::chrono::steady_clock::now();
            auto start = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(start - lastLog).count() >= 1) {
                std::cout << "Real Server FPS: " << frameCount << std::endl;
                frameCount = 0;
                lastLog = start;
            }

            //auto start = std::chrono::steady_clock::now();
            // ====== 1. 采集 + 编码 + 发送 ======
            CaptureInfo frameInfo;

            frameOutput = capturer.CaptureNextFrame(frameInfo);

            auto t2 = std::chrono::steady_clock::now();

            if (!frameOutput) {
                //std::cout << "frameOutput is empty" << std::endl;
            }
            else {
                //cpu copy once
                std::vector<uint8_t> packet =
                    encoder.Encode(frameOutput, frameInfo.rowPitch);
                auto t3 = std::chrono::steady_clock::now();

                if (!packet.empty()) {
                    //std::cout << "packet size: " << packet.size() << std::endl;
                    if (!network.SendVideoPacket(packet)) {
                        std::cout << "[Server] 发送失败，客户端断开" << std::endl;
                        connected = false;
                    }

                    uint8_t* p = packet.data();
                    /*printf("Head: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);*/
                }
                else {
                    std::cout << "encode error" << std::endl;
                }
                auto t4 = std::chrono::steady_clock::now();
                int CaptureNextFrameTime = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - start).count();
                int EncodeTime = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
                int SendVideoPacketTime = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3).count();
                //std::cout << "CaptureNextFrameTime:" << CaptureNextFrameTime << std::endl;
                //std::cout << "EncodeTime:" << EncodeTime << std::endl;
                //std::cout << "SendVideoPacketTime:" << SendVideoPacketTime << std::endl;


                capturer.ReleaseFrame();
            }
            // zero copy 
            /*
            // 2. 纹理可共享（获取纹理包装器）
            ID3D11Texture2D* nvTexture = nullptr;
            // 通过共享句柄跨设备获取，用于合成编码器的输入
            HRESULT hr = capturer.GetSharedTextureForDevice(encoder.GetDevice(), &nvTexture);

            if (SUCCEEDED(hr)) {
                std::vector<uint8_t> packet =
                    encoder.Encode(nvTexture);
                if (!packet.empty()) {
                    if (!network.SendVideoPacket(packet)) {
                        std::cout << "[Server] 发送失败，客户端断开" << std::endl;
                        connected = false;
                    }
                }
                capturer.ReleaseFrameGPU();
                GPUTexture->Release();
            }
            else if (FAILED(hr)) {
                std::cout << "GetSharedTextureForDevice error:"<<hr << std::endl;
            }*/

            // ====== 2. 网络连接状态超时检测 ======
            /*if (!network.IsConnected(lastPingTime)) {
                std::cout << "[Server] 检测到连接断开" << std::endl;
                connected = false;
            }*/

            // 控制帧率，避免100% CPU占用
            //auto end = std::chrono::steady_clock::now();
            //int workTime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            //std::cout << "workTime:" << workTime << std::endl;
           /* if (workTime < target_time) {
                std::this_thread::sleep_for(std::chrono::milliseconds(target_time-workTime));
            }*/
        }
        // 关闭定时器
        CloseHandle(hTimer);
        // ========= 连接断开后重新等待 =========
        std::cout << "[Server] 客户端已断开，等待重连..." << std::endl;
        connected = false;
        if (netThread.joinable()) {
            netThread.join();
            std::cout << "netThread join" << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }


    return 0;
}